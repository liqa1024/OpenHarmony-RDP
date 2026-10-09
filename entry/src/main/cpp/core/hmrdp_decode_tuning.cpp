#include "hmrdp_decode_tuning.h"

#include <algorithm>
#include <cstdio>

#include <unistd.h>

namespace hmrdp {
namespace {

// Automatic ceiling: the platform queue honours the width, so the cap only keeps
// a very wide machine from queuing more tasks than one region can use.
constexpr int kAutoCap = 16;
constexpr int kMinWorkers = 1;

// Reads cpuinfo_max_freq per CPU to find the performance cluster; returns 0 when
// /sys is denied (sandbox) or the SoC is homogeneous.
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
  if (count == 0 || maxKhz <= 0 || count < cores) {
    return 0;
  }
  int perf = 0;
  for (int i = 0; i < count; ++i) {
    if (values[i] * 10 >= maxKhz * 8) {
      ++perf;
    }
  }
  if (perf == count) {
    return 0;
  }
  return perf;
}

}  // namespace

int DecodeCpuCount() {
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  return cores > 0 ? static_cast<int>(cores) : 1;
}

int DecodeThreads() {
  const int cores = DecodeCpuCount();
  return std::max(kMinWorkers, std::min(cores, kAutoCap));
}

std::string CpuFreqInfo() {
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
  const int perf = ProbePerfCores(cores);
  char buf[160];
  std::snprintf(buf, sizeof(buf), "workers=%d (auto, perf-cores=%d[%s] cores=%d)", DecodeThreads(),
                perf, perf > 0 ? "known" : "unknown", cores);
  return std::string(buf);
}

}  // namespace hmrdp
