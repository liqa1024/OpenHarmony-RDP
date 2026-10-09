#ifndef HMRDP_PARALLEL_H
#define HMRDP_PARALLEL_H

#ifdef __cplusplus
extern "C" {
#endif

int HmrdpParallelAvailable(void);

// The decode width (>= 1). The patched decoder reads this to choose between its
// serial branch and the platform queue; resolved on demand, so a machine with a
// different core count needs no re-arming.
unsigned int HmrdpDecodeWidth(void);

// Runs fn(ctx, i) for i in [0, tasks) and returns once all have finished. The
// calling thread runs one chunk itself and the queue runs the rest, so the queue
// only needs width-1 workers. fn must not block a worker with a non-FFRT lock.
// Returns 0 on success; non-zero means the caller must fall back to its own
// executor. fn is a plain function pointer because the caller is C.
int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx);

#ifdef __cplusplus
}  // extern "C"

#include <mutex>

namespace hmrdp {

// One pipeline's decode executor: its own FFRT concurrent queue, so two sessions
// decoding at once never share - and never destroy - each other's queue. The
// queue's maximum concurrency is the decode width minus one, because the calling
// thread runs one chunk itself.
class DecodeExecutor {
 public:
  DecodeExecutor();
  ~DecodeExecutor();
  DecodeExecutor(const DecodeExecutor&) = delete;
  DecodeExecutor& operator=(const DecodeExecutor&) = delete;

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
