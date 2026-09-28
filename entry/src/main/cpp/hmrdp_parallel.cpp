/*
 * HmRdp - platform parallel executor for the progressive tile decode
 * (hmrdp_parallel.h, doc_agent/gfx-engine.md §7.3).
 */
#include "hmrdp_parallel.h"

#include <cstddef>

#include <ffrt/queue.h>
#include <ffrt/task.h>
#include <ffrt/type_def.h>

#include "hmrdp_decode_tuning.h"

namespace {

// The platform queue is the decode's executor. FFRT is the platform's own task
// runtime, so its scheduling, QoS and core placement follow the system and keep
// following it across system updates, and it is the only executor the decoder
// has (doc_agent/gfx-engine.md §7.3).
constexpr int kUsePlatformExecutor = 1;

// Upper bound on the tasks one call accepts; the decoder's chunk count is capped
// well below this, and a larger request falls back to the caller's executor.
constexpr unsigned int kMaxParallelTasks = 128;

constexpr const char* kQueueName = "hmrdp-tile";
constexpr const char* kTaskName = "hmrdp-chunk";
// The tile decode is on the frame-delivery critical path; ask for the QoS that
// keeps it on the performance cores rather than the default core class.
constexpr ffrt_qos_t kTaskQos = ffrt_qos_user_initiated;

// One chunk. FFRT hands a task a single void*, so the callback gets the chunk's
// function, context and index through this.
struct Item {
  void (*fn)(void*, unsigned int);
  void* ctx;
  unsigned int index;
};

// The executor the calling thread's decoder work belongs to, set by
// DecodeExecutorScope around the decode entry (HmrdpGfxReplayRecv). Null outside
// any pipeline, which makes HmrdpParallelRun fall back to running inline.
thread_local hmrdp::DecodeExecutor* g_currentExecutor = nullptr;

void Thunk(void* arg) {
  Item* item = static_cast<Item*>(arg);
  item->fn(item->ctx, item->index);
}

}  // namespace

namespace hmrdp {

DecodeExecutor::DecodeExecutor() = default;

DecodeExecutor::~DecodeExecutor() {
  // Destroying the queue is safe here: only this executor's owner calls Run(), and
  // by the time the pipeline is destroyed its thread has been joined, so no task
  // can still be in flight on the queue.
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_ != nullptr) {
    ffrt_queue_destroy(static_cast<ffrt_queue_t>(queue_));
    queue_ = nullptr;
    concurrency_ = 0;
  }
}

