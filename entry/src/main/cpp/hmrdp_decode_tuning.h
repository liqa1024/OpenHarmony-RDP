/*
 * HmRdp - how wide the Progressive tile decode runs.
 *
 * The width is the machine's online core count, capped by kAutoCap; it is not a
 * setting. The executor is the platform task queue (FFRT concurrent queue, see
 * hmrdp_parallel.*): the calling thread always runs one chunk itself (caller
 * participation) and the queue offers the other width-1 workers. A machine that
 * reports one core (or a build without the platform queue) decodes on the
 * receiving thread. Threads cannot be pinned, so the width chooses how many
 * threads, not where they run.
 *
 * Details: doc_agent/cpu-accel-plan.md §1.
 */
#ifndef HMRDP_DECODE_TUNING_H
#define HMRDP_DECODE_TUNING_H

#include <string>

namespace hmrdp {

// The number of threads one Progressive region may use (never 0).
int DecodeThreads();

// CPU count as the platform reports it (sysconf); 1 when unknown.
int DecodeCpuCount();

// One line for the log, e.g. "workers=4 (cores=9)".
std::string DecodeThreadsInfo();

// Current frequency of the device's CPUs as "min-max" kHz, or an empty string
// when the platform does not expose it. Sampled on demand: the *playback rate*
// decides which clock the whole SoC runs at, so a run's figures are only
// comparable to another run at the same frequency (doc_agent/gfx-engine.md §8.1).
std::string CpuFreqInfo();

}  // namespace hmrdp

#endif  // HMRDP_DECODE_TUNING_H
