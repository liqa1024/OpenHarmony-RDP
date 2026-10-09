#ifndef HMRDP_MOONLIGHT_H
#define HMRDP_MOONLIGHT_H

#include <string>

namespace hmrdp {

std::string MoonlightProbe();

struct MoonlightVideoContext {
  void* window = nullptr;
};

enum class MoonlightVideoPreference : int {
  kAuto = 0,
  kH264 = 1,
  kHevc = 2,
  kAv1 = 3,
};

int MoonlightVideoFormats(MoonlightVideoPreference preference = MoonlightVideoPreference::kAuto);

int MoonlightVideoCodecSupport(int codec);

std::string MoonlightVideoCodec();

std::string MoonlightVideoWarning();

void* MoonlightVideoCallbacks();

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_H
