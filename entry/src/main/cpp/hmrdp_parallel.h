/*
 * HmRdp - platform parallel executor for the progressive tile decode
 * (doc_agent/cpu-accel-plan.md §0).
 *
 * The decode's granularity is a chunk callback that FreeRDP's progressive codec
 * owns; this module is *who runs those callbacks* - FFRT, following the FFRT
 * programming model rather than only its task API:
 *
 *  - a **concurrent queue** (`ffrt_queue_concurrent`), not bare task submission.
 *    A concurrent queue has an explicit maximum concurrency, which is what the
 *    settings' worker count maps to ("并发度...同时也对应 FFRT Worker 数量"), so the
 *    width stays under our control instead of being left to whatever the global
 *    pool happens to run.
 *  - tasks carry a **task attribute** with a name and an explicit QoS. Without
 *    one a task gets the default QoS, whose core class is not the one the
 *    frame-delivery critical path wants.
 *  - the barrier is per task: every chunk is submitted with a handle and the
 *    caller waits for exactly those handles (`ffrt_queue_wait`). Nothing waits
 *    on "all work in the system".
 *  - the callbacks themselves are pure with respect to FFRT: they touch the
 *    tile's own buffers plus one atomic claim counter, and never take a
 *    non-FFRT lock or block. (Should they ever need to block, FFRT's own
 *    mutex/condition_variable are the ones to use - blocking a worker with a
 *    pthread primitive starves the executor.)
 *
 * The patched decoder calls this through weak symbols, so a build without these
 * exports (or a device without ffrt) decodes on the receiving thread.
 */
#ifndef HMRDP_PARALLEL_H
#define HMRDP_PARALLEL_H

#ifdef __cplusplus
extern "C" {
#endif

// Non-zero when the platform queue is selected.
int HmrdpParallelAvailable(void);

// The configured decode width (>= 1). The patched decoder reads this to choose
// between its serial branch and the platform queue; it is resolved on demand,
// so a settings change takes effect at the next Progressive region.
unsigned int HmrdpDecodeWidth(void);

// Runs fn(ctx, i) for i in [0, tasks) on the platform concurrent queue and
// returns once all of them have finished. The queue's maximum concurrency is the
// configured decode worker count. Returns 0 on success; non-zero means the caller
// must fall back to its own executor. `fn` is a plain function pointer because
// the caller is C.
int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx);

// Dev read-out: the highest number of callbacks that were inside fn at the same
// moment since the previous call, then reset. Answers "did the platform queue
// actually run this in parallel", which wall clock alone cannot.
unsigned int HmrdpParallelTakeMaxConcurrency(void);

#ifdef __cplusplus
}
#endif

#endif  // HMRDP_PARALLEL_H
