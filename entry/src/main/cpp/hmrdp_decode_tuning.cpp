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

// Automatic ceiling. Measured on the test device (video sample, CPU route):
// 2, 3, 4, 6 and 8 workers all give the same frame time (24.0-25.1 ms) and the
// same process CPU (~31 s/run), while a single worker is 2.6x slower at less
// than half the CPU (65.0 ms / 13.7 s). The decode therefore stops scaling after
// a couple of workers, and each extra one is a core woken for every Progressive
// message - so the automatic choice stays small and the device's cluster count
// can only pull it further down (doc_agent/gfx-engine.md §3).
constexpr int kAutoCap = 4;
// Manual range.
constexpr int kMaxWorkers = 8;
constexpr int kMinWorkers = 1;

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
