/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight streaming session - see hmrdp_moonlight_session.h.
 */
#include "hmrdp_moonlight_session.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "hmrdp_log.h"

#include <Limelight.h>

#include "hmrdp_moonlight.h"

namespace hmrdp {
namespace {

std::mutex g_mutex;
// Guards the session thread's wait for stop/termination.
std::mutex g_cvMutex;
std::condition_variable g_cv;
std::thread g_thread;
std::atomic<bool> g_active{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_terminated{false};
std::atomic<bool> g_stopRequested{false};
MoonlightSessionCallback g_callback = nullptr;
std::atomic<MoonlightCursorCallback> g_cursorCallback{nullptr};
// The window the decode bridge renders into; owned by the ArkTS page, only
// borrowed for the life of the stream.
MoonlightVideoContext g_context;

void Emit(MoonlightSessionEvent event, int value, const char* text) {
  const MoonlightSessionCallback callback = g_callback;
  if (callback != nullptr) {
    callback(event, value, text != nullptr ? text : "");
  }
}

void OnConnectionStarted() {
  HMRDP_LOGI("moonlight session: connected");
  g_connected = true;
  Emit(MoonlightSessionEvent::kStarted, 0, "");
}

void OnConnectionTerminated(int errorCode) {
  HMRDP_LOGW("moonlight session: terminated (%{public}d)", errorCode);
  g_connected = false;
  {
    // Set the flag and notify under the same mutex the session thread waits on,
    // so the wakeup cannot be lost between its predicate check and its wait.
    std::lock_guard<std::mutex> lock(g_cvMutex);
    g_terminated = true;
    g_cv.notify_all();
  }
  Emit(MoonlightSessionEvent::kTerminated, errorCode, "");
}

void OnStageStarting(int stage) {
  HMRDP_LOGI("moonlight session: stage start %{public}s", LiGetStageName(stage));
  Emit(MoonlightSessionEvent::kStageStarting, stage, LiGetStageName(stage));
}

void OnStageComplete(int stage) {
  HMRDP_LOGI("moonlight session: stage done %{public}s", LiGetStageName(stage));
  Emit(MoonlightSessionEvent::kStageComplete, stage, LiGetStageName(stage));
}

void OnStageFailed(int stage, int errorCode) {
  HMRDP_LOGE("moonlight session: stage failed %{public}s (%{public}d)",
             LiGetStageName(stage), errorCode);
  Emit(MoonlightSessionEvent::kStageFailed, errorCode, LiGetStageName(stage));
}

// Runs on the control-stream thread: forward the host pointer to the registered
// cursor listener (the N-API bridge hops it to the JS thread).
void OnCursorUpdate(bool visible, bool hasShape, uint32_t shapeId, uint16_t width,
                    uint16_t height, int16_t hotspotX, int16_t hotspotY,
                    const uint8_t* bgra, uint32_t size) {
  const MoonlightCursorCallback callback = g_cursorCallback.load();
  if (callback == nullptr) {
    return;
  }
  MoonlightCursorShape shape;
  shape.visible = visible;
  shape.hasShape = hasShape;
  shape.shapeId = shapeId;
  shape.width = width;
  shape.height = height;
  shape.hotspotX = hotspotX;
  shape.hotspotY = hotspotY;
  shape.bgra = bgra;
  shape.size = size;
  callback(shape);
}

// Runs on the session thread: fills the config structs, calls LiStartConnection
// (which returns once the streams are established, not when the session ends),
// waits for stop/termination, then completes the lifecycle with LiStopConnection
// on this same thread.
void Run(const MoonlightStreamConfig& config) {
  SERVER_INFORMATION serverInfo;
  LiInitializeServerInformation(&serverInfo);
  serverInfo.address = config.host.c_str();
  serverInfo.serverInfoAppVersion = config.serverInfoAppVersion.c_str();
  serverInfo.serverInfoGfeVersion = config.serverInfoGfeVersion.c_str();
  serverInfo.rtspSessionUrl = config.sessionUrl.c_str();
  serverInfo.serverCodecModeSupport = config.serverCodecModeSupport;

  STREAM_CONFIGURATION stream;
  LiInitializeStreamConfiguration(&stream);
  stream.width = config.width;
  stream.height = config.height;
  stream.fps = config.fps;
  stream.bitrate = config.bitrateKbps;
  stream.packetSize = config.packetSize;
  stream.streamingRemotely = config.streamingRemotely ? STREAM_CFG_REMOTE : STREAM_CFG_LOCAL;
  stream.audioConfiguration = config.audioConfiguration;
  stream.supportedVideoFormats = config.supportedVideoFormats;
  stream.clientRefreshRateX100 = config.clientRefreshRateX100;
  stream.colorSpace = config.colorSpace;
  stream.colorRange = config.colorRange;
  stream.encryptionFlags = config.encryptionFlags;
  if (config.remoteInputAesKey.size() == 16) {
    std::memcpy(stream.remoteInputAesKey, config.remoteInputAesKey.data(), 16);
  }
  if (config.remoteInputAesIv.size() == 16) {
    std::memcpy(stream.remoteInputAesIv, config.remoteInputAesIv.data(), 16);
  }

  CONNECTION_LISTENER_CALLBACKS listeners;
  LiInitializeConnectionCallbacks(&listeners);
  listeners.stageStarting = OnStageStarting;
  listeners.stageComplete = OnStageComplete;
  listeners.stageFailed = OnStageFailed;
  listeners.connectionStarted = OnConnectionStarted;
  listeners.connectionTerminated = OnConnectionTerminated;
  listeners.cursorUpdate = OnCursorUpdate;

  PDECODER_RENDERER_CALLBACKS video =
      static_cast<PDECODER_RENDERER_CALLBACKS>(MoonlightVideoCallbacks());

  // Audio is not wired yet: a null renderer disables the audio stream.
  HMRDP_LOGI("moonlight session: LiStartConnection enter");
  const int result = LiStartConnection(&serverInfo, &stream, &listeners, video, nullptr,
                                       &g_context, 0, nullptr, 0);
  HMRDP_LOGI("moonlight session: LiStartConnection returned %{public}d", result);
  if (result == 0) {
    // Local cursor mode: ask the host to stop compositing the pointer into the
    // video and forward it on the cursor channel instead. A no-op unless the
    // host advertised the capability (foundation-sunshine) and the control
    // stream is encrypted v2.
    if (config.localCursorMode) {
      LiSetLocalCursorMode(true);
    }
    // Established: the streams now run on moonlight's own threads. LiStartConnection
    // does NOT block until the session ends - the session lives until we call
    // LiStopConnection (which must run on this thread) or the host ends it. Wait
    // here so Start/Stop stay paired on one thread and a second Start cannot run
    // while this session is still alive.
    {
      std::unique_lock<std::mutex> lock(g_cvMutex);
      g_cv.wait(lock, []() {
        return g_stopRequested.load() || g_terminated.load();
      });
    }
    LiStopConnection();
    HMRDP_LOGI("moonlight session: LiStopConnection done");
  } else if (!g_stopRequested && !g_terminated) {
    // A start failure that did not come through stageFailed.
    HMRDP_LOGW("moonlight session: LiStartConnection failed (%{public}d)", result);
    Emit(MoonlightSessionEvent::kTerminated, result, "");
  }
  g_connected = false;
  g_active = false;
}

}  // namespace

void MoonlightSessionSetWindow(void* window) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_context.window = window;
}

void MoonlightSessionSetCursorCallback(MoonlightCursorCallback callback) {
  g_cursorCallback.store(callback);
}

std::string MoonlightSessionStart(const MoonlightStreamConfig& config,
                                  MoonlightSessionCallback callback) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_active) {
    return "busy";
  }
  // A session that ended on its own leaves its thread joinable; assigning over a
  // joinable std::thread calls std::terminate, so reap it first.
  if (g_thread.joinable()) {
    g_thread.join();
  }
  if (config.host.empty() || config.remoteInputAesKey.size() != 16
      || config.remoteInputAesIv.size() != 16) {
    return "config";
  }
  g_callback = callback;
  g_stopRequested = false;
  g_connected = false;
  g_terminated = false;
  g_active = true;
  try {
    g_thread = std::thread([config]() {
      Run(config);
    });
  } catch (...) {
    g_active = false;
    g_callback = nullptr;
    return "thread";
  }
  HMRDP_LOGI("moonlight session: start %{public}s %{public}dx%{public}d @%{public}d",
             config.host.c_str(), config.width, config.height, config.fps);
  return std::string();
}

