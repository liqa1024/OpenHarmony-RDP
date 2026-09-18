/*
 * HmRdp - platform parallel executor (see hmrdp_parallel.h).
 */
#include "hmrdp_parallel.h"

#include <atomic>
#include <cstddef>
#include <ctime>

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
  // Probe only: when this task was handed to the queue, so its queue wait can be
  // told from its execution (see HmrdpParallelStat). 0 while the probe is off.
  unsigned long long submitNs;
};

std::atomic<unsigned int> g_active{0};
std::atomic<unsigned int> g_maxActive{0};

// Dev-only parallel-section accounting (HmrdpParallelStat). All default 0 and
// are untouched while the probe is off, so a live session pays nothing.
std::atomic<int> g_probe{0};
std::atomic<unsigned long long> g_regions{0};
std::atomic<unsigned long long> g_tasks{0};
std::atomic<unsigned long long> g_wallNs{0};
std::atomic<unsigned long long> g_capacityNs{0};
std::atomic<unsigned long long> g_workNs{0};
std::atomic<unsigned long long> g_waitNs{0};

inline bool ProbeOn() {
  return g_probe.load(std::memory_order_relaxed) != 0;
}

inline unsigned long long NowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (static_cast<unsigned long long>(ts.tv_sec) * 1000000000ull) +
         static_cast<unsigned long long>(ts.tv_nsec);
}

ffrt_queue_t g_queue = nullptr;
int g_queueConcurrency = 0;

// (Re)creates the concurrent queue when its concurrency limit changes - that is
// the width minus one while the caller participates, or the full width for the
// force-queue probe. Called only with no task in flight (HmrdpParallelRun waits
// for all of them), so it is safe to destroy the previous queue here.
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
  // The probe reads the clock at the task's own boundaries. This is the worker's
  // time, so summing it over the tasks says how much of the offered thread time
  // was spent decoding rather than queued or idle (HmrdpParallelStat).
  const bool probe = ProbeOn();
  const unsigned long long startNs = probe ? NowNs() : 0;
  const unsigned int now = g_active.fetch_add(1) + 1;
  unsigned int prevMax = g_maxActive.load();
  while (now > prevMax && !g_maxActive.compare_exchange_weak(prevMax, now)) {
  }
  item->fn(item->ctx, item->index);
  g_active.fetch_sub(1);
  if (probe) {
    const unsigned long long endNs = NowNs();
    // `submitNs == 0` means the probe came on after this task was submitted; its
    // wait is then unknown, not zero, so it is left out instead of charged to the
    // region. `startNs >= submitNs` cannot fail for a task submitted after the
    // probe was already on.
    if (startNs >= item->submitNs && item->submitNs != 0) {
      g_waitNs.fetch_add(startNs - item->submitNs, std::memory_order_relaxed);
    }
    g_workNs.fetch_add(endNs - startNs, std::memory_order_relaxed);
  }
}

}  // namespace

extern "C" int HmrdpParallelAvailable(void) {
  return kUsePlatformExecutor;
}

// Dev A/B: 1 = per-task contiguous home ranges with tail stealing (see the patch
// note in native/scripts/patch-freerdp.ps1 step 20), 0 = the shared claim cursor.
// Mode 2 is home too - it only adds "force the queue at width 1" (below).
extern "C" int HmrdpTileHomeMode(void) {
  return hmrdp::ParallelMode() >= 1 ? 1 : 0;
}

