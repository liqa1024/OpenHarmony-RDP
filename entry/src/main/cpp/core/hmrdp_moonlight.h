#ifndef HMRDP_MOONLIGHT_H
#define HMRDP_MOONLIGHT_H

#include <cstdint>
#include <string>

namespace hmrdp {

std::string MoonlightProbe();

// Raw cumulative telemetry; ArkTS diffs the counters into rates and per-frame means (nothing here
// is a display rate). RTT/packet counters come from the library at sample time, the rest from the decode bridge.
struct MoonlightTelemetry {
  // Decoded pictures out of the codec (the rate the display is actually fed) ...
  uint64_t frames = 0;
  // ... versus the access units handed to it and the ones dropped for being too late to show.
  uint64_t submittedFrames = 0;
  uint64_t droppedFrames = 0;
  uint64_t bytes = 0;
  uint64_t hostLatencyTenthMs = 0;
  uint64_t hostLatencyFrames = 0;
  uint64_t decodeRoundTripUs = 0;
  uint64_t decodeRoundTripFrames = 0;
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
};

// 超分: the presenter the decode bridge renders decoded frames into (a hmrdp::FramePresenter*), or
// nullptr for the default direct-to-surface path. Owned by the session; set before the session
// starts and cleared when it ends. With a presenter the bridge decodes to NV12 buffers and calls
// PresentVideo per frame; without one it renders straight into the window.
void MoonlightVideoSetPresenter(void* presenter);

// kAuto picks the best shared codec, the rest force one; mirrors ML_CODEC_*.
enum class MoonlightVideoCodecChoice : int {
  kAuto = 0,
  kH264 = 1,
  kHevc = 2,
  kAv1 = 3,
};

// Single VIDEO_FORMAT_* the session will use; 0 = no shared codec.
int MoonlightVideoFormats(MoonlightVideoCodecChoice choice, int serverCodecModeSupport);

// Highest frame rate the decoder for that VIDEO_FORMAT_* reports at a picture size, 0 when the
// platform states none. The control layer caps the rate it asks the host for with this.
int MoonlightVideoMaxFps(int videoFormat, int width, int height);

int MoonlightVideoCodecSupport(int codec);

std::string MoonlightVideoCodec();

std::string MoonlightVideoWarning();

void* MoonlightVideoCallbacks();

}

#endif
