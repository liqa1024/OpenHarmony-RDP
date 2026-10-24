#ifndef HMRDP_MOONLIGHT_H
#define HMRDP_MOONLIGHT_H

#include <cstdint>
#include <string>

namespace hmrdp {

std::string MoonlightProbe();

// Raw cumulative telemetry; ArkTS diffs the counters into rates and per-frame means (nothing here
// is a display rate). RTT/packet counters come from the library at sample time, the rest from the decode bridge.
struct MoonlightTelemetry {
  // Decoded pictures out of the codec (the rate the codec produces) ...
  uint64_t frames = 0;
  // ... pictures actually presented (the rate the display is fed) ...
  uint64_t presentedFrames = 0;
  // ... versus the access units handed to it and the ones dropped for being too late to show.
  uint64_t submittedFrames = 0;
  uint64_t droppedFrames = 0;
  // Display-gate drops in the decoder and pictures replaced before the sink took them. Both are
  // cumulative and, with droppedFrames, make up the stream's total loss.
  uint64_t presentDroppedFrames = 0;
  uint64_t handoffReplacedFrames = 0;
  // The part of that loss given up while the display's gear was below the stream rate: dropped
  // because the screen cannot show it, which nothing on the client can fix. Cumulative.
  uint64_t refreshDroppedFrames = 0;
  // The display's current gear, 0 when the platform states none.
  int32_t displayRefreshRateHz = 0;
  uint64_t bytes = 0;
  uint64_t hostLatencyTenthMs = 0;
  uint64_t hostLatencyFrames = 0;
  uint64_t decodeRoundTripUs = 0;
  uint64_t decodeRoundTripFrames = 0;
  // GPU work of the presents since the previous sample (microseconds): convert, 超分 and letterbox
  // are the video path's own `copy/sr/blit` phases. presentWaitUs is the display wait of the same
  // window. All zero when no presenter is attached.
  uint64_t gpuCopyUs = 0;
  uint64_t gpuSrUs = 0;
  uint64_t gpuBlitUs = 0;
  uint64_t presentWaitUs = 0;
  int32_t pendingVideoFrames = 0;
  int32_t rttMs = -1;
  int32_t rttVarianceMs = -1;
  uint32_t packetVideo = 0;
  uint32_t packetFec = 0;
  uint32_t packetFecRecovered = 0;
  uint32_t packetFecFailed = 0;
  uint32_t packetOos = 0;
  uint32_t packetInvalid = 0;
};

MoonlightTelemetry MoonlightReadTelemetry();

struct MoonlightVideoContext {
  void* window = nullptr;
  int width = 0;
  int height = 0;
};

// 超分: the presenter the decode bridge renders decoded frames into (a hmrdp::FramePresenter*), or
// nullptr when 超分 is off - the direct-to-surface path, which is every non-SR session (see
// moonlight-video.md §2). Owned by the session, borrowed by the bridge: attach before starting the
// connection, detach before destroying the presenter (a detach waits out a present already in flight).
// With a presenter the bridge decodes in buffer mode and calls PresentVideo per frame; without one it
// renders straight into the window.
void MoonlightVideoAttachPresenter(void* presenter);
void MoonlightVideoDetachPresenter();

// 超分: the session resolved a lower stream size than the output. The bridge reports "sr-unavailable"
// when no presenter can be attached, and "sr-inactive" when the presenter runs but its upscale object
// is not active, so the toolbar never claims 超分 for a session that is presenting the raw input. It
// additionally reports "color-mismatch" when the decoded buffers declare a colour space other than
// the one the host was asked to encode with (see hmrdp_vk_video.h).
void MoonlightVideoSetSuperResolutionRequested(bool requested);
bool MoonlightVideoBufferMode();

// HDR: the stream's encoding, from the connection's colour-space / range setting (`COLORSPACE_*` /
// `COLOR_RANGE_*`) and the HDR base transfer (`hdrMode`: 0 = SDR, 1 = HDR10/PQ, 2 = HLG). Rec.2020
// with hdrMode 2 picks the Rec.2020 HLG description, otherwise Rec.2020 picks the Rec.2020 PQ one;
// any other colour space is the SDR description whatever hdrMode says. It is what the decoder is told
// the frames are, so it is set with the same values the session negotiates.
void MoonlightVideoSetStreamColorSpace(int colorSpace, int colorRange, int hdrMode);
// The host's answer, driven by the control stream's HDR-mode packet (ConnListenerSetHdrMode). It is
// the only HDR fact this side has: the picture is presented by the platform's own decode path, so
// there is nothing of ours for it to drive (see moonlight-video.md §2). `MoonlightVideoHdr()` is the
// reading the toolbar shows.
void MoonlightVideoSetHdrMode(bool enabled);

// 超分: a live surface change (XComponent recreated or a new window bound while the session keeps
// running). The presenter replaces its swapchain/surface; a null window destroys it. The resize path
// only updates the extent (no swapchain teardown).
void MoonlightVideoSetSurface(void* window, int width, int height);
void MoonlightVideoClearSurface();

// 超分: forward a surface (window) resize to the attached presenter's swapchain; a no-op on the
// direct-to-surface path (the window scaling mode fits the picture itself).
void MoonlightVideoResizeSurface(int width, int height);

// kAuto picks the best shared codec, the rest force one; mirrors ML_CODEC_*.
enum class MoonlightVideoCodecChoice : int {
  kAuto = 0,
  kH264 = 1,
  kHevc = 2,
  kAv1 = 3,
};

// Single VIDEO_FORMAT_* the session will use; 0 = no shared codec. `hdr` asks for a 10-bit profile
// (HEVC Main10 / AV1 Main10) and for the host to accept it, so an HDR session never falls back to an
// 8-bit codec: H.264 cannot carry HDR and is not a candidate then.
int MoonlightVideoFormats(MoonlightVideoCodecChoice choice, int serverCodecModeSupport, bool hdr);

// Highest frame rate the decoder for that VIDEO_FORMAT_* reports at a picture size, 0 when the
// platform states none. The control layer caps the rate it asks the host for with this.
int MoonlightVideoMaxFps(int videoFormat, int width, int height);

int MoonlightVideoCodecSupport(int codec);

std::string MoonlightVideoCodec();

// The HDR format the session really presents: the base transfer, with " Vivid" appended while the
// stream carries the dynamic layer; "" otherwise. The reading is the host's own answer - the platform's
// decode path presents the HDR picture and this side never touches it, so nothing is claimed that the
// host did not state.
std::string MoonlightVideoHdr();

std::string MoonlightVideoWarning();

void* MoonlightVideoCallbacks();

}

#endif
