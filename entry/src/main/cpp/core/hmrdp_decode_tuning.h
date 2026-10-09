#ifndef HMRDP_DECODE_TUNING_H
#define HMRDP_DECODE_TUNING_H

#include <string>

namespace hmrdp {

int DecodeThreads();

int DecodeCpuCount();

std::string DecodeThreadsInfo();

// Current CPU frequency as "min-max" kHz, or empty when not exposed; sampled
// figures are only comparable at the same frequency (doc_agent/gfx-engine.md §7.1).
std::string CpuFreqInfo();

}  // namespace hmrdp

#endif  // HMRDP_DECODE_TUNING_H
