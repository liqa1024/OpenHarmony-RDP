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

// Dev read-out: the highest number of callbacks that were inside fn at the same
// moment since the previous call, then reset. Answers "did the platform queue
// actually run this in parallel", which wall clock alone cannot.
unsigned int HmrdpParallelTakeMaxConcurrency(void);

// Dev-only accounting of the decode's parallel section, read back by the
// replay's `par` line (doc_agent/cpu-accel-plan.md §5). Every figure is timed at
// the task boundary on the app side of the queue.
//
// The thread account needs the region's **chunk count** (the `tasks` passed in),
// never the queue's own maximum concurrency: at most `tasks` callbacks run at
// once - the submitted ones plus the calling thread's own chunk - so the summed
// callback time can never exceed `tasks * wall`. That bound is what makes
//
//   workNs <= capacityNs        (idleNs = capacityNs - workNs >= 0)
//
// hold for *any* decomposition. The chunk count is the decode's own decision -
// the decoder derives it from the region's tile count (doc_agent/cpu-accel-plan.md
// §2) - so the account follows how many threads the region actually asked for
// instead of the width ceiling, and `capacityNs / wallNs` is the average of that
// count.
//
// `waitNs` is deliberately **not** part of that account: it is the tasks' queue
// latency, and a queued task overlaps with the work of the tasks already
// running, so `workNs + waitNs` may exceed capacityNs. It is reported next to
// the account, not inside it.
//
// There are no per-slot terms on purpose: a per-task "idle before submit /
// after finish" only equals thread time when the task count equals the width,
// which is exactly the coupling this account avoids.
struct HmrdpParallelStat {
  unsigned long long regions;  // regions dispatched on the queue (tasks > 1)
  unsigned long long tasks;    // task count summed over those regions
  unsigned long long wallNs;   // region wall clock summed (submit .. all waited)
  unsigned long long capacityNs;  // tasks * wall summed: the thread time asked for
  unsigned long long workNs;   // summed callback time (worker busy)
  unsigned long long waitNs;   // summed queue latency (task start - submission)
};

// Off by default: the clock reads are not free on this platform and a live
// session never displays the figures. The replay turns it on for its own runs,
// exactly like the decoder's probes (HmrdpSetProgSample).
void HmrdpParallelSetProbe(int on);
void HmrdpParallelResetStat(void);
void HmrdpParallelGetStat(struct HmrdpParallelStat* out);

#ifdef __cplusplus
}
#endif

#endif  // HMRDP_PARALLEL_H
