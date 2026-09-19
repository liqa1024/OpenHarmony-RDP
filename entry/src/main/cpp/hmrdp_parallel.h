/*
 * HmRdp - platform parallel executor for the progressive tile decode
 * (doc_agent/cpu-accel-plan.md §1).
 *
 * The decode's granularity is a chunk callback that FreeRDP's progressive codec
 * owns; this module is *who runs those callbacks* - FFRT, following the FFRT
 * programming model rather than only its task API:
 *
 *  - a **concurrent queue** (`ffrt_queue_concurrent`), not bare task submission.
 *    A concurrent queue has an explicit maximum concurrency, which is what the
 *    decode's width maps to ("并发度...同时也对应 FFRT Worker 数量"), so the thread
 *    count stays under our control instead of being left to whatever the global
 *    pool happens to run. The limit is the width **minus one**: the calling
 *    thread runs one chunk itself (caller participation, see below).
 *  - **caller participation**: the calling (receiving) thread runs one chunk
 *    while the queue runs the rest, and only then waits. The region uses `width`
 *    threads, but one of them is the thread that would otherwise sit in the
 *    barrier - so no thread is idle while tiles are claimable, one fewer worker
 *    is asked of the pool, and the caller's share runs in the receiving thread's
 *    own context (warm caches, its QoS) instead of a freshly woken worker's.
 *    Measured effect and why it is not optional: doc_agent/gfx-engine.md §8.1.
 *  - tasks carry a **task attribute** with a name and an explicit QoS. Without
 *    one a task gets the default QoS, whose core class is not the one the
 *    frame-delivery critical path wants.
 *  - the barrier is per task: every chunk is submitted with a handle and the
 *    caller waits for exactly those handles (`ffrt_queue_wait`). Nothing waits
 *    on "all work in the system".
 *  - the callbacks themselves are pure with respect to FFRT: they touch the
 *    tile's own buffers plus one atomic claim cursor, and never take a
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

// The decode width (>= 1). The patched decoder reads this to choose between its
// serial branch and the platform queue; it is resolved on demand, so a machine
// with a different core count needs no re-arming.
unsigned int HmrdpDecodeWidth(void);

// Runs fn(ctx, i) for i in [0, tasks) and returns once all of them have finished.
// The calling thread runs one of the chunks itself and the queue runs the rest
// (caller participation), so the region uses the width's worth of threads while
// the queue's maximum concurrency is only width-1. Returns 0 on success; non-zero
// means the caller must fall back to its own executor. `fn` is a plain function
// pointer because the caller is C.
int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx);

#ifdef __cplusplus
}
#endif

#endif  // HMRDP_PARALLEL_H
