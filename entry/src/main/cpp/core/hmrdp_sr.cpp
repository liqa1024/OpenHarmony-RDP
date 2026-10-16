#include "hmrdp_sr.h"

namespace hmrdp {

int SuperResolutionSessionSize(int output, int ratioPercent) {
  if (output <= 0 || ratioPercent <= 100) {
    return output;
  }
  const int scaled = (output * 100 + ratioPercent / 2) / ratioPercent;
  const int even = scaled - (scaled % 2);
  return even > 0 ? even : 2;
}

}