void MoonlightSessionStop() {
  std::thread thread;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_active) {
      // Break a pending LiStartConnection; a no-op once established (the session
      // thread then runs LiStopConnection itself).
      LiInterruptConnection();
    }
    thread = std::move(g_thread);
  }
  {
    std::lock_guard<std::mutex> lock(g_cvMutex);
    g_stopRequested = true;
    g_cv.notify_all();
  }
  if (thread.joinable()) {
    thread.join();
  }
  g_active = false;
  g_callback = nullptr;
  HMRDP_LOGI("moonlight session: stopped");
}

bool MoonlightSessionActive() {
  return g_active;
}

std::string MoonlightLaunchQueryParameters() {
  const char* extra = LiGetLaunchUrlQueryParameters();
  return extra != nullptr ? std::string(extra) : std::string();
}

void MoonlightSessionMouseMove(int deltaX, int deltaY) {
  if (g_connected) {
    LiSendMouseMoveEvent(static_cast<short>(deltaX), static_cast<short>(deltaY));
  }
}

void MoonlightSessionMousePosition(int x, int y, int referenceWidth, int referenceHeight) {
  if (g_connected) {
    LiSendMousePositionEvent(static_cast<short>(x), static_cast<short>(y),
                             static_cast<short>(referenceWidth), static_cast<short>(referenceHeight));
  }
}

void MoonlightSessionMouseButton(int action, int button) {
  if (g_connected) {
    LiSendMouseButtonEvent(static_cast<char>(action), button);
  }
}

void MoonlightSessionKey(int keyCode, int action, int modifiers) {
  if (g_connected) {
    LiSendKeyboardEvent(static_cast<short>(keyCode), static_cast<char>(action),
                        static_cast<char>(modifiers));
  }
}

void MoonlightSessionScroll(int clicks) {
  if (g_connected) {
    LiSendScrollEvent(static_cast<signed char>(clicks));
  }
}

}  // namespace hmrdp
