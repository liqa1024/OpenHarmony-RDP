/*
 * HmRdp - CPU energy proxy for the replay thread-count A/B (hmrdp_replay.cpp).
 *
 * The device exposes no power/energy counter (no /sys/class/power_supply, no
 * /sys/class/powercap, and the power/battery system abilities dump nothing), and
 * the app sandbox denies /proc/stat, so the proxy is built from the two sources
 * that are readable:
 *
 *   1. cpufreq/stats/time_in_state - per-core time-in-frequency (exact, two
 *      snapshots per run). Frequency alone must not be read as "working": the
 *      SoC raises whole clusters, so it supplies the *clock* of the work, not the
 *      amount of it.
 *   2. the per-core cpuidle state times - per-core idle time, so busy =
 *      wall - idle. This is the real per-core occupancy, which is what says how
 *      many cores a configured worker count actually keeps busy. Where cpuidle is
 *      not readable the residency histogram above stands in for it (time at the
 *      core's minimum frequency = idle), and Line() says which source was used.
 *
 *     E2 = sum over cores: busy_seconds(core) * f2avg(core)
 *          f2avg = sum over that core's frequency steps above its minimum of
 *                  (time * f_GHz^2) / (time above the minimum)
 *
 * So a core contributes only its truly busy time, weighted by the clock it ran
 * at while working. Why f^2: dynamic power is P ~ V^2 * f and the voltage curve
 * is unknown, so one fixed exponent keeps every compared run on the same basis
 * (the "V ~ sqrt(f)" midpoint). This is the number that separates "one core at a
 * high clock" from "N cores at a lower clock" - the question a worker-count A/B
 * asks, and the one `cpu=` alone cannot answer (doc_agent/cpu-accel-plan.md §2).
 *
 * It also counts, per run, how many cores this process's own threads were seen
 * on (`ours`), which is the direct answer to "with N workers, how many cores
 * actually work".
 *
 * The probe is installed by the replay only: a live session reports the decode
 * cost through the per-frame meter instead.
 */
#ifndef HMRDP_ENERGY_H
#define HMRDP_ENERGY_H

#include <cstdint>
#include <string>
#include <vector>

namespace hmrdp {

class EnergyProbe {
 public:
  // Opens the window (snapshots the sources) and resets the totals.
  void Begin();
  // Samples the process's own thread-to-core placement if the interval elapsed;
  // cheap enough to call once per frame.
  void Poll();
  // Closes the window and freezes the totals.
  void End();

  bool Valid() const { return valid_; }
  // e.g. "energy: src=cpuidle cores=14 active=3 ours=2 coreBusy=21.4s
  //       C1=33.9 (core*GHz*s) E2=18.73 (core*GHz^2*s) E2/frame=0.0960"
  // `active` = cores the machine kept busy (busy > 5% of the wall), `ours` =
  // cores this process's threads were seen on, `coreBusy` = their busy time.
  // `C1` is the same integral with power ~ f instead of f^2, i.e. a *cycle*
  // proxy: divided by the work done it gives a per-unit-of-work cost that the
  // run's frequency band cannot distort, so "one core at 2.7GHz" and "many cores
  // at 1.2GHz" become comparable in how much work they really did
  // (doc_agent/cpu-accel-plan.md §1).
  std::string Line(unsigned long long frames) const;

 private:
  // One core's time-in-frequency histogram (frequencies ascending, kHz).
  struct Residency {
    std::vector<long> khz;
    std::vector<uint64_t> ticks;
    bool ok = false;
  };

  static bool ReadResidency(std::vector<Residency>* out);
  static bool ReadIdleUs(int core, std::vector<uint64_t>* out);
  static void SampleOurCores(std::vector<char>* seen);
  // tid -> cumulative user+system ticks, for the per-run delta.
  static void ReadThreadCpu(std::vector<std::pair<int, uint64_t>>* out);
  static int64_t NowUs();

  bool valid_ = false;
  // 0 = none, 1 = cpuidle busy + residency clock, 2 = residency only.
  int source_ = 0;
  int64_t beginUs_ = 0;
  int64_t lastPollUs_ = 0;
  uint32_t cores_ = 0;
  uint32_t activeCores_ = 0;
  uint32_t ourCores_ = 0;
  double wallSeconds_ = 0.0;
  double coreBusy_ = 0.0;
  double c1_ = 0.0;
  double e2_ = 0.0;
  std::vector<Residency> beginResid_;
  std::vector<std::vector<uint64_t>> beginIdle_;
  std::vector<char> ourCoresSeen_;
  std::vector<std::pair<int, uint64_t>> beginThreads_;
};

}  // namespace hmrdp

#endif  // HMRDP_ENERGY_H
