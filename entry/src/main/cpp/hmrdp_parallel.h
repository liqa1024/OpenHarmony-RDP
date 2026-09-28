/*
 * HmRdp - platform parallel executor for the progressive tile decode
 * (doc_agent/cpu-accel-plan.md §1).
 *
 * The decode's granularity is a chunk callback owned by FreeRDP's progressive
 * codec; this module is *who runs those callbacks*:
 *
 *  - a FFRT **concurrent queue** (`ffrt_queue_concurrent`) so the thread count
 *    stays under our control. Its maximum concurrency is the decode width **minus
 *    one**: the calling thread runs one chunk itself (caller participation).
 *  - **caller participation**: the receiving thread runs one chunk while the queue
 *    runs the rest, then waits. No thread is idle while tiles are claimable, one
 *    fewer worker is asked of the pool, and the caller's share runs in the
 *    receiving thread's own context (warm caches, its QoS).
 *  - tasks carry a **named attribute with an explicit QoS**; the default QoS's
 *    core class is not the one the frame-delivery critical path wants.
 *  - the barrier is per task: every chunk is submitted with a handle and the
 *    caller waits for exactly those (`ffrt_queue_wait`), never "all work".
 *  - the callbacks never take a non-FFRT lock or block (blocking a worker with a
 *    pthread primitive starves the executor; use FFRT's own primitives if ever
 *    needed).
 *
 * The patched decoder calls this through weak symbols, so a build without these
 * exports (or a device without FFRT) decodes on the receiving thread.
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
}  // extern "C"

#include <mutex>

namespace hmrdp {

// One pipeline's decode executor: its own FFRT concurrent queue, so two sessions
// decoding at once never share - and never destroy - each other's queue
// (doc_agent/architecture.md §4.1). The queue's maximum concurrency is the decode
// width minus one, because the calling thread runs one chunk itself.
class DecodeExecutor {
 public:
  DecodeExecutor();
  ~DecodeExecutor();
  DecodeExecutor(const DecodeExecutor&) = delete;
  DecodeExecutor& operator=(const DecodeExecutor&) = delete;

  // Runs fn(ctx, i) for i in [0, tasks) and returns once all have finished, the
  // calling thread taking one chunk (caller participation). Returns 0 on success;
  // non-zero means the caller must run the tasks itself.
  int Run(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx);

 private:
  std::mutex mutex_;
  void* queue_ = nullptr;  // ffrt_queue_t
  int concurrency_ = 0;
};

// Binds `executor` as the current one for the calling thread until the scope
// ends, so the weakly-linked HmrdpParallelRun - called inline by the patched
// decoder - resolves this pipeline's queue instead of a process-global one. The
// previous binding is restored, so nesting is safe.
class DecodeExecutorScope {
 public:
  explicit DecodeExecutorScope(DecodeExecutor* executor);
  ~DecodeExecutorScope();
  DecodeExecutorScope(const DecodeExecutorScope&) = delete;
  DecodeExecutorScope& operator=(const DecodeExecutorScope&) = delete;

 private:
  DecodeExecutor* previous_ = nullptr;
};

}  // namespace hmrdp
#endif  // __cplusplus

#endif  // HMRDP_PARALLEL_H