// Dev probe (patched decoder, native/scripts/patch-steps/26-...): non-zero makes
// even width 1 decode on the platform queue - one task on a concurrency-1 queue -
// instead of the receiving thread's serial loop, so the executor's own cost can
// be measured against that branch. Off in normal operation.
extern "C" int HmrdpParallelForceQueue(void) {
  return hmrdp::ParallelMode() == 2 ? 1 : 0;
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
  // The configured width is both the number of threads one region may use and
  // the factor the `par` account is built on (HmrdpParallelStat), so it is read
  // once here and never replaced by the queue's own concurrency below.
  const int width = hmrdp::DecodeThreads();
  // The dev force-queue probe (native/scripts/patch-steps/26) exists to price the
  // executor's own round trip against the inline branch, so it keeps the old
  // "submit everything, the caller only waits" shape: caller participation would
  // remove exactly the cost it measures. It also lets width 1 reach the queue.
  const bool forceQueue = hmrdp::ParallelMode() == 2;
  if (!forceQueue && (tasks == 1 || width <= 1)) {
    // One callback is not worth a queue round trip, and width 1 has no second
    // thread to hand anything to: the calling thread decodes the whole region.
    fn(ctx, 0);
    return 0;
  }
  // Caller participation: the calling (receiving) thread runs one chunk itself
  // while the queue runs the rest. The region still uses `width` threads - one of
  // them this thread, which would otherwise sit in the barrier - and the queue
  // only has to offer `width - 1` workers. That keeps this thread off the idle
  // path and asks the worker pool for one fewer thread, which matters where the
  // pool is smaller than the width (the auto width is the online core count).
  // Mode 3 is the A/B control: the same decomposition and width, but the chunk
  // goes to the queue like the rest, so only participation differs between the
  // two runs.
  const bool participate = !forceQueue && hmrdp::ParallelMode() != 3;
  const int queueConcurrency = participate ? (width > 1 ? width - 1 : 1) : width;
  // The chunk the caller keeps for itself. Any index works (the homes are equal
  // sized and the tail is stolen either way); the first one keeps the submitted
  // chunks' indices contiguous, which is what the home ranges assume.
  const unsigned int callerIndex = 0;
  if (EnsureQueue(queueConcurrency) != 0) {
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
  // Region span from the receiving thread's side. Together with each task's own
  // submit/start/end this makes the region's `tasks * wall` account whole, with
  // no bucket left to guess at (HmrdpParallelStat).
  const bool probe = ProbeOn();
  const unsigned long long wallStartNs = probe ? NowNs() : 0;
  for (unsigned int i = 0; i < tasks; ++i) {
    items[i].fn = fn;
    items[i].ctx = ctx;
    items[i].index = i;
    items[i].submitNs = probe ? NowNs() : 0;
    handles[i] = nullptr;
    if (participate && i == callerIndex) {
      // The caller's own chunk is not submitted. Its queue latency is not a
      // thing, so the probe must not charge one: Thunk skips a zero submitNs
      // instead of folding the submission of the other chunks into `wait`.
      items[i].submitNs = 0;
      continue;
    }
    handles[i] = ffrt_queue_submit_h_f(g_queue, Thunk, &items[i], &taskAttr);
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
  if (participate) {
    Thunk(&items[callerIndex]);
  }

  // Barrier for exactly our tasks: wait each handle, so the wall is the slowest
  // chunk and not "every task the process ever submitted".
  for (unsigned int i = 0; i < tasks; ++i) {
    if (handles[i] != nullptr) {
      ffrt_queue_wait(handles[i]);
      ffrt_task_handle_destroy(handles[i]);
    }
  }
  if (probe && wallStartNs != 0) {
    // capacity is K * wall with K = this region's *width* (the thread count the
    // region may use), not the queue's max_concurrency: with the caller
    // participating the queue offers width-1 workers, but the calling thread is
    // the width-th, so at most `width` callbacks run at once either way. See
    // HmrdpParallelStat for why neither the task count nor the queue limit is
    // what the thread account is built on.
    const unsigned long long wallNs = NowNs() - wallStartNs;
    g_regions.fetch_add(1, std::memory_order_relaxed);
    g_tasks.fetch_add(tasks, std::memory_order_relaxed);
    g_wallNs.fetch_add(wallNs, std::memory_order_relaxed);
    g_capacityNs.fetch_add(wallNs * static_cast<unsigned long long>(width),
                           std::memory_order_relaxed);
  }
  return 0;
}

extern "C" unsigned int HmrdpParallelTakeMaxConcurrency(void) {
  return g_maxActive.exchange(0);
}

extern "C" void HmrdpParallelSetProbe(int on) {
  g_probe.store(on ? 1 : 0, std::memory_order_relaxed);
}

extern "C" void HmrdpParallelResetStat(void) {
  g_regions.store(0, std::memory_order_relaxed);
  g_tasks.store(0, std::memory_order_relaxed);
  g_wallNs.store(0, std::memory_order_relaxed);
  g_capacityNs.store(0, std::memory_order_relaxed);
  g_workNs.store(0, std::memory_order_relaxed);
  g_waitNs.store(0, std::memory_order_relaxed);
}

extern "C" void HmrdpParallelGetStat(struct HmrdpParallelStat* out) {
  if (out == nullptr) {
    return;
  }
  out->regions = g_regions.load(std::memory_order_relaxed);
  out->tasks = g_tasks.load(std::memory_order_relaxed);
  out->wallNs = g_wallNs.load(std::memory_order_relaxed);
  out->capacityNs = g_capacityNs.load(std::memory_order_relaxed);
  out->workNs = g_workNs.load(std::memory_order_relaxed);
  out->waitNs = g_waitNs.load(std::memory_order_relaxed);
}
