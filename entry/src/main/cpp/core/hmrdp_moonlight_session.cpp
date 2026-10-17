#include "hmrdp_moonlight_session.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "hmrdp_log.h"

#include <Limelight.h>

#include "hmrdp_moonlight.h"
#include "hmrdp_moonlight_audio.h"
#include "hmrdp_presenter.h"

namespace hmrdp {
namespace {

// Must match SS_TOUCHPAD_FRAME_MAX_CONTACTS in the patched moonlight-common-c.
constexpr int kMaxTouchpadContacts = 5;

std::mutex g_mutex;
std::mutex g_cvMutex;
std::condition_variable g_cv;
std::thread g_thread;
std::atomic<bool> g_active{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_terminated{false};
std::atomic<bool> g_stopRequested{false};
MoonlightSessionCallback g_callback = nullptr;
std::atomic<MoonlightCursorCallback> g_cursorCallback{nullptr};
std::atomic<MoonlightClipboardCallback> g_clipboardCallback{nullptr};
// The cursor shape arrives in stream pixels; when the presenter upscales locally, the same ratio
// must be applied to the shape (RDP reports the same value with its pointer shape).
std::atomic<int> g_cursorMagnificationMilli{1000};
// Window the decode bridge renders into; owned by the ArkTS page, borrowed here.
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
    // so the wakeup cannot be lost.
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

// Runs on the control-stream thread; the N-API bridge hops it to the JS thread.
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
  shape.magnificationMilli = g_cursorMagnificationMilli.load();
  shape.width = width;
  shape.height = height;
  shape.hotspotX = hotspotX;
  shape.hotspotY = hotspotY;
  shape.bgra = bgra;
  shape.size = size;
  callback(shape);
}

// Control-stream thread; the N-API bridge copies the frame and hops it to JS.
void OnClipboardData(const char* data, int length) {
  const MoonlightClipboardCallback callback = g_clipboardCallback.load();
  if (callback == nullptr || data == nullptr || length <= 0) {
    return;
  }
  callback(reinterpret_cast<const uint8_t*>(data), length);
}

// LiStartConnection returns once the streams are established, not when the
// session ends.
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
  // 超分: the caller resolved the session size (output / upscale ratio) and the presenter upscales
  // every frame back to the output; without it the stream *is* the output. Mirrors the RDP session's
  // /w /h route.
  int sessionWidth = config.streamWidth > 0 ? config.streamWidth : config.width;
  int sessionHeight = config.streamHeight > 0 ? config.streamHeight : config.height;
  MoonlightVideoContext context;
  {
    // The ArkUI thread may rebind the surface while this thread is starting; take one snapshot.
    std::lock_guard<std::mutex> lock(g_mutex);
    context = g_context;
  }
  const bool srRequested = sessionWidth != config.width || sessionHeight != config.height;
  // The presenter lives in this scope for the whole connection: the bridge borrows it from here and
  // must have given it back (MoonlightVideoDetachPresenter) before the scope ends.
  std::unique_ptr<FramePresenter> presenter;
  bool srAttached = false;
  if (srRequested && context.window != nullptr) {
    std::unique_ptr<FramePresenter> created = CreateFramePresenter();
    // A GLES presenter or a device without the video-import extension must not put the decoder into
    // buffer mode: PresentVideo would refuse every frame and the window would stay black.
    if (created != nullptr && created->supportsVideo()) {
      const int surfaceWidth = context.width > 0 ? context.width : config.width;
      const int surfaceHeight = context.height > 0 ? context.height : config.height;
      created->SetSurface(context.window, surfaceWidth, surfaceHeight);
      if (created->Prepare()) {
        presenter = std::move(created);
        presenter->SetSuperResolution(true, static_cast<SuperResolutionBackend>(config.srBackend),
                                      config.width, config.height);
        srAttached = true;
      } else {
        HMRDP_LOGW("moonlight session: presenter prepare failed; using the output size in surface mode");
      }
    } else if (created != nullptr) {
      HMRDP_LOGW("moonlight session: presenter cannot import decoded buffers; using the output size "
                 "in surface mode");
    }
  }
  if (srRequested && !srAttached) {
    // Keep a visible picture: the decoder renders straight into the window at the output size. The
    // host stream is still requested at the session size, so the host upscales instead of this device.
    sessionWidth = config.width;
    sessionHeight = config.height;
  }
  // The cursor is forwarded in stream pixels; apply the same ratio the picture gets. A fallback to
  // the output-size surface path is 1:1 and leaves the default 1000.
  int cursorMagnificationMilli = 1000;
  if (srAttached && sessionWidth > 0) {
    const int ratio = static_cast<int>(
        std::lround(1000.0 * static_cast<double>(config.width) / static_cast<double>(sessionWidth)));
    if (ratio > cursorMagnificationMilli) {
      cursorMagnificationMilli = ratio;
    }
  }
  g_cursorMagnificationMilli.store(cursorMagnificationMilli);
  MoonlightVideoSetSuperResolutionRequested(srRequested);
  MoonlightVideoAttachPresenter(srAttached ? presenter.get() : nullptr);
  if (srRequested) {
    HMRDP_LOGI("moonlight session %{public}dx%{public}d (super resolution %{public}s -> "
               "%{public}dx%{public}d, attached=%{public}d)",
               config.width, config.height,
               config.srBackend == static_cast<int>(SuperResolutionBackend::kFsr) ? "fsr"
                                                                                 : "xengine",
               sessionWidth, sessionHeight, srAttached ? 1 : 0);
  }
  stream.width = sessionWidth;
  stream.height = sessionHeight;
  stream.fps = config.fps;
  stream.bitrate = config.bitrateKbps;
  stream.packetSize = config.packetSize;
  stream.streamingRemotely = config.streamingRemotely;
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
  listeners.clipboardData = OnClipboardData;

