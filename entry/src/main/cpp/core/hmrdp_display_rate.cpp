#include "hmrdp_display_rate.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>

#include <window_manager/oh_display_manager.h>

#include "hmrdp_log.h"

namespace hmrdp {

namespace {

std::atomic<int> g_refreshRateHz{0};
std::once_flag g_watchOnce;

// A frame arriving more than half a period before the next slot is surplus: the display cannot show
// it, and taking the slot would cost the frame that could be shown.
constexpr int64_t kSlotToleranceDivisor = 2;

int64_t NowUs() {
  return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void ReadRefreshRate() {
  uint32_t refreshRate = 0;
  if (OH_NativeDisplayManager_GetDefaultDisplayRefreshRate(&refreshRate) == DISPLAY_MANAGER_OK &&
      refreshRate > 0) {
    g_refreshRateHz.store(static_cast<int>(refreshRate));
  }
}

// No user data on the display-change callback: the rate is process-wide, so one watcher serves all.
void OnDisplayChanged(uint64_t displayId) {
  (void)displayId;
  ReadRefreshRate();
}

}  // namespace

int DisplayRefreshRateHz() {
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

bool PresentSlots::Take() {
  const int refreshRate = DisplayRefreshRateHz();
  if (refreshRate <= 0) {
    return true;
  }
  const int64_t periodUs = 1000000 / refreshRate;
  const int64_t nowUs = NowUs();
  if (nowUs + periodUs / kSlotToleranceDivisor < nextUs_) {
    return false;
  }
  nextUs_ = (nextUs_ > nowUs ? nextUs_ : nowUs) + periodUs;
  return true;
}

int64_t PresentSlots::WaitUs() const {
  const int refreshRate = DisplayRefreshRateHz();
  if (refreshRate <= 0) {
    return 0;
  }
  const int64_t periodUs = 1000000 / refreshRate;
  const int64_t openAtUs = nextUs_ - periodUs / kSlotToleranceDivisor;
  const int64_t nowUs = NowUs();
  return openAtUs > nowUs ? openAtUs - nowUs : 0;
}

}
