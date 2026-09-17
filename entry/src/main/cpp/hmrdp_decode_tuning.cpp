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

// Exported by the patched libwinpr (pool.c): the request is stored and applied
// by the decoder itself at a Progressive message boundary, so changing it while
// a stream is decoding can never tear down workers with work in flight. Weak, so
// nothing here is required for a stock FreeRDP to link - it then keeps its own
// built-in default (min(cores, 4)).
extern "C" void HmrdpSetDecodeThreads(unsigned int workers) __attribute__((weak));

namespace hmrdp {
namespace {

// Automatic ceiling. The tile decode's pool section reaches its plateau after a
// couple of workers and does not improve further: past that point the frame is
// bounded by the parts that stay on the receiving thread (input parse, the
// tile-to-surface copies, present) and by the GPU's copy of the dirty area, so
// each extra worker is just another core woken for every Progressive message
// (doc_agent/cpu-accel-plan.md §1/§2). The automatic choice therefore stays
// small; the device's cluster count can only pull it further down. As for any
// worker-count choice, the energy comparison is `cpu=` at a given `cpuKHz=` band,
// never frame time alone.
constexpr int kAutoCap = 4;
// Manual range.
constexpr int kMaxWorkers = 8;
constexpr int kMinWorkers = 1;

// 0 keeps the stored/manual choice (or the automatic one) in effect. The value
// is process-wide and applied at a Progressive message boundary; the settings
// slider and the replay page's 「线程」 row both drive it. The automatic end
// stays the default (doc_agent/cpu-accel-plan.md §1/§2): which end costs less
// energy depends on the duty cycle, and that trade is read from `cpu=` together
// with the `cpuKHz=` band the run actually got, never from the CPU seconds
// alone.
constexpr int kPinnedWorkers = 0;

// 0 = automatic.
int g_requested = 0;

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

void Forward(int workers) {
  if (HmrdpSetDecodeThreads != nullptr) {
    HmrdpSetDecodeThreads(static_cast<unsigned int>(workers));
  }
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
  // The performance cores are the ones the decode can actually use; on a device
  // that does not expose its clusters, the core count is the best proxy. Either
  // way the result is capped at kAutoCap, where the measured curve is flat.
  const int perf = DecodePerfCores();
  const int wanted = perf > 0 ? perf : DecodeCpuCount();
  return Clamp(std::min(wanted, kAutoCap));
}

int DecodeThreads() {
  if (kPinnedWorkers > 0) {
    return kPinnedWorkers;
  }
  return g_requested > 0 ? Clamp(g_requested) : AutoDecodeThreads();
}

void SetDecodeThreads(int workers) {
  g_requested = workers > 0 ? Clamp(workers) : 0;
  Forward(DecodeThreads());
  HMRDP_LOGI("decode threads: %{public}s", DecodeThreadsInfo().c_str());
}

void ApplyStoredDecodeThreads() {
  Forward(DecodeThreads());
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
  char buf[160];
  if (kPinnedWorkers > 0) {
    std::snprintf(buf, sizeof(buf),
                  "workers=%d (pinned serial: the parallel pool is not tuned for this platform, "
                  "see doc_agent/cpu-accel-plan.md §1; cores=%d)",
                  DecodeThreads(), cores);
    return std::string(buf);
  }
  const char* perfText = perf > 0 ? "known" : "unknown";
  if (g_requested > 0) {
    std::snprintf(buf, sizeof(buf), "workers=%d (manual, perf-cores=%d[%s] cores=%d)",
                  DecodeThreads(), perf, perfText, cores);
  } else {
    std::snprintf(buf, sizeof(buf), "workers=%d (auto, perf-cores=%d[%s] cores=%d)",
                  DecodeThreads(), perf, perfText, cores);
  }
  return std::string(buf);
}

}  // namespace hmrdp
