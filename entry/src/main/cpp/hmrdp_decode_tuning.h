/*
 * HmRdp - how many workers the Progressive tile decode may use.
 *
 * FreeRDP decodes Progressive tiles through the WinPR thread pool (one pool per
 * process, one work item per region, workers woken for every message). The
 * worker count is the only knob that shapes that parallelism on this platform:
 * threads cannot be pinned to a core or a cluster here, so "adaptive" means
 * choosing how many workers exist, not where they run.
 *
 *  - `1` decodes on the receiving thread (the pool is bypassed entirely: no
 *    submission, no wake-up, no wait). That is the power-optimal setting for a
 *    stream the client can already keep up with.
 *  - `n > 1` runs `n` pool workers. The decode wall time only improves until the
 *    work stops scaling - measured on the test device, beyond 4 workers the
 *    frame time does not move at all (the extra workers land on the little
 *    cores), so the automatic choice stays near the performance-core count.
 *
 * The automatic choice is min(performance cores - or the core count when the
 * device does not expose its clusters - , 4): the decode stops getting faster
 * after a couple of workers (measured, doc_agent/cpu-accel-plan.md §1), so the
 * ceiling is small and only comes down on smaller devices. The value is
 * process-wide and applies to the live session and the offline replay alike (a
 * session/replay picks it up when its codec context starts, i.e. when it begins
 * decoding).
 *
 * **Currently pinned to 1** (`kPinnedWorkers` in the .cpp): the parallel path is
 * FreeRDP's own pool, untuned for this platform and inefficient, and it is due
 * to be rewritten - one serial baseline keeps measurements free of extra
 * variables. Requests are still stored, so reopening the knob (and the settings
 * / replay controls that set it) is a one-line change.
 */
#ifndef HMRDP_DECODE_TUNING_H
#define HMRDP_DECODE_TUNING_H

#include <string>

namespace hmrdp {

// workers = 0 means "automatic". Out-of-range values are clamped to [1, 8].
void SetDecodeThreads(int workers);

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

// Re-forwards the stored value (used when a library was reloaded).
void ApplyStoredDecodeThreads();

// Current frequency of the device's CPUs as "min-max" kHz, or an empty string
// when the platform does not expose it. Sampled on demand: the *playback rate*
// decides which clock the whole SoC runs at, so a run's figures are only
// comparable to another run at the same frequency (doc_agent/gfx-engine.md §8.3).
std::string CpuFreqInfo();

}  // namespace hmrdp

#endif  // HMRDP_DECODE_TUNING_H
