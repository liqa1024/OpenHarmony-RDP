#ifndef HMRDP_MOONLIGHT_H
#define HMRDP_MOONLIGHT_H

#include <cstdint>
#include <string>

namespace hmrdp {

std::string MoonlightProbe();

// Raw cumulative telemetry of the live Moonlight session. The ArkTS side diffs
// the counters into rates (fps / bitrate) and per-frame means (host encode and
// local decode); nothing here is a display rate. RTT and packet counters are
// read from the library at sample time, the rest is accumulated in the decode
// bridge.
struct MoonlightTelemetry {
  uint64_t frames = 0;
  uint64_t bytes = 0;
  // Sum / count over frames whose host provided a frame processing latency.
  uint64_t hostLatencyTenthMs = 0;
  uint64_t hostLatencyFrames = 0;
  // Sum (us) / count over frames paired from submit to decoded output.
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

// kAuto lets the client pick the best codec both sides support; the rest force
// one codec (the per-connection manual override). Mirrors ML_CODEC_*.
enum class MoonlightVideoCodecChoice : int {
  kAuto = 0,
  kH264 = 1,
  kHevc = 2,
  kAv1 = 3,
};

// Returns the single VIDEO_FORMAT_* the session will use; 0 = the device and
// host share no codec.
int MoonlightVideoFormats(MoonlightVideoCodecChoice choice, int serverCodecModeSupport);

int MoonlightVideoCodecSupport(int codec);

std::string MoonlightVideoCodec();

std::string MoonlightVideoWarning();

void* MoonlightVideoCallbacks();

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_H
