/*
 * HmRdp - platform parallel executor for the progressive tile decode
 * (doc_agent/cpu-accel-plan.md §2 M-a).
 *
 * The decode's granularity is a chunk callback that FreeRDP's progressive codec
 * owns; the question M-a asks is *who runs those callbacks*. The codec's own
 * WinPR pool creates and parks its own threads, submits one work item per chunk
 * into a locked queue and waits on a pool-global countdown - on this platform
 * that shape costs several times the CPU of the decode it carries
 * (doc_agent/cpu-accel-plan.md §1).
 *
 * This module runs the same callbacks on FFRT, following the FFRT programming
 * model rather than only its task API:
 *
 *  - a **concurrent queue** (`ffrt_queue_concurrent`), not bare task submission.
 *    A concurrent queue has an explicit maximum concurrency, which is what the
 *    settings' worker count maps to ("并发度...同时也对应 FFRT Worker 数量"), and
 *    it keeps the width under our control instead of leaving it to whatever the
 *    global pool happens to run.
 *  - tasks carry a **task attribute** with a name and an explicit QoS. Without
 *    one a task gets the default QoS, whose core class is not the one the
 *    frame-delivery critical path wants.
 *  - the barrier is per task: every chunk is submitted with a handle and the
 *    caller waits for exactly those handles (`ffrt_queue_wait`). Nothing waits
 *    on "all work in the system", which is what made the pool's wait a global
 *    countdown.
 *  - the callbacks themselves are pure with respect to FFRT: they touch the
 *    tile's own buffers plus one atomic claim counter, and never take a
 *    non-FFRT lock or block. (Should they ever need to block, FFRT's own
 *    mutex/condition_variable are the ones to use - blocking a worker with a
 *    pthread primitive starves the executor.)
 *
 * The patched decoder calls this through weak symbols, so a build without these
 * exports (or a device without ffrt) keeps using its own pool.
 */
#ifndef HMRDP_PARALLEL_H
#define HMRDP_PARALLEL_H

#ifdef __cplusplus
extern "C" {
#endif

// Non-zero when the platform queue is selected.
int HmrdpParallelAvailable(void);

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