  PDECODER_RENDERER_CALLBACKS video =
      static_cast<PDECODER_RENDERER_CALLBACKS>(MoonlightVideoCallbacks());
  MoonlightAudioSetLowLatency(config.enableAudio && config.lowLatencyAudio);
  // A null audio renderer leaves the library's audio callbacks as no-ops (fake
  // renderer); the ring then never receives a format, so nothing plays.
  PAUDIO_RENDERER_CALLBACKS audio =
      config.enableAudio ? static_cast<PAUDIO_RENDERER_CALLBACKS>(MoonlightAudioCallbacks())
                         : nullptr;

  HMRDP_LOGI("moonlight session: LiStartConnection enter");
  const int result = LiStartConnection(&serverInfo, &stream, &listeners, video, audio,
                                       &g_context, 0, nullptr, 0);
  HMRDP_LOGI("moonlight session: LiStartConnection returned %{public}d", result);
  if (result == 0) {
    // Ask the host to stop compositing the pointer into the video and forward it
    // on the cursor channel instead (foundation-sunshine, encrypted v2 only).
    if (config.localCursorMode) {
      LiSetLocalCursorMode(true);
    }
    // Wait here so Start/Stop stay paired on one thread; LiStopConnection must
    // run on this thread.
    {
      std::unique_lock<std::mutex> lock(g_cvMutex);
      g_cv.wait(lock, []() {
        return g_stopRequested.load() || g_terminated.load();
      });
    }
    // Ordering matters: disable while the control stream is still up.
    if (config.localCursorMode) {
      LiSetLocalCursorMode(false);
    }
    LiStopConnection();
    HMRDP_LOGI("moonlight session: LiStopConnection done");
  } else if (!g_stopRequested && !g_terminated) {
    HMRDP_LOGW("moonlight session: LiStartConnection failed (%{public}d)", result);
    Emit(MoonlightSessionEvent::kTerminated, result, "");
  }
  // The bridge must stop using the presenter before it is destroyed.
  MoonlightVideoDetachPresenter();
  g_connected = false;
  g_active = false;
}

}  // namespace

void MoonlightSessionSetWindow(void* window, int width, int height) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_context.window = window;
  g_context.width = width;
  g_context.height = height;
  // The presenter, when one is attached, owns the swapchain/surface built from this window.
  MoonlightVideoSetSurface(window, width, height);
}

void MoonlightSessionResizeSurface(int width, int height) {
  MoonlightVideoResizeSurface(width, height);
}

void MoonlightSessionSetCursorCallback(MoonlightCursorCallback callback) {
  g_cursorCallback.store(callback);
}

void MoonlightSessionSetClipboardCallback(MoonlightClipboardCallback callback) {
  g_clipboardCallback.store(callback);
}

int MoonlightSessionSendClipboard(const void* data, int length) {
  if (!g_active.load()) {
    return -3;
  }
  return LiSendClipboardData(data, length);
}

std::string MoonlightSessionStart(const MoonlightStreamConfig& config,
                                  MoonlightSessionCallback callback) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_active) {
    return "busy";
  }
  // A joinable thread must be reaped before assignment, or std::thread's
  // destructor calls std::terminate.
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
  MoonlightAudioReset();
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
      // Break a pending LiStartConnection; a no-op once established.
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
  MoonlightAudioReset();
  HMRDP_LOGI("moonlight session: stopped");
}

bool MoonlightSessionActive() {
  return g_active;
}

std::string MoonlightLaunchQueryParameters() {
  const char* extra = LiGetLaunchUrlQueryParameters();
  return extra != nullptr ? std::string(extra) : std::string();
}

