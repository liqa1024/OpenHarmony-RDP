/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight (GameStream) streaming session: the LiStartConnection wrapper.
 *
 * moonlight-common-c owns one process-global connection at a time and
 * LiStartConnection() blocks until the RTSP handshake finishes, so the session
 * runs it on its own thread and reports progress/termination through a callback.
 * The video path is the decode bridge in hmrdp_moonlight.* (surface direct to the
 * session's XComponent); audio is not wired yet (no Opus renderer). Input is
 * forwarded straight to the LiSend* entry points. See
 * doc_agent/moonlight-native.md §2/§3.
 */
#ifndef HMRDP_MOONLIGHT_SESSION_H
#define HMRDP_MOONLIGHT_SESSION_H

#include <cstdint>
#include <string>

namespace hmrdp {

/** What a session callback reports. `value` is the stage or error code. */
enum class MoonlightSessionEvent : int {
  // Connection established (connectionStarted).
  kStarted = 0,
  // Connection ended after establishment; value = errorCode (0 = graceful).
  kTerminated = 1,
  // A setup stage began; value = STAGE_* (text = LiGetStageName).
  kStageStarting = 2,
  // A setup stage completed; value = STAGE_*.
  kStageComplete = 3,
  // Setup failed; value = errorCode, text = LiGetStageName(stage).
  kStageFailed = 4,
};

/** Called from the session's own thread; must not block. */
using MoonlightSessionCallback = void (*)(MoonlightSessionEvent event, int value, const char* text);

/** Everything LiStartConnection needs, assembled by the ArkTS control layer. */
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
};

/**
 * Sets the OHNativeWindow the decoder renders into; must be called before
 * MoonlightSessionStart. The window is passed to LiStartConnection as the
 * render context.
 */
void MoonlightSessionSetWindow(void* window);

/**
 * Starts a streaming session. Returns an empty string on success (the session
 * thread is running), else a stable error token ("busy" / "config" / "thread").
 * `callback` may be null.
 */
std::string MoonlightSessionStart(const MoonlightStreamConfig& config,
                                  MoonlightSessionCallback callback);

/** Interrupts/stops the session and waits for the session thread to finish. */
void MoonlightSessionStop();

/** True between a successful start and its termination. */
bool MoonlightSessionActive();

// Input (no-ops unless the session is active), mirroring the LiSend* entry points.
void MoonlightSessionMouseMove(int deltaX, int deltaY);
void MoonlightSessionMousePosition(int x, int y, int referenceWidth, int referenceHeight);
void MoonlightSessionMouseButton(int action, int button);
void MoonlightSessionKey(int keyCode, int action, int modifiers);
void MoonlightSessionScroll(int clicks);

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_SESSION_H
