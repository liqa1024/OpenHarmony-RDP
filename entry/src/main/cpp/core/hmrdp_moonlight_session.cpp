/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight streaming session - see hmrdp_moonlight_session.h.
 */
#include "hmrdp_moonlight_session.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "hmrdp_log.h"

#ifdef HMRDP_HAVE_MOONLIGHT

#include <Limelight.h>

#include "hmrdp_moonlight.h"

namespace hmrdp {
namespace {

std::mutex g_mutex;
std::thread g_thread;
std::atomic<bool> g_active{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_terminated{false};
std::atomic<bool> g_stopRequested{false};
MoonlightSessionCallback g_callback = nullptr;
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
  g_connected = true;
  Emit(MoonlightSessionEvent::kStarted, 0, "");
}

void OnConnectionTerminated(int errorCode) {
  g_terminated = true;
  Emit(MoonlightSessionEvent::kTerminated, errorCode, "");
}

void OnStageStarting(int stage) {
  Emit(MoonlightSessionEvent::kStageStarting, stage, LiGetStageName(stage));
}

void OnStageComplete(int stage) {
  Emit(MoonlightSessionEvent::kStageComplete, stage, LiGetStageName(stage));
}

void OnStageFailed(int stage, int errorCode) {
  Emit(MoonlightSessionEvent::kStageFailed, errorCode, LiGetStageName(stage));
}

// Runs on the session thread: fills the config structs, then blocks in
// LiStartConnection until the stream ends (or the start fails).
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

  PDECODER_RENDERER_CALLBACKS video =
      static_cast<PDECODER_RENDERER_CALLBACKS>(MoonlightVideoCallbacks());

  // Audio is not wired yet: a null renderer disables the audio stream.
  const int result = LiStartConnection(&serverInfo, &stream, &listeners, video, nullptr,
                                       &g_context, 0, nullptr, 0);
  if (result != 0 && !g_stopRequested && !g_terminated) {
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

std::string MoonlightSessionStart(const MoonlightStreamConfig& config,
                                  MoonlightSessionCallback callback) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_active) {
    return "busy";
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
  HMRDP_LOGI("moonlight session: start %{public}s %dx%d @%{public}d", config.host.c_str(),
             config.width, config.height, config.fps);
  return std::string();
}

void MoonlightSessionStop() {
  std::thread thread;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_active) {
      return;
    }
    g_stopRequested = true;
    // Break a pending LiStartConnection; a no-op once connected.
    LiInterruptConnection();
    thread = std::move(g_thread);
  }
  if (thread.joinable()) {
    thread.join();
  }
  if (g_connected) {
    LiStopConnection();
    g_connected = false;
  }
  g_active = false;
  g_callback = nullptr;
  HMRDP_LOGI("moonlight session: stopped");
}

bool MoonlightSessionActive() {
  return g_active;
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

#else  // !HMRDP_HAVE_MOONLIGHT

namespace hmrdp {

void MoonlightSessionSetWindow(void*) {}
std::string MoonlightSessionStart(const MoonlightStreamConfig&, MoonlightSessionCallback) {
  return "no-moonlight";
}
void MoonlightSessionStop() {}
bool MoonlightSessionActive() {
  return false;
}
void MoonlightSessionMouseMove(int, int) {}
void MoonlightSessionMousePosition(int, int, int, int) {}
void MoonlightSessionMouseButton(int, int) {}
void MoonlightSessionKey(int, int, int) {}
void MoonlightSessionScroll(int) {}

}  // namespace hmrdp

#endif
