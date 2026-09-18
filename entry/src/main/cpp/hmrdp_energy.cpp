/*
 * HmRdp - CPU energy proxy (see hmrdp_energy.h).
 */
#include "hmrdp_energy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <unistd.h>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// How often the process's own thread-to-core placement is sampled.
constexpr int64_t kEnergyPollUs = 250000;
// Upper bound on the core count the probe follows.
constexpr int kEnergyMaxCores = 64;
// A core counts as "kept busy" by the machine above this fraction of the wall.
constexpr double kEnergyActiveFraction = 0.05;
// Upper bound on the idle states a core may report.
constexpr int kEnergyMaxIdleStates = 16;
// Threads listed by the per-thread CPU log line.
constexpr size_t kEnergyTopThreads = 8;

}  // namespace

// Per-thread cumulative CPU (tid -> user+system ticks). Taken at Begin and End,
// so the figures are that run's, not the process's lifetime.
void EnergyProbe::ReadThreadCpu(std::vector<std::pair<int, uint64_t>>* out) {
  out->clear();
  DIR* dir = opendir("/proc/self/task");
  if (dir == nullptr) {
    return;
  }
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
      continue;
    }
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", ent->d_name);
    FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
      continue;
    }
    char line[512];
    const bool got = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (!got) {
      continue;
    }
    // comm is parenthesized and may contain spaces; the numeric fields start
    // after the last ')'. utime/stime are fields 14/15, i.e. indexes 11/12.
    const char* close = std::strrchr(line, ')');
    if (close == nullptr) {
      continue;
    }
    char* save = nullptr;
    int index = 0;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    for (char* tok = strtok_r(const_cast<char*>(close + 1), " \n", &save); tok != nullptr;
         tok = strtok_r(nullptr, " \n", &save)) {
      if (index == 11) {
        utime = std::strtoull(tok, nullptr, 10);
      } else if (index == 12) {
        stime = std::strtoull(tok, nullptr, 10);
        break;
      }
      index++;
    }
    out->emplace_back(std::atoi(ent->d_name), utime + stime);
  }
  closedir(dir);
}

int64_t EnergyProbe::NowUs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

bool EnergyProbe::ReadResidency(std::vector<Residency>* out) {
  out->assign(kEnergyMaxCores, Residency());
  bool any = false;
  for (int core = 0; core < kEnergyMaxCores; ++core) {
    char path[176];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpufreq/stats/time_in_state", core);
    FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
      continue;
    }
    Residency res;
    long khz = 0;
    unsigned long long ticks = 0;
    while (std::fscanf(f, "%ld %llu", &khz, &ticks) == 2) {
      res.khz.push_back(khz);
      res.ticks.push_back(static_cast<uint64_t>(ticks));
    }
    std::fclose(f);
    if (!res.khz.empty()) {
      res.ok = true;
      any = true;
      (*out)[core] = std::move(res);
    }
  }
  return any;
}

bool EnergyProbe::ReadIdleUs(int core, std::vector<uint64_t>* out) {
  out->clear();
  for (int state = 0; state < kEnergyMaxIdleStates; ++state) {
    char path[176];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/time", core, state);
    FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
      break;
    }
    unsigned long long us = 0;
    const bool read = std::fscanf(f, "%llu", &us) == 1;
    std::fclose(f);
    if (!read) {
      break;
    }
    out->push_back(static_cast<uint64_t>(us));
  }
  return !out->empty();
}

void EnergyProbe::SampleOurCores(std::vector<char>* seen) {
  DIR* dir = opendir("/proc/self/task");
  if (dir == nullptr) {
    return;
  }
  struct dirent* ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
      continue;
    }
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", ent->d_name);
    FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
      continue;
    }
    char line[512];
    const bool got = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (!got) {
      continue;
    }
    // The comm field is parenthesized and may contain spaces, so the fields
    // start after the last ')'. The first one there is field 3 (state); the
    // processor field is 39, i.e. index 36 of that token list.
    const char* close = std::strrchr(line, ')');
    if (close == nullptr) {
      continue;
    }
    char* save = nullptr;
    int index = 0;
    for (char* tok = strtok_r(const_cast<char*>(close + 1), " \n", &save); tok != nullptr;
         tok = strtok_r(nullptr, " \n", &save)) {
      if (index == 36) {
        const int core = std::atoi(tok);
        if (core >= 0 && core < kEnergyMaxCores) {
          (*seen)[core] = 1;
        }
        break;
      }
      index++;
    }
  }
  closedir(dir);
}

