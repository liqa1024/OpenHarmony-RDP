#ifndef HMRDP_DECODE_TUNING_H
#define HMRDP_DECODE_TUNING_H

#include <string>

namespace hmrdp {

int DecodeThreads();

int DecodeCpuCount();

std::string DecodeThreadsInfo();

// "min-max" kHz, or empty when not exposed; sampled figures are only comparable
// at the same frequency.
std::string CpuFreqInfo();

}

#endif
