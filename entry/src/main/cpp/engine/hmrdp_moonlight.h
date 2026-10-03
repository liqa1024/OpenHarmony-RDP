/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight (GameStream) client wrapper: the link probe and the video decode
 * bridge (see doc_agent/moonlight-native.md §3).
 */
#ifndef HMRDP_MOONLIGHT_H
#define HMRDP_MOONLIGHT_H

#include <string>

namespace hmrdp {

// "1" when this build has the Moonlight client linked, else "0|<code>" with a
// stable code (the UI layer owns the wording), mirroring the capability probes
// in hmrdp_vk_context. The code is "no-moonlight" when the archives were not
// built into this module.
std::string MoonlightProbe();

// Renderer context passed as LiStartConnection's renderContext. `window` is the
// session's XComponent OHNativeWindow; the decoder renders straight into it.
struct MoonlightVideoContext {
  void* window = nullptr;
};

// The session's codec setting; mirrors the ArkTS ML_CODEC_* values so the
// connection's choice crosses the boundary as-is.
enum class MoonlightVideoPreference : int {
  kAuto = 0,
  kH264 = 1,
  kHevc = 2,
  kAv1 = 3,
};

// VIDEO_FORMAT_* bits to advertise as supportedVideoFormats, for the given
// ML_CODEC_* preference (kAuto = hardware-first; the policy is in
// doc_agent/moonlight-native.md §2). 0 when this build has no Moonlight client.
int MoonlightVideoFormats(MoonlightVideoPreference preference = MoonlightVideoPreference::kAuto);

// Decoder support for one ML_CODEC_* value, as a DecoderSupport int (0 none,
// 1 software, 2 hardware); auto or unknown returns 0.
int MoonlightVideoCodecSupport(int codec);

// The negotiated codec token ("h264" / "hevc" / "av1"), or "" before a stream.
std::string MoonlightVideoCodec();

// Stable code for the current decode path, or "" when fine: "software-decode|<mime>"
// or "no-decoder|<mime>". Set by setup, so it is meaningful once a stream has
// started; the UI owns the wording.
std::string MoonlightVideoWarning();

// The decoder-renderer callbacks for the direct link, or nullptr without a
// Moonlight client. Opaque here: cast to PDECODER_RENDERER_CALLBACKS from a
// translation unit that includes Limelight.h.
void* MoonlightVideoCallbacks();

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_H
