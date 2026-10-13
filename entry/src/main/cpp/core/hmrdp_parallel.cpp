#include "hmrdp_parallel.h"

#include <cstddef>

#include <ffrt/queue.h>
#include <ffrt/task.h>
#include <ffrt/type_def.h>

#include "hmrdp_decode_tuning.h"

namespace {

constexpr int kUsePlatformExecutor = 1;

// Upper bound on the tasks one call accepts; a larger request falls back to the
// caller's executor.
constexpr unsigned int kMaxParallelTasks = 128;

constexpr const char* kQueueName = "hmrdp-tile";
constexpr const char* kTaskName = "hmrdp-chunk";
// The tile decode is on the frame-delivery critical path; ask for the QoS that
// keeps it on the performance cores rather than the default core class.
constexpr ffrt_qos_t kTaskQos = ffrt_qos_user_initiated;

struct Item {
  void (*fn)(void*, unsigned int);
  void* ctx;
  unsigned int index;
};

thread_local hmrdp::DecodeExecutor* g_currentExecutor = nullptr;

void Thunk(void* arg) {
  Item* item = static_cast<Item*>(arg);
  item->fn(item->ctx, item->index);
}

}  // namespace

namespace hmrdp {

DecodeExecutor::DecodeExecutor() = default;

DecodeExecutor::~DecodeExecutor() {
  // Only this executor's owner calls Run(), and by the time the pipeline is
  // destroyed its thread has been joined, so no task can still be in flight.
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
    for (unsigned int i = 0; i < tasks; ++i) {
      fn(ctx, i);
    }
    return 0;
  }

  const int queueConcurrency = width - 1;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_ == nullptr || concurrency_ != queueConcurrency) {
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
  const unsigned int callerIndex = 0;
  for (unsigned int i = 0; i < tasks; ++i) {
    items[i].fn = fn;
    items[i].ctx = ctx;
    items[i].index = i;
    handles[i] = nullptr;
    if (i == callerIndex) {
      continue;
    }
    handles[i] = ffrt_queue_submit_h_f(queue, Thunk, &items[i], &taskAttr);
    if (handles[i] == nullptr) {
      Thunk(&items[i]);
    }
  }
  ffrt_task_attr_destroy(&taskAttr);

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

// Read by the patched decoder to choose between the serial branch and the platform queue.
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
  for (unsigned int i = 0; i < tasks; ++i) {
    fn(ctx, i);
  }
  return 0;
}
