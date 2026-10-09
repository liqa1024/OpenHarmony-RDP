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
  // Foundation-sunshine cursor extension: the host renders the pointer on a
  // separate channel instead of compositing it into the video. Carried on the
  // cursor callback, not the session callback.
  kCursorShape = 5,
};

// Called from the session's own thread; must not block.
using MoonlightSessionCallback = void (*)(MoonlightSessionEvent event, int value, const char* text);

// When hasShape is set, bgra is width*height*4 bytes and only valid during the
// call; otherwise it is a visibility-only (show/hide) change with no geometry.
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
  bool streamingRemotely = false;
  int audioConfiguration = 0;
  int supportedVideoFormats = 0;
  int clientRefreshRateX100 = 0;
  int colorSpace = 0;
  int colorRange = 0;
  int encryptionFlags = 0;
  // 16-byte remote-input AES key and IV (key + rikeyid-derived IV from /launch).
  std::string remoteInputAesKey;
  std::string remoteInputAesIv;

  // Request foundation-sunshine local cursor mode when the host advertises it;
  // off keeps the host-composited cursor.
  bool localCursorMode = false;
};

// Null disables delivery; safe to call before a session.
void MoonlightSessionSetCursorCallback(MoonlightCursorCallback callback);

// Must be called before MoonlightSessionStart; passed to LiStartConnection as
// the render context.
void MoonlightSessionSetWindow(void* window);

// Empty string on success, else a stable token ("busy" / "config" / "thread").
std::string MoonlightSessionStart(const MoonlightStreamConfig& config,
                                  MoonlightSessionCallback callback);

void MoonlightSessionStop();

bool MoonlightSessionActive();

// The query string common-c wants appended to /launch and /resume; owned by the
// library, copied out here.
std::string MoonlightLaunchQueryParameters();

// No-ops unless the session is active, mirroring the LiSend* entry points.
void MoonlightSessionMouseMove(int deltaX, int deltaY);
void MoonlightSessionMousePosition(int x, int y, int referenceWidth, int referenceHeight);
void MoonlightSessionMouseButton(int action, int button);
void MoonlightSessionKey(int keyCode, int action, int modifiers);
void MoonlightSessionScroll(int clicks);

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_SESSION_H
