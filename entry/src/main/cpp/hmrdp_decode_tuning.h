/*
 * HmRdp - how wide the Progressive tile decode may run.
 *
 * The decode's executor is the platform task queue (a FFRT concurrent queue, see
 * hmrdp_parallel.*); this value is the number of threads one region may use. The
 * calling thread runs one chunk itself (caller participation), so the queue's
 * maximum concurrency is this value minus one. There is no second executor: a
 * width of 1 decodes on the receiving thread, anything wider is FFRT. Threads
 * cannot be pinned to a core or a cluster here, so the knob chooses the width,
 * not where the work runs.
 *
 *  - `1` decodes on the receiving thread (no queue at all: no submission, no
 *    wake-up, no wait).
 *  - `0` (automatic) is the whole machine, capped by kAutoCap.
 *  - `n > 1` uses `n` threads: the calling thread plus `n - 1` queue workers.
 *
 * The value is process-wide and applies to the live session and the offline
 * replay alike; the patched decoder reads it through HmrdpDecodeWidth() and
 * picks it up at a Progressive region boundary.
 * Width, energy proxy and measurement discipline: doc_agent/cpu-accel-plan.md
 * §0/§1.
 */
#ifndef HMRDP_DECODE_TUNING_H
#define HMRDP_DECODE_TUNING_H

#include <string>

namespace hmrdp {

// workers = 0 means "automatic". Out-of-range values are clamped to [1, 8].
void SetDecodeThreads(int workers);

// Dev A/B for the tile work decomposition. Mode 1 (the default) gives every task
// one contiguous *home* range - a worker walks contiguous memory and different
// workers' ranges are far apart - and lets it steal the other homes' remaining
// blocks once its own is done, so the tail is one block rather than one whole
// range. Mode 0 is the plain shared claim cursor (blocks of HMRDP_TILE_CLAIM
// handed out dynamically), kept for the A/B. Mode 2 is home plus a dev probe that
// routes the width-1 case through the platform queue, so the executor's own cost
// can be measured against the inline serial branch (see hmrdp_parallel.h). Read
// on demand, like the width.
void SetParallelMode(int mode);
int ParallelMode();

// The value that is actually in effect (automatic resolved, clamped).
int DecodeThreads();

// The automatic choice (never 0).
int AutoDecodeThreads();

// CPU count as the platform reports it (sysconf); 1 when unknown.
int DecodeCpuCount();

// Performance-core count, or 0 when the device does not let us tell.
int DecodePerfCores();

// One line for the log / the settings page, e.g.
// "workers=4 (auto: perf-cores=4 cores=9)".
std::string DecodeThreadsInfo();

// Current frequency of the device's CPUs as "min-max" kHz, or an empty string
// when the platform does not expose it. Sampled on demand: the *playback rate*
// decides which clock the whole SoC runs at, so a run's figures are only
// comparable to another run at the same frequency (doc_agent/gfx-engine.md §8.3).
std::string CpuFreqInfo();

}  // namespace hmrdp

#endif  // HMRDP_DECODE_TUNING_H
