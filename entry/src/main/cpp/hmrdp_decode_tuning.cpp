/*
 * HmRdp - Progressive tile decode worker count (see hmrdp_decode_tuning.h).
 */
#include "hmrdp_decode_tuning.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// Automatic ceiling. The platform queue honours the requested width, so the
// automatic choice is the whole machine - there is no measured knee below the
// core count to stop at. The gain per worker flattens, but it does not turn
// negative, and the per-run energy proxy has been flat to slightly better with
// more width (doc_agent/cpu-accel-plan.md §0/§2).
constexpr int kAutoCap = 16;
// Manual range (the settings slider and the replay page's 「线程」 row).
constexpr int kMaxWorkers = 8;
constexpr int kMinWorkers = 1;

// 0 = automatic (the whole machine, capped by kAutoCap); 1..8 = manual. The value
// is process-wide and applied at a Progressive message boundary; the settings
// slider and the replay page's 「线程」 row both drive it
// (doc_agent/cpu-accel-plan.md §0).
int g_requested = 0;

// Dev A/B for the tile work decomposition (see the header): the default is the
// home + tail-steal partition, which keeps a worker on contiguous memory while
// the tail stays block-granular. 0 selects the plain shared claim cursor; 2 is
// home plus "route width 1 through the platform queue" (a dev probe, see
// hmrdp_parallel.h).
int g_parallelMode = 1;

// Reads cpuinfo_max_freq once: the count of CPUs at (nearly) the top frequency,
// i.e. the performance cluster. Returns 0 when the device does not expose it
// (the app sandbox may deny /sys, and a homogeneous SoC has one value for all).
int ProbePerfCores(int cores) {
  if (cores <= 0) {
    return 0;
  }
  long maxKhz = 0;
  long values[64];
  int count = 0;
  for (int cpu = 0; cpu < cores && count < static_cast<int>(sizeof(values) / sizeof(values[0]));
       ++cpu) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
      continue;
    }
    long khz = 0;
    const bool read = std::fscanf(f, "%ld", &khz) == 1;
    std::fclose(f);
    if (!read || khz <= 0) {
      continue;
    }
    values[count++] = khz;
    maxKhz = std::max(maxKhz, khz);
  }
  // Fewer samples than CPUs means the probe is not describing this device.
  if (count == 0 || maxKhz <= 0 || count < cores) {
    return 0;
  }
  int perf = 0;
  for (int i = 0; i < count; ++i) {
    if (values[i] * 10 >= maxKhz * 8) {
      ++perf;
    }
  }
  // All cores at the same frequency is a homogeneous SoC: no cluster to prefer.
  if (perf == count) {
    return 0;
  }
  return perf;
}

int Clamp(int workers) {
  return std::max(kMinWorkers, std::min(kMaxWorkers, workers));
}

}  // namespace

int DecodeCpuCount() {
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  return cores > 0 ? static_cast<int>(cores) : 1;
}

int DecodePerfCores() {
  return ProbePerfCores(DecodeCpuCount());
}

int AutoDecodeThreads() {
  // Automatic = the whole machine, capped only by kAutoCap. The decode wall
  // scales with the width now that the width is honoured, and the per-run energy
  // proxy does not get worse with more width (doc_agent/cpu-accel-plan.md §0/§2).
  const int cores = DecodeCpuCount();
  return std::max(kMinWorkers, std::min(cores, kAutoCap));
}

int DecodeThreads() {
  return g_requested > 0 ? Clamp(g_requested) : AutoDecodeThreads();
}

void SetDecodeThreads(int workers) {
  g_requested = workers > 0 ? Clamp(workers) : 0;
  HMRDP_LOGI("decode threads: %{public}s", DecodeThreadsInfo().c_str());
}

void SetParallelMode(int mode) {
  // 0 = shared cursor, 1 = home + tail steal (the default), 2 = home + route
  // width 1 through the platform queue (dev probe, see hmrdp_parallel.h),
  // 3 = home + tail steal **without** caller participation - the A/B control for
  //     it: same decomposition, only the calling thread's chunk moves back to the
  //     queue (see HmrdpParallelRun).
  g_parallelMode = (mode >= 0 && mode <= 3) ? mode : 0;
  HMRDP_LOGI("decode parallel mode: %{public}s", DecodeThreadsInfo().c_str());
}

int ParallelMode() {
  return g_parallelMode;
}

std::string CpuFreqInfo() {
  // Same sysfs shape as ProbePerfCores: the app sandbox usually allows it, and a
  // device that does not expose it simply reports nothing.
  const int cores = DecodeCpuCount();
  long lo = -1;
  long hi = 0;
  for (int cpu = 0; cpu < cores; ++cpu) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
      continue;
    }
    long khz = 0;
    const bool read = std::fscanf(f, "%ld", &khz) == 1;
    std::fclose(f);
    if (!read || khz <= 0) {
      continue;
    }
    if (lo < 0 || khz < lo) {
      lo = khz;
    }
    if (khz > hi) {
      hi = khz;
    }
  }
  if (lo < 0) {
    return std::string();
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%ld-%ld", lo, hi);
  return std::string(buf);
}

std::string DecodeThreadsInfo() {
  const int cores = DecodeCpuCount();
  const int perf = DecodePerfCores();
  char buf[200];
  const char* perfText = perf > 0 ? "known" : "unknown";
  const char* modeText = "normal";
  if (g_parallelMode == 1) {
    modeText = "home-steal";
  } else if (g_parallelMode == 2) {
    modeText = "force-queue";
  } else if (g_parallelMode == 3) {
    modeText = "home-steal-no-participate";
  }
  if (g_requested > 0) {
    std::snprintf(buf, sizeof(buf), "workers=%d (manual, mode=%s, perf-cores=%d[%s] cores=%d)",
                  DecodeThreads(), modeText, perf, perfText, cores);
  } else {
    std::snprintf(buf, sizeof(buf), "workers=%d (auto, mode=%s, perf-cores=%d[%s] cores=%d)",
                  DecodeThreads(), modeText, perf, perfText, cores);
  }
  return std::string(buf);
}

}  // namespace hmrdp
