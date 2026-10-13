#ifndef HMRDP_PARALLEL_H
#define HMRDP_PARALLEL_H

#ifdef __cplusplus
extern "C" {
#endif

int HmrdpParallelAvailable(void);

// Decode width (>= 1). The patched decoder reads this to pick its serial branch vs the platform
// queue; resolved on demand, so a different core count needs no re-arming.
unsigned int HmrdpDecodeWidth(void);

// Runs fn(ctx, i) for i in [0, tasks); the calling thread runs one chunk and the queue the rest (width-1 workers).
// fn must not block a worker with a non-FFRT lock; non-zero means the caller must fall back to its own executor.
int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx);

#ifdef __cplusplus
}

#include <mutex>

namespace hmrdp {

// One pipeline's own FFRT concurrent queue, so two sessions decoding at once never share/destroy
// each other's queue; max concurrency is the decode width minus one (the caller runs one chunk).
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

// Binds `executor` as current for the calling thread until scope end, so the weakly-linked
// HmrdpParallelRun (called inline by the patched decoder) resolves this pipeline's queue (previous binding restored).
class DecodeExecutorScope {
 public:
  explicit DecodeExecutorScope(DecodeExecutor* executor);
  ~DecodeExecutorScope();
  DecodeExecutorScope(const DecodeExecutorScope&) = delete;
  DecodeExecutorScope& operator=(const DecodeExecutorScope&) = delete;

 private:
  DecodeExecutor* previous_ = nullptr;
};

}
#endif

#endif
