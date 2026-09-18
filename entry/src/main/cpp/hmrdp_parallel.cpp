/*
 * HmRdp - platform parallel executor (see hmrdp_parallel.h).
 */
#include "hmrdp_parallel.h"

#include <atomic>
#include <cstddef>

#include <ffrt/queue.h>
#include <ffrt/task.h>
#include <ffrt/type_def.h>

#include "hmrdp_decode_tuning.h"

namespace {

// The platform queue is the decode's executor. It is not chosen for a marginal
// measurement win: FFRT is the platform's own task runtime, so its scheduling,
// QoS and core placement follow the system and keep following it across system
// updates, and it is the only executor the decoder has
// (doc_agent/cpu-accel-plan.md §0/§1).
constexpr int kUsePlatformExecutor = 1;

// Upper bound on the tasks one call accepts; the decoder's chunk count is capped
// well below this, and a larger request falls back to the caller's executor.
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

std::atomic<unsigned int> g_active{0};
std::atomic<unsigned int> g_maxActive{0};

ffrt_queue_t g_queue = nullptr;
int g_queueConcurrency = 0;

// (Re)creates the concurrent queue when the configured width changes. Called
// only with no task in flight (HmrdpParallelRun waits for all of them), so it is
// safe to destroy the previous queue here.
int EnsureQueue(int concurrency) {
  if (g_queue != nullptr && g_queueConcurrency == concurrency) {
    return 0;
  }
  if (g_queue != nullptr) {
    ffrt_queue_destroy(g_queue);
    g_queue = nullptr;
    g_queueConcurrency = 0;
  }
  ffrt_queue_attr_t attr;
  if (ffrt_queue_attr_init(&attr) != 0) {
    return 1;
  }
  ffrt_queue_attr_set_max_concurrency(&attr, concurrency);
  ffrt_queue_attr_set_qos(&attr, kTaskQos);
  g_queue = ffrt_queue_create(ffrt_queue_concurrent, kQueueName, &attr);
  ffrt_queue_attr_destroy(&attr);
  if (g_queue == nullptr) {
    return 1;
  }
  g_queueConcurrency = concurrency;
  return 0;
}

void Thunk(void* arg) {
  Item* item = static_cast<Item*>(arg);
  const unsigned int now = g_active.fetch_add(1) + 1;
  unsigned int prevMax = g_maxActive.load();
  while (now > prevMax && !g_maxActive.compare_exchange_weak(prevMax, now)) {
  }
  item->fn(item->ctx, item->index);
  g_active.fetch_sub(1);
}

}  // namespace

extern "C" int HmrdpParallelAvailable(void) {
  return kUsePlatformExecutor;
}

// The configured decode width, read by the patched decoder to decide between
// the serial branch and the platform queue (see hmrdp_decode_tuning.h). The
// width is resolved on demand, so a settings change needs no re-arming.
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
  // One callback is not worth a queue round trip; the decoder only gets here with
  // at least two workers, but a single-chunk region is common.
  const int concurrency = hmrdp::DecodeThreads();
  if (tasks == 1 || concurrency <= 1) {
    fn(ctx, 0);
    return 0;
  }
  if (EnsureQueue(concurrency) != 0) {
    return 1;
  }

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
  for (unsigned int i = 0; i < tasks; ++i) {
    items[i].fn = fn;
    items[i].ctx = ctx;
    items[i].index = i;
    handles[i] = ffrt_queue_submit_h_f(g_queue, Thunk, &items[i], &taskAttr);
    if (handles[i] == nullptr) {
      // A chunk that could not be queued must still run: doing it here keeps the
      // region all-or-nothing (the caller treats a non-zero return as "not done"
      // and would otherwise redo the chunks that did run).
      Thunk(&items[i]);
    }
  }
  ffrt_task_attr_destroy(&taskAttr);

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

extern "C" unsigned int HmrdpParallelTakeMaxConcurrency(void) {
  return g_maxActive.exchange(0);
}
