#ifndef HMRDP_MOONLIGHT_SESSION_H
#define HMRDP_MOONLIGHT_SESSION_H

#include <cstdint>
#include <string>

namespace hmrdp {

enum class MoonlightSessionEvent : int {
  kStarted = 0,
  kTerminated = 1,
  kStageStarting = 2,
  kStageComplete = 3,
  kStageFailed = 4,
  // Foundation-sunshine cursor extension: host renders the pointer on a separate channel,
  // carried on the cursor callback (not the session callback).
  kCursorShape = 5,
  // Shortcut passthrough: a system-intercepted key as two event codes so `value` stays the keyCode.
  kKeyInterceptDown = 6,
  kKeyInterceptUp = 7,
  kClipboardData = 8,
};

// Called from the session's own thread; must not block.
using MoonlightSessionCallback = void (*)(MoonlightSessionEvent event, int value, const char* text);

// When hasShape is set, bgra is width*height*4 and valid only during the call; otherwise a
// visibility-only change with no geometry.
struct MoonlightCursorShape {
  bool visible = false;
  bool hasShape = false;
  uint32_t shapeId = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  int16_t hotspotX = 0;
  int16_t hotspotY = 0;
  const uint8_t* bgra = nullptr;
  uint32_t size = 0;
};

// Called from the control-stream thread; must not block.
using MoonlightCursorCallback = void (*)(const MoonlightCursorShape& shape);

// Called from the control-stream thread; the callee must not block it.
using MoonlightClipboardCallback = void (*)(const uint8_t* data, int length);

struct MoonlightStreamConfig {
  std::string host;
  std::string serverInfoAppVersion;
  std::string serverInfoGfeVersion;
  std::string sessionUrl;
  int serverCodecModeSupport = 0;

  int width = 1920;
  int height = 1080;
  int fps = 60;
  int bitrateKbps = 20000;
  int packetSize = 1024;
  // STREAM_CFG_* (0 local, 1 remote, 2 auto).
  int streamingRemotely = 2;
  int audioConfiguration = 0;
  int supportedVideoFormats = 0;
  int clientRefreshRateX100 = 0;
  int colorSpace = 0;
  int colorRange = 0;
  int encryptionFlags = 0;
  // 16-byte remote-input AES key and IV (key + rikeyid-derived IV from /launch).
  std::string remoteInputAesKey;
  std::string remoteInputAesIv;

  // Request foundation-sunshine local cursor mode when the host advertises it; off keeps the
  // host-composited cursor.
  bool localCursorMode = false;

  // False leaves the library's audio renderer unset (silent) so the host plays locally (localAudioPlayMode).
  bool enableAudio = true;

  bool lowLatencyAudio = true;
};

// Null disables delivery; safe to call before a session.
void MoonlightSessionSetCursorCallback(MoonlightCursorCallback callback);

// Null disables delivery; safe to call before a session.
void MoonlightSessionSetClipboardCallback(MoonlightClipboardCallback callback);

// Send an opaque clipboard frame to the host; returns LiSendClipboardData's code.
int MoonlightSessionSendClipboard(const void* data, int length);

// Must be called before MoonlightSessionStart; passed to LiStartConnection as the render context.
void MoonlightSessionSetWindow(void* window);

// Empty string on success, else a stable token ("busy" / "config" / "thread").
std::string MoonlightSessionStart(const MoonlightStreamConfig& config,
                                  MoonlightSessionCallback callback);

void MoonlightSessionStop();

bool MoonlightSessionActive();

// The query string common-c wants appended to /launch and /resume; owned by the library, copied out here.
std::string MoonlightLaunchQueryParameters();

// Input entry points, mirroring the LiSend* ones. Each returns the library's own result code
// verbatim: 0 when the event was taken, LI_ERR_UNSUPPORTED when the host lacks the feature (a
// by-design refusal, not a failure), any other non-zero when the event was refused. Nothing is
// reported when no session is active — there is no transport to refuse the event, and a dead
// session is signalled elsewhere (0 is returned so it is not mistaken for a budget rejection).
int MoonlightSessionMouseMove(int deltaX, int deltaY);
int MoonlightSessionMousePosition(int x, int y, int referenceWidth, int referenceHeight);
int MoonlightSessionMouseButton(int action, int button);
int MoonlightSessionKey(int keyCode, int action, int modifiers);
int MoonlightSessionScroll(int clicks);
// Native touchscreen: x/y normalized 0..1 over the stream area, pressure 0..1 (0 = unknown);
// eventType is a LI_TOUCH_EVENT_* constant.
int MoonlightSessionTouch(int eventType, int pointerId, float x, float y, float pressure);
// Native precision touchpad (foundation-sunshine extension): contacts normalized over the physical
// pad surface, widthMm/heightMm its size; the library itself reports LI_ERR_UNSUPPORTED unless the
// host advertised the touchpad feature flag.
int MoonlightSessionTouchpadFrame(int contactCount, const uint8_t* eventTypes,
                                  const uint32_t* pointerIds, const float* x, const float* y,
                                  const float* pressure, int widthMm, int heightMm, int buttonState);
// High-resolution wheel, in WHEEL_DELTA (120) units; vertical and horizontal.
int MoonlightSessionHighResScroll(int amount);
int MoonlightSessionHighResHScroll(int amount);
// One controller slot's whole state (LiSendMultiControllerEvent); activeGamepadMask carries a bit
// per attached controller, triggers are 0..255 and sticks -32767..32767 with +Y up.
int MoonlightSessionControllerState(int controllerNumber, int activeGamepadMask, int buttonFlags,
                                   int leftTrigger, int rightTrigger, int leftStickX, int leftStickY,
                                   int rightStickX, int rightStickY);
// Declares one attached controller's type / buttons / capabilities
// (LiSendControllerArrivalEvent); the library falls back to a plain state event when the host
// does not support arrival events.
int MoonlightSessionControllerArrival(int controllerNumber, int activeGamepadMask, int type,
                                      int supportedButtonFlags, int capabilities);
// Composed text (IME / non-US layout) sent as one UTF-8 string.
int MoonlightSessionText(const char* text, int length);
// LI_FF_* bits the host advertised; 0 before a session is established.
int MoonlightSessionHostFeatureFlags();

}

#endif