int MoonlightSessionMouseMove(int deltaX, int deltaY) {
  if (!g_connected) {
    return 0;
  }
  return LiSendMouseMoveEvent(static_cast<short>(deltaX), static_cast<short>(deltaY));
}

int MoonlightSessionMousePosition(int x, int y, int referenceWidth, int referenceHeight) {
  if (!g_connected) {
    return 0;
  }
  return LiSendMousePositionEvent(static_cast<short>(x), static_cast<short>(y),
                                  static_cast<short>(referenceWidth),
                                  static_cast<short>(referenceHeight));
}

int MoonlightSessionMouseButton(int action, int button) {
  if (!g_connected) {
    return 0;
  }
  return LiSendMouseButtonEvent(static_cast<char>(action), button);
}

int MoonlightSessionKey(int keyCode, int action, int modifiers) {
  if (!g_connected) {
    return 0;
  }
  return LiSendKeyboardEvent(static_cast<short>(keyCode), static_cast<char>(action),
                             static_cast<char>(modifiers));
}

int MoonlightSessionScroll(int clicks) {
  if (!g_connected) {
    return 0;
  }
  return LiSendScrollEvent(static_cast<signed char>(clicks));
}

int MoonlightSessionTouch(int eventType, int pointerId, float x, float y, float pressure) {
  if (!g_connected) {
    return 0;
  }
  return LiSendTouchEvent(static_cast<uint8_t>(eventType), static_cast<uint32_t>(pointerId),
                          x, y, pressure, 0.0f, 0.0f, LI_ROT_UNKNOWN);
}

int MoonlightSessionTouchpadFrame(int contactCount, const uint8_t* eventTypes,
                                  const uint32_t* pointerIds, const float* x, const float* y,
                                  const float* pressure, int widthMm, int heightMm, int buttonState) {
  if (!g_connected) {
    return 0;
  }
  if (contactCount <= 0 || contactCount > kMaxTouchpadContacts) {
    return LI_ERR_UNSUPPORTED;
  }
  // LiSendTouchpadFrameEvent itself returns LI_ERR_UNSUPPORTED unless the host
  // advertised the extension, so stock Sunshine / GFE simply ignore this.
  return LiSendTouchpadFrameEvent(static_cast<uint8_t>(contactCount), eventTypes, pointerIds, x, y,
                                  pressure, LI_ROT_UNKNOWN, static_cast<uint16_t>(widthMm),
                                  static_cast<uint16_t>(heightMm),
                                  static_cast<uint8_t>(buttonState));
}

int MoonlightSessionHighResScroll(int amount) {
  if (!g_connected) {
    return 0;
  }
  return LiSendHighResScrollEvent(static_cast<short>(amount));
}

int MoonlightSessionHighResHScroll(int amount) {
  if (!g_connected) {
    return 0;
  }
  return LiSendHighResHScrollEvent(static_cast<short>(amount));
}

int MoonlightSessionControllerState(int controllerNumber, int activeGamepadMask, int buttonFlags,
                                   int leftTrigger, int rightTrigger, int leftStickX, int leftStickY,
                                   int rightStickX, int rightStickY) {
  if (!g_connected) {
    return 0;
  }
  return LiSendMultiControllerEvent(static_cast<short>(controllerNumber),
                                    static_cast<short>(activeGamepadMask), buttonFlags,
                                    static_cast<unsigned char>(leftTrigger),
                                    static_cast<unsigned char>(rightTrigger),
                                    static_cast<short>(leftStickX), static_cast<short>(leftStickY),
                                    static_cast<short>(rightStickX), static_cast<short>(rightStickY));
}

int MoonlightSessionControllerArrival(int controllerNumber, int activeGamepadMask, int type,
                                      int supportedButtonFlags, int capabilities) {
  if (!g_connected) {
    return 0;
  }
  return LiSendControllerArrivalEvent(static_cast<uint8_t>(controllerNumber),
                                      static_cast<uint16_t>(activeGamepadMask),
                                      static_cast<uint8_t>(type),
                                      static_cast<uint32_t>(supportedButtonFlags),
                                      static_cast<uint16_t>(capabilities));
}

int MoonlightSessionText(const char* text, int length) {
  if (!g_connected) {
    return 0;
  }
  if (text == nullptr || length <= 0) {
    return LI_ERR_UNSUPPORTED;
  }
  return LiSendUtf8TextEvent(text, static_cast<unsigned int>(length));
}

int MoonlightSessionHostFeatureFlags() {
  return g_connected ? static_cast<int>(LiGetHostFeatureFlags()) : 0;
}

}  // namespace hmrdp
