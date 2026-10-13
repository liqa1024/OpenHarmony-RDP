#ifndef HMRDP_MOONLIGHT_H
#define HMRDP_MOONLIGHT_H

#include <cstdint>
#include <string>

namespace hmrdp {

std::string MoonlightProbe();

// Raw cumulative telemetry; ArkTS diffs the counters into rates and per-frame means (nothing here
// is a display rate). RTT/packet counters come from the library at sample time, the rest from the decode bridge.
struct MoonlightTelemetry {
  uint64_t frames = 0;
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

// kAuto picks the best shared codec, the rest force one; mirrors ML_CODEC_*.
enum class MoonlightVideoCodecChoice : int {
  kAuto = 0,
  kH264 = 1,
  kHevc = 2,
  kAv1 = 3,
};

// Single VIDEO_FORMAT_* the session will use; 0 = no shared codec.
int MoonlightVideoFormats(MoonlightVideoCodecChoice choice, int serverCodecModeSupport);

int MoonlightVideoCodecSupport(int codec);

std::string MoonlightVideoCodec();

std::string MoonlightVideoWarning();

void* MoonlightVideoCallbacks();

}

#endif