void EnergyProbe::Begin() {
  valid_ = false;
  source_ = 0;
  cores_ = 0;
  activeCores_ = 0;
  ourCores_ = 0;
  wallSeconds_ = 0.0;
  coreBusy_ = 0.0;
  c1_ = 0.0;
  e2_ = 0.0;
  beginResid_.clear();
  beginIdle_.clear();
  ourCoresSeen_.assign(kEnergyMaxCores, 0);
  beginUs_ = NowUs();
  lastPollUs_ = beginUs_;
  ReadThreadCpu(&beginThreads_);

  if (!ReadResidency(&beginResid_)) {
    HMRDP_LOGW("energy: no frequency residency source, probe disabled");
    return;
  }
  for (const Residency& r : beginResid_) {
    if (r.ok) {
      cores_++;
    }
  }
  beginIdle_.assign(kEnergyMaxCores, std::vector<uint64_t>());
  bool idleAny = false;
  for (int core = 0; core < kEnergyMaxCores; ++core) {
    if (beginResid_[core].ok && ReadIdleUs(core, &beginIdle_[core])) {
      idleAny = true;
    }
  }
  // cpuidle gives the real per-core busy time; without it the residency
  // histogram has to stand in (time at the minimum frequency = idle).
  source_ = idleAny ? 1 : 2;
  HMRDP_LOGI("energy: begin src=%{public}s cores=%{public}u",
             source_ == 1 ? "cpuidle" : "resid", cores_);
}

void EnergyProbe::Poll() {
  if (beginUs_ == 0) {
    return;
  }
  const int64_t now = NowUs();
  if (now - lastPollUs_ < kEnergyPollUs) {
    return;
  }
  SampleOurCores(&ourCoresSeen_);
  lastPollUs_ = now;
}