int DecodeExecutor::Run(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx) {
  if (tasks == 0) {
    return 0;
  }
  if (fn == nullptr || tasks > kMaxParallelTasks) {
    return 1;
  }
  const int width = hmrdp::DecodeThreads();
  if (tasks == 1 || width <= 1) {
    // One callback is not worth a queue round trip, and width 1 has no second
    // thread to hand anything to: the calling thread decodes the whole region.
    for (unsigned int i = 0; i < tasks; ++i) {
      fn(ctx, i);
    }
    return 0;
  }

  // Caller participation: the calling (receiving) thread runs one chunk itself
  // while the queue runs the rest, so the region uses `width` threads. The queue
  // only has to offer `width - 1` workers, which keeps this thread off the idle
  // path and asks the pool for one fewer thread than the width.
  const int queueConcurrency = width - 1;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_ == nullptr || concurrency_ != queueConcurrency) {
    // (Re)create the queue when its concurrency limit changes - that is the width
    // minus one. Only this executor's owner calls Run(), and a previous call has
    // already waited for its own tasks, so nothing is in flight on the old queue.
    if (queue_ != nullptr) {
      ffrt_queue_destroy(static_cast<ffrt_queue_t>(queue_));
      queue_ = nullptr;
      concurrency_ = 0;
    }
    ffrt_queue_attr_t attr;
    if (ffrt_queue_attr_init(&attr) != 0) {
      return 1;
    }
    ffrt_queue_attr_set_max_concurrency(&attr, queueConcurrency);
    ffrt_queue_attr_set_qos(&attr, kTaskQos);
    ffrt_queue_t queue = ffrt_queue_create(ffrt_queue_concurrent, kQueueName, &attr);
    ffrt_queue_attr_destroy(&attr);
    if (queue == nullptr) {
      return 1;
    }
    queue_ = queue;
    concurrency_ = queueConcurrency;
  }
  ffrt_queue_t queue = static_cast<ffrt_queue_t>(queue_);

  ffrt_task_attr_t taskAttr;
  if (ffrt_task_attr_init(&taskAttr) != 0) {
    return 1;
  }
  ffrt_task_attr_set_name(&taskAttr, kTaskName);
  ffrt_task_attr_set_qos(&taskAttr, kTaskQos);

  // The descriptors and handles live on this frame: the waits below return only
  // after the tasks finished, so they outlive the tasks.
  Item items[kMaxParallelTasks];
  ffrt_task_handle_t handles[kMaxParallelTasks];
  // The chunk the caller keeps for itself. Any index works (the homes are equal
  // sized and the tail is stolen either way); the first one keeps the submitted
  // chunks' indices contiguous, which is what the home ranges assume.
  const unsigned int callerIndex = 0;
  for (unsigned int i = 0; i < tasks; ++i) {
    items[i].fn = fn;
    items[i].ctx = ctx;
    items[i].index = i;
    handles[i] = nullptr;
    if (i == callerIndex) {
      continue;  // the calling thread runs this one, below
    }
    handles[i] = ffrt_queue_submit_h_f(queue, Thunk, &items[i], &taskAttr);
    if (handles[i] == nullptr) {
      // A chunk that could not be queued must still run: doing it here keeps the
      // region all-or-nothing (the caller treats a non-zero return as "not done"
      // and would otherwise redo the chunks that did run).
      Thunk(&items[i]);
    }
  }
  ffrt_task_attr_destroy(&taskAttr);

  // The caller's chunk runs after the submissions - so the workers are already
  // busy and this thread starts at most one chunk late - and before the barrier,
  // so the thread is never idle while work is available.
  Thunk(&items[callerIndex]);

  // Barrier for exactly our tasks: wait each handle, so the wall is the slowest
  // chunk and not "every task the process ever submitted".
  for (unsigned int i = 0; i < tasks; ++i) {
    if (handles[i] != nullptr) {
      ffrt_queue_wait(handles[i]);
      ffrt_task_handle_destroy(handles[i]);
    }
  }
  return 0;
}

DecodeExecutorScope::DecodeExecutorScope(DecodeExecutor* executor)
    : previous_(g_currentExecutor) {
  g_currentExecutor = executor;
}

DecodeExecutorScope::~DecodeExecutorScope() {
  g_currentExecutor = previous_;
}

}  // namespace hmrdp

extern "C" int HmrdpParallelAvailable(void) {
  return kUsePlatformExecutor;
}

// The decode width, read by the patched decoder to decide between the serial
// branch and the platform queue (see hmrdp_decode_tuning.h). It is resolved on
// demand, so a machine that comes up with a different core count needs no
// re-arming.
extern "C" unsigned int HmrdpDecodeWidth(void) {
  return static_cast<unsigned int>(hmrdp::DecodeThreads());
}

extern "C" int HmrdpParallelRun(unsigned int tasks, void (*fn)(void*, unsigned int), void* ctx) {
  if (tasks == 0) {
    return 0;
  }
  if (fn == nullptr || tasks > kMaxParallelTasks) {
    return 1;
  }
  hmrdp::DecodeExecutor* executor = g_currentExecutor;
  if (executor != nullptr) {
    return executor->Run(tasks, fn, ctx);
  }
  // No pipeline bound on this thread: decode the whole region here so a caller
  // that is not one of our pipelines still gets its tiles processed.
  for (unsigned int i = 0; i < tasks; ++i) {
    fn(ctx, i);
  }
  return 0;
}
