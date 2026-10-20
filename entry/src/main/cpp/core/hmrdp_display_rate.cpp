#include "hmrdp_display_rate.h"

#include <atomic>
#include <mutex>

#include <window_manager/oh_display_manager.h>

#include "hmrdp_log.h"

namespace hmrdp {

namespace {

std::atomic<int> g_refreshRateHz{0};
// What the UI layer pushed for the display the session window is on; 0 until it pushes anything, which
// keeps the default-display reading in charge.
std::atomic<int> g_windowDisplayHz{0};
std::once_flag g_watchOnce;

void ReadRefreshRate() {
  uint32_t refreshRate = 0;
  if (OH_NativeDisplayManager_GetDefaultDisplayRefreshRate(&refreshRate) == DISPLAY_MANAGER_OK &&
      refreshRate > 0) {
    g_refreshRateHz.store(static_cast<int>(refreshRate));
  }
}

// The refresh period in microseconds, rounded rather than truncated: the slot grid is a pure
// arithmetic progression, so a period that is systematically short would itself drift against the
// display (60 Hz truncated is 0.67 us short per slot, ~40 us per second).
int64_t PeriodUs(int refreshRate) {
  return (1000000 + refreshRate / 2) / refreshRate;
}

// No user data on the display-change callback: the rate is process-wide, so one watcher serves all.
void OnDisplayChanged(uint64_t displayId) {
  (void)displayId;
  ReadRefreshRate();
}

}  // namespace

void SetDisplayRefreshRateHz(int hz) {
  g_windowDisplayHz.store(hz > 0 ? hz : 0);
}

int DisplayRefreshRateHz() {
  const int pushed = g_windowDisplayHz.load();
  if (pushed > 0) {
    return pushed;
  }
  std::call_once(g_watchOnce, []() {
    ReadRefreshRate();
    uint32_t listenerIndex = 0;
    if (OH_NativeDisplayManager_RegisterDisplayChangeListener(OnDisplayChanged, &listenerIndex) !=
        DISPLAY_MANAGER_OK) {
      HMRDP_LOGW("display rate: no change listener; the rate read now is what everything uses");
    }
    HMRDP_LOGI("display rate: %{public}d Hz", g_refreshRateHz.load());
  });
  return g_refreshRateHz.load();
}

bool DisplayBelowFrameRate(int frameRate) {
  const int refreshRate = DisplayRefreshRateHz();
  return refreshRate > 0 && frameRate > refreshRate;
}

bool SlotSchedule::Claim(int64_t nowUs, SlotTolerance tolerance, int64_t* slotUs) {
  const int refreshRate = DisplayRefreshRateHz();
  if (refreshRate <= 0) {
    if (slotUs != nullptr) {
      *slotUs = nowUs;
    }
    return true;
  }
  const int64_t periodUs = PeriodUs(refreshRate);
  const int64_t windowUs = periodUs / static_cast<int64_t>(tolerance);
  const int64_t slot = nextUs_ > nowUs ? nextUs_ : nowUs;
  if (slot - nowUs > windowUs) {
    return false;
  }
  // Advance the grid by whole periods, never onto `nowUs`. Rebasing on `nowUs` writes every frame's
  // lateness into the grid — a wake-up that lands a few hundred microseconds late (or a present that
  // takes a while) then makes the grid tick *slower* than the display, which costs a refresh boundary
  // roughly every second. Periods skipped while the sink was stalled are stepped over without moving
  // the phase, so the grid stays within one period of now and the phase is never re-randomised.
  const int64_t missed = nowUs > nextUs_ ? (nowUs - nextUs_) / periodUs : 0;
  nextUs_ += (missed + 1) * periodUs;
  if (slotUs != nullptr) {
    *slotUs = slot;
  }
  return true;
}

int64_t SlotSchedule::NextSlotUs(int64_t nowUs) const {
  return nextUs_ > nowUs ? nextUs_ : nowUs;
}

int64_t SlotSchedule::WaitUs(int64_t nowUs, SlotTolerance tolerance) const {
  const int refreshRate = DisplayRefreshRateHz();
  if (refreshRate <= 0) {
    return 0;
  }
  const int64_t periodUs = PeriodUs(refreshRate);
  const int64_t openAtUs = nextUs_ - periodUs / static_cast<int64_t>(tolerance);
  return openAtUs > nowUs ? openAtUs - nowUs : 0;
}

}