void EnergyProbe::End() {
  if (beginUs_ == 0) {
    return;
  }
  const int64_t endUs = NowUs();
  wallSeconds_ = static_cast<double>(endUs - beginUs_) / 1000000.0;

  std::vector<Residency> endResid;
  if (!ReadResidency(&endResid)) {
    HMRDP_LOGW("energy: residency snapshot failed at the end of the run");
    return;
  }
  const long clkTck = sysconf(_SC_CLK_TCK);
  const double hz = static_cast<double>(clkTck > 0 ? clkTck : 100);

  for (int core = 0; core < kEnergyMaxCores; ++core) {
    const Residency& before = beginResid_[core];
    const Residency& after = endResid[core];
    if (!before.ok || !after.ok || before.khz.size() != after.khz.size()) {
      continue;
    }
    // Total zero means the core was offline at the first snapshot; its whole
    // boot history must not be attributed to this run.
    uint64_t beforeTotal = 0;
    for (uint64_t t : before.ticks) {
      beforeTotal += t;
    }
    if (beforeTotal == 0) {
      continue;
    }

    // Clock of the work: mean f and mean f^2 over the frequency steps above this
    // core's minimum (the minimum is where an idle core sits). The f^2 mean
    // feeds the energy proxy; the f mean feeds the cycle proxy.
    const long minKhz = before.khz.front();
    double aboveSeconds = 0.0;
    double weightedF = 0.0;
    double weightedF2 = 0.0;
    for (size_t step = 0; step < before.khz.size(); ++step) {
      if (after.ticks[step] <= before.ticks[step] || before.khz[step] <= minKhz) {
        continue;
      }
      // time_in_state is in USER_HZ ticks.
      const double seconds =
          static_cast<double>(after.ticks[step] - before.ticks[step]) / hz;
      const double ghz = static_cast<double>(before.khz[step]) / 1000000.0;
      aboveSeconds += seconds;
      weightedF += seconds * ghz;
      weightedF2 += seconds * ghz * ghz;
    }

    double busySeconds = 0.0;
    if (source_ == 1) {
      // Real occupancy: the wall time this core was not idle (cpuidle time is
      // in microseconds).
      std::vector<uint64_t> endIdle;
      if (!ReadIdleUs(core, &endIdle)) {
        continue;
      }
      const std::vector<uint64_t>& beginIdle = beginIdle_[core];
      if (endIdle.size() != beginIdle.size()) {
        continue;
      }
      uint64_t idleUs = 0;
      for (size_t state = 0; state < endIdle.size(); ++state) {
        if (endIdle[state] > beginIdle[state]) {
          idleUs += endIdle[state] - beginIdle[state];
        }
      }
      busySeconds = wallSeconds_ - static_cast<double>(idleUs) / 1000000.0;
      if (busySeconds < 0.0) {
        busySeconds = 0.0;
      }
    } else {
      busySeconds = aboveSeconds;
    }

    if (busySeconds > 0.0 && aboveSeconds > 0.0) {
      coreBusy_ += busySeconds;
      c1_ += busySeconds * (weightedF / aboveSeconds);
      e2_ += busySeconds * (weightedF2 / aboveSeconds);
    }
    if (busySeconds > kEnergyActiveFraction * wallSeconds_) {
      activeCores_++;
    }
  }
  for (char seen : ourCoresSeen_) {
    if (seen != 0) {
      ourCores_++;
    }
  }
  valid_ = true;
  HMRDP_LOGI("energy: end src=%{public}s cores=%{public}u active=%{public}u ours=%{public}u "
             "coreBusy=%{public}.1fs e2=%{public}.2f",
             source_ == 1 ? "cpuidle" : "resid", cores_, activeCores_, ourCores_, coreBusy_, e2_);

  // Which threads spent the run's CPU: the platform queue's workers show up as
  // their own seconds alongside the receiving thread, which is how "the decode
  // really spread over N workers" is read from the account rather than inferred
  // from the wall clock (doc_agent/cpu-accel-plan.md §1).
  {
    std::vector<std::pair<int, uint64_t>> endThreads;
    ReadThreadCpu(&endThreads);
    std::vector<std::pair<int, double>> deltas;
    double total = 0.0;
    for (const auto& end : endThreads) {
      for (const auto& begin : beginThreads_) {
        if (begin.first == end.first) {
          if (end.second > begin.second) {
            const double seconds = static_cast<double>(end.second - begin.second) / hz;
            deltas.emplace_back(end.first, seconds);
            total += seconds;
          }
          break;
        }
      }
    }
    std::sort(deltas.begin(), deltas.end(),
              [](const std::pair<int, double>& a, const std::pair<int, double>& b) {
                return a.second > b.second;
              });
    std::string line;
    char buf[48];
    for (size_t i = 0; i < deltas.size() && i < kEnergyTopThreads; ++i) {
      std::snprintf(buf, sizeof(buf), " %d=%.2fs", deltas[i].first, deltas[i].second);
      line += buf;
    }
    HMRDP_LOGI("energy: threads n=%{public}u total=%{public}.1fs%{public}s",
               static_cast<unsigned int>(deltas.size()), total, line.c_str());
  }
  beginThreads_.clear();
  beginUs_ = 0;
}

std::string EnergyProbe::Line(unsigned long long frames) const {
  char buf[288];
  const double perFrame = frames > 0 ? e2_ / static_cast<double>(frames) : 0.0;
  std::snprintf(buf, sizeof(buf),
                "energy: src=%s cores=%u active=%u ours=%u coreBusy=%.1fs C1=%.1f (core*GHz*s) "
                "E2=%.2f (core*GHz^2*s) E2/frame=%.4f",
                source_ == 1 ? "cpuidle" : "resid", cores_, activeCores_, ourCores_, coreBusy_, c1_,
                e2_, perFrame);
  return std::string(buf);
}

}  // namespace hmrdp
