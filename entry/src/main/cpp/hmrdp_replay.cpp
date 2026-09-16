/*
 * HmRdp - dev-only recorded-RDP replay (doc_agent/gfx-engine.md §6).
 *
 * Present-on-screen consumer of the shared replay driver: the capture is read,
 * decompressed and parsed by hmrdp_gfx_driver.cpp (FreeRDP's own ZGX + RDPGFX
 * parsing), the resulting commands go to the Vulkan desktop engine, and this
 * file only presents the composed screen on every EndFrame through
 * GpuVkPresentComposed(). The compare route can additionally run an offline gdi
 * desktop on the same bytes and compare the two screens pixel by pixel.
 */
#include "hmrdp_replay.h"

#include <native_window/external_window.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_rfx.h"  // kGpuCmd / kGpuCodec ids
#include "hmrdp_vk_desktop.h"
#include "hmrdp_vk_renderer.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

// The replay's desktop engine: the Vulkan GFX engine plus its swapchain
// presenter. It exposes exactly what the pump/stats/compare code needs, so that
// code stays free of the engine's own types.
class ReplayDesktop {
 public:
  // Brings up the engine + presenter on the given XComponent surface; returns
  // false and fills `error` on failure.
  bool Init(void* window, int width, int height, std::string* error);
  void Resize(int width, int height);
  void Apply(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
             const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
             uint32_t payloadLen);
  // Composes and presents the engine screen. Returns false when nothing was
  // dirty (static frame) or the present failed.
  bool Present();
  bool screenDirty() const { return engine_ != nullptr && engine_->screenDirty(); }
  // The composed screen mirror, for the compare route's pixel A/B.
  bool ReadScreen(std::vector<uint8_t>* out) { return engine_->ReadScreen(out); }
  int screenWidth() const { return engine_->screenWidth(); }
  int screenHeight() const { return engine_->screenHeight(); }
  // One-line engine summary for the dev panel.
  std::string Summary() const { return engine_->Stats(); }

 private:
  std::unique_ptr<GfxVkDesktop> engine_;
  std::unique_ptr<VkRenderer> renderer_;
};


bool ReplayDesktop::Init(void* window, int width, int height, std::string* error) {
  renderer_ = std::make_unique<VkRenderer>();
  renderer_->SetSurface(window, width, height);
  // Prepare() creates the swapchain and presents a black frame; it also pins the
  // image format the engine must be created with (a blit cannot convert channel
  // order, so engine and swapchain have to agree).
  if (!renderer_->Prepare()) {
    if (error != nullptr) {
      *error = "vulkan surface/swapchain failed: " + renderer_->lastError();
    }
    renderer_.reset();
    return false;
  }
  engine_ = std::make_unique<GfxVkDesktop>();
  if (!engine_->Init(renderer_->format())) {
    if (error != nullptr) {
      *error = "vulkan engine init failed";
    }
    engine_.reset();
    renderer_.reset();
    return false;
  }
  return true;
}


void ReplayDesktop::Resize(int width, int height) {
  renderer_->ResizeSurface(width, height);
}

void ReplayDesktop::Apply(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                          const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                          uint32_t payloadLen) {
  engine_->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
}

bool ReplayDesktop::Present() {
  return GpuVkPresentComposed(engine_.get(), renderer_.get());
}

namespace {

constexpr int kFrameMs = 16;                      // ~60 Hz playback target
constexpr int kLogEvery = 120;
constexpr int64_t kMaxRunUs = 120ll * 1000000ll;  // safety cap, fast mode
// In realtime mode the run lasts as long as the recording did (plus whatever the
// client fell behind), so the cap has to be much higher to avoid cutting a long
// capture short.
constexpr int64_t kMaxRealtimeRunUs = 900ll * 1000000ll;
constexpr int kStartWaitUs = 3000000;

// Compare route: sample a full-screen readback every N frames (same idea as the
// live shadow check - reading the engine screen back is expensive).
constexpr uint64_t kCompareEvery = 30;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Stable route name for the stats panel / hilog.
const char* RouteName(GfxReplayRoute route) {
  switch (route) {
    case GfxReplayRoute::kCpu:
      return "cpu";
    case GfxReplayRoute::kVulkan:
      return "vulkan";
    case GfxReplayRoute::kVulkanCompare:
      return "vulkan-compare";
  }
  return "?";
}

// Sends the replayed commands into the engine; the driver supplies them.
class ReplaySink : public GfxCommandSink {
 public:
  ReplaySink(ReplayDesktop* engine, GfxReplay* owner) : engine_(engine), owner_(owner) {}

  void ApplyGfx(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                uint32_t payloadLen) override {
    if (engine_ == nullptr) {
      return;
    }
    // The surface command carries its codec id in scalars[0]; 0 marks the
    // non-surface commands (fill/copy/cache/...).
    const uint32_t codecId =
        (cmdId == kGpuCmdWireToSurface && scalars != nullptr) ? scalars[0] : 0u;
    const int64_t t0 = NowUs();
    engine_->Apply(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
    const int64_t total = NowUs() - t0;
    if (owner_ != nullptr) {
      owner_->RecordApply(cmdId, codecId, static_cast<uint64_t>(total));
    }
  }

 private:
  ReplayDesktop* engine_ = nullptr;
  GfxReplay* owner_ = nullptr;
};

}  // namespace

GfxReplay& GfxReplay::Instance() {
  static GfxReplay instance;
  return instance;
}

GfxReplay::~GfxReplay() {
  Stop();
}

bool GfxReplay::Start(void* nativeWindow, int surfaceW, int surfaceH,
                      const std::string& gfxPath, GfxReplayRoute route, bool realtime) {
  Stop();
  if (nativeWindow == nullptr || surfaceW <= 0 || surfaceH <= 0 || gfxPath.empty()) {
    if (nativeWindow != nullptr) {
      OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(nativeWindow));
    }
    return false;
  }
  // The realtime mode needs the arrival times recorded in the capture; an older
  // (version 0) file has none, so fall back to the fixed pacing and say so - a
  // silent fallback would make two runs with the same settings incomparable.
  const bool effectiveRealtime = realtime && GfxCaptureHasTimestamps(gfxPath);
  if (realtime && !effectiveRealtime) {
    HMRDP_LOGW("gfx replay: capture has no arrival times, realtime mode unavailable");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    {
      std::lock_guard<std::mutex> err(errorMutex_);
      lastError_.clear();
      traffic_.clear();
    }
    window_ = nativeWindow;
    surfaceW_ = surfaceW;
    surfaceH_ = surfaceH;
    gfxPath_ = gfxPath;
    route_.store(static_cast<int>(route));
    cpuDesktop_ = nullptr;
    desktop_.reset();
    frames_.store(0);
    presents_.store(0);
    presentSkips_.store(0);
    presentFailures_.store(0);
    applyUs_.store(0);
    applyCount_.store(0);
    progUs_.store(0);
    progCount_.store(0);
    clearUs_.store(0);
    clearCount_.store(0);
    uncompUs_.store(0);
    uncompCount_.store(0);
    fillUs_.store(0);
    fillCount_.store(0);
    blitUs_.store(0);
    blitCount_.store(0);
    cacheUs_.store(0);
    cacheCount_.store(0);
    otherUs_.store(0);
    otherCount_.store(0);
    cmpChecks_.store(0);
    cmpBad_.store(0);
    cmpMaxDiff_.store(0);
    cmpRgbDiff_.store(0);
    cmpAlphaDiff_.store(0);
    cmpBBoxX0_.store(-1);
    cmpBBoxY0_.store(-1);
    cmpBBoxX1_.store(-1);
    cmpBBoxY1_.store(-1);
    cmpMaxDelta_.store(0);
    cmpSmallDeltaPx_.store(0);
    presentUs_.store(0);
    uploadBytes_.store(0);
    uploadBoxBytes_.store(0);
    uploadRectPresents_.store(0);
    uploadTruncated_.store(0);
    uploadMaxRects_.store(0);
    pumpUs_.store(0);
    pumpStartUs_.store(0);
    paceUs_.store(0);
    realtimeRequested_.store(realtime ? 1 : 0);
    realtimeActive_.store(effectiveRealtime ? 1 : 0);
    realtimeLagUs_.store(0);
    recordBaseUs_ = 0;
    recordWallBaseUs_ = 0;
    lastFrameEndUs_ = 0;
    firstPacedUs_.store(0);
    startUs_.store(NowUs());
    endUs_.store(0);
    // The pure CPU route presents raw gdi frames through the Vulkan presenter;
    // the engine route builds its own presenter inside the worker.
    if (route == GfxReplayRoute::kCpu) {
      presenter_ = CreateFramePresenter();
      presenter_->SetSurface(window_, surfaceW_, surfaceH_);
    } else {
      presenter_.reset();
    }
    running_.store(true);
    thread_ = std::thread(&GfxReplay::Run, this);
  }
  const int64_t deadline = NowUs() + kStartWaitUs;
  while (NowUs() < deadline) {
    if (!running_.load() || presents_.load() > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!running_.load()) {
    std::lock_guard<std::mutex> err(errorMutex_);
    HMRDP_LOGE("gfx replay: start failed (%{public}s)",
               lastError_.empty() ? "unknown" : lastError_.c_str());
    Stop();
    return false;
  }
  HMRDP_LOGI(
      "gfx replay: started route=%{public}s mode=%{public}s surface=%{public}dx%{public}d "
      "path=%{public}s",
      RouteName(static_cast<GfxReplayRoute>(route_.load())),
      effectiveRealtime ? "realtime" : "fast", surfaceW, surfaceH, gfxPath.c_str());
  return true;
}

void GfxReplay::Resize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  pendingW_.store(width);
  pendingH_.store(height);
}

void GfxReplay::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.exchange(false)) {
      if (!thread_.joinable()) {
        if (window_ != nullptr) {
          OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(window_));
          window_ = nullptr;
        }
        return;
      }
    }
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  presenter_.reset();
  if (window_ != nullptr) {
    OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(window_));
    window_ = nullptr;
  }
  HMRDP_LOGI("gfx replay: stopped frames=%{public}llu presents=%{public}llu",
             static_cast<unsigned long long>(frames_.load()),
             static_cast<unsigned long long>(presents_.load()));
}

std::string GfxReplay::StatsLines() {
  // Freeze the clock once the replay has ended: the UI keeps polling every
  // second, and a growing denominator would make fps/feed decay on a paused
  // (finished) replay.
  const int64_t stopUs = endUs_.load();
  const int64_t nowUs = stopUs != 0 ? stopUs : NowUs();
  // fps is the **playback** rate: measured from the first paced frame (see
  // PaceFrame) to the end, so the run's start-up (engine/presenter init, waiting for
  // the first frame) does not dilute it. PaceFrame counts one frame per paced
  // presentation, hence `presents - 1` periods.
  const uint64_t presents = presents_.load();
  const int64_t firstPacedUs = firstPacedUs_.load();
  const int64_t elapsedUs =
      firstPacedUs != 0 ? nowUs - firstPacedUs : (startUs_.load() != 0 ? nowUs - startUs_.load() : 0);
  const double fps =
      (elapsedUs > 0 && presents > 1)
          ? static_cast<double>(presents - 1) * 1000000.0 / static_cast<double>(elapsedUs)
          : 0.0;
  auto avgMs = [](uint64_t total, uint64_t count) {
    return count > 0 ? static_cast<double>(total) / static_cast<double>(count) / 1000.0 : 0.0;
  };
  std::string err;
  std::string traffic;
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    err = lastError_;
    traffic = traffic_;
  }
  const uint64_t pace = paceUs_.load();
  // "feed" = compute time, excluding the deliberate playback throttling. Once
  // the pump has returned its exact figure is used; while still running the wall
  // clock minus the paced sleep is a good live approximation.
  const uint64_t elapsed = elapsedUs > 0 ? static_cast<uint64_t>(elapsedUs) : 0;
  const uint64_t measuredPump = pumpUs_.load();
  uint64_t pumpUs = measuredPump > 0
                        ? (measuredPump > pace ? measuredPump - pace : 0)
                        : (elapsed > pace ? elapsed - pace : 0);
  // Exclude the run's start-up (engine/presenter init, first frame wait) so `feed`
  // is the playback's own compute, comparable between runs and routes.
  const int64_t firstPaced = firstPacedUs_.load();
  const int64_t pumpStart = pumpStartUs_.load();
  if (firstPaced != 0 && pumpStart != 0 && firstPaced > pumpStart &&
      static_cast<uint64_t>(firstPaced - pumpStart) < pumpUs) {
    pumpUs -= static_cast<uint64_t>(firstPaced - pumpStart);
  }

  const char* routeName = RouteName(static_cast<GfxReplayRoute>(route_.load()));
  // mode=realtime replays the capture's own arrival times; mode=fast gives every
  // presented frame a kFrameMs budget (a throughput figure, not the live cadence).
  // `lag` is only meaningful in realtime mode: the worst lateness behind the
  // recorded schedule, i.e. how much of the live load the client could not absorb.
  // "fast(untimed)" = realtime was requested but the capture carries no arrival
  // times, so the fixed pacing ran instead (the difference is visible here on
  // purpose: two runs that differ in mode are not comparable).
  const char* modeName = realtimeActive_.load() != 0
                             ? "realtime"
                             : (realtimeRequested_.load() != 0 ? "fast(untimed)" : "fast");
  const unsigned long long frames = static_cast<unsigned long long>(frames_.load());
  char head[400];
  std::snprintf(head, sizeof(head),
                "route=%s  mode=%s  frames=%llu  presents=%llu  fps=%.1f  fail=%llu  skip=%llu\n"
                "feed=%llums   parse=%llums   present=%.2fms   lag=%llums   (running=%d)",
                routeName, modeName, frames,
                static_cast<unsigned long long>(presents), fps,
                static_cast<unsigned long long>(presentFailures_.load()),
                static_cast<unsigned long long>(presentSkips_.load()),
                static_cast<unsigned long long>(pumpUs / 1000),
                static_cast<unsigned long long>(hmrdp::GfxReplayParseUs() / 1000),
                avgMs(presentUs_.load(), presents),
                static_cast<unsigned long long>(realtimeLagUs_.load() / 1000),
                running_.load() ? 1 : 0);
  std::string out(head);

  // CPU route only (the GPU route composes in the engine and never presents a gdi
  // frame): how many bytes actually left the CPU, versus what the merged bounding
  // box would have cost for the same run (the saving is then visible in every run
  // rather than only during an A/B). `rectlist=used/total presents` and
  // `truncated` say whether the rect path was exercised or fell back.
  const uint64_t uploadBoxBytes = uploadBoxBytes_.load();
  if (uploadBoxBytes > 0) {
    const double mb = 1.0 / (1024.0 * 1024.0);
    char up[288];
    std::snprintf(up, sizeof(up),
                  "\nupload rects  rectlist=%llu/%llu  uploaded=%.1fMB (box=%.1fMB)  "
                  "maxRects=%llu  truncated=%llu",
                  static_cast<unsigned long long>(uploadRectPresents_.load()),
                  static_cast<unsigned long long>(presents),
                  static_cast<double>(uploadBytes_.load()) * mb,
                  static_cast<double>(uploadBoxBytes) * mb,
                  static_cast<unsigned long long>(uploadMaxRects_.load()),
                  static_cast<unsigned long long>(uploadTruncated_.load()));
    out += up;
  }

  // Per-command-class breakdown only exists on the GPU route (the CPU route
  // decodes inside FreeRDP and never calls the sink).
  if (applyCount_.load() > 0) {
    char cls[640];
    std::snprintf(
        cls, sizeof(cls),
        "\nprog  %.2fms x%llu\nclear %.2fms x%llu\nunc   %.2fms x%llu\n"
        "fill  %.2fms x%llu\nblit  %.2fms x%llu\ncache %.2fms x%llu\nother %.2fms x%llu",
        avgMs(progUs_.load(), progCount_.load()),
        static_cast<unsigned long long>(progCount_.load()),
        avgMs(clearUs_.load(), clearCount_.load()),
        static_cast<unsigned long long>(clearCount_.load()),
        avgMs(uncompUs_.load(), uncompCount_.load()),
        static_cast<unsigned long long>(uncompCount_.load()),
        avgMs(fillUs_.load(), fillCount_.load()),
        static_cast<unsigned long long>(fillCount_.load()),
        avgMs(blitUs_.load(), blitCount_.load()),
        static_cast<unsigned long long>(blitCount_.load()),
        avgMs(cacheUs_.load(), cacheCount_.load()),
        static_cast<unsigned long long>(cacheCount_.load()),
        avgMs(otherUs_.load(), otherCount_.load()),
        static_cast<unsigned long long>(otherCount_.load()));
    out += cls;
  }
  const uint64_t cmpChecks = cmpChecks_.load();
  if (cmpChecks > 0) {
    char cmp[192];
    std::snprintf(cmp, sizeof(cmp),
                  "\ncompare(GPU vs gdi): checks=%llu bad=%llu rgbPx=%llu alphaPx=%llu "
                  "maxRgbPx=%llu bbox=(%d,%d)-(%d,%d) maxDelta=%d smallDeltaPx=%llu",
                  static_cast<unsigned long long>(cmpChecks),
                  static_cast<unsigned long long>(cmpBad_.load()),
                  static_cast<unsigned long long>(cmpRgbDiff_.load()),
                  static_cast<unsigned long long>(cmpAlphaDiff_.load()),
                  static_cast<unsigned long long>(cmpMaxDiff_.load()), cmpBBoxX0_.load(),
                  cmpBBoxY0_.load(), cmpBBoxX1_.load(), cmpBBoxY1_.load(), cmpMaxDelta_.load(),
                  static_cast<unsigned long long>(cmpSmallDeltaPx_.load()));
    out += cmp;
  }
  if (!traffic.empty()) {
    out += "\n";
    out += traffic;
  }
  if (!err.empty()) {
    out += "\nerr=";
    out += err;
  }
  return out;
}

std::string GfxReplay::Stats() {
  // Single-line form for logging: the multi-line panel text with the newlines
  // flattened, so one hilog record carries the whole summary.
  std::string s = StatsLines();
  for (char& c : s) {
    if (c == '\n') {
      c = ' ';
    }
  }
  return s;
}

void GfxReplay::RecordApply(uint16_t cmdId, uint32_t codecId, uint64_t micros) {
  applyUs_.fetch_add(micros);
  applyCount_.fetch_add(1);
  std::atomic<uint64_t>* us = &otherUs_;
  std::atomic<uint64_t>* count = &otherCount_;
  if (cmdId == kGpuCmdWireToSurface) {
    if (codecId == kGpuCodecCaprogressive || codecId == kGpuCodecCaprogressiveV2) {
      us = &progUs_;
      count = &progCount_;
    } else if (codecId == kGpuCodecClearCodec) {
      us = &clearUs_;
      count = &clearCount_;
    } else {
      us = &uncompUs_;
      count = &uncompCount_;
    }
  } else if (cmdId == kGpuCmdSolidFill) {
    us = &fillUs_;
    count = &fillCount_;
  } else if (cmdId == kGpuCmdSurfaceToSurface) {
    us = &blitUs_;
    count = &blitCount_;
  } else if (cmdId == kGpuCmdSurfaceToCache || cmdId == kGpuCmdCacheToSurface ||
             cmdId == kGpuCmdEvictCacheEntry) {
    us = &cacheUs_;
    count = &cacheCount_;
  }
  us->fetch_add(micros);
  count->fetch_add(1);
}

void GfxReplay::RecordPresent(uint64_t micros) {
  presentUs_.fetch_add(micros);
}

void GfxReplay::PaceFrame(bool presented) {
  // Give every presented frame a kFrameMs period: once the frame's own work
  // (decode + command application + present) is done, sleep for the rest of the
  // budget.
  //
  // Nothing is carried over between frames, on purpose. A frame that overruns keeps
  // its longer period, and a frame that finishes early sleeps the remainder, so the
  // reported fps / present / feed figures describe the playback as it actually
  // happened. A "catch up on later frames" schedule would make the average reach the
  // target while individual frames were still measured under a different (faster)
  // cadence - exactly what a reference measurement must not do - and it does not
  // make playback any smoother either.
  //
  // Only frames that produced a picture are paced: the stream carries many frame
  // markers with no drawable update (management/ack frames, off-screen surfaces),
  // and those must not spend a frame budget (work between two presented frames is
  // still inside their period).
  if (!presented) {
    return;
  }
  const int64_t now = static_cast<int64_t>(NowUs());
  // Realtime mode takes its cadence from the capture (PaceRecord), so no synthetic
  // kFrameMs budget is added on top of it.
  if (lastFrameEndUs_ != 0 && realtimeActive_.load() == 0) {
    const int64_t target = lastFrameEndUs_ + static_cast<int64_t>(kFrameMs) * 1000;
    if (target > now) {
      std::this_thread::sleep_until(
          std::chrono::steady_clock::time_point(std::chrono::microseconds(target)));
      paceUs_.fetch_add(static_cast<uint64_t>(static_cast<int64_t>(NowUs()) - now));
    }
  }
  lastFrameEndUs_ = static_cast<int64_t>(NowUs());
  if (firstPacedUs_.load() == 0) {
    // fps is measured from here, so the run's start-up is not counted as playback.
    firstPacedUs_.store(lastFrameEndUs_);
  }
}

void GfxReplay::PaceRecord(uint64_t timestampUs) {
  const int64_t now = static_cast<int64_t>(NowUs());
  if (recordBaseUs_ == 0) {
    // First record defines the schedule origin: everything else is measured as an
    // offset from it, so the run's start-up is not replayed as a stall.
    recordBaseUs_ = timestampUs;
    recordWallBaseUs_ = now;
    return;
  }
  const int64_t target =
      recordWallBaseUs_ + static_cast<int64_t>(timestampUs - recordBaseUs_);
  if (target > now) {
    // Sleep in slices so a long gap in the recording (an idle stretch, a paused
    // session) cannot make Stop() - and therefore a route switch on the dev page -
    // wait for the whole gap before the pump notices it must stop.
    constexpr int64_t kSliceUs = 20000;
    const int64_t start = static_cast<int64_t>(NowUs());
    while (running_.load() && static_cast<int64_t>(NowUs()) < target) {
      const int64_t remaining = target - static_cast<int64_t>(NowUs());
      std::this_thread::sleep_for(
          std::chrono::microseconds(remaining > kSliceUs ? kSliceUs : remaining));
    }
    paceUs_.fetch_add(static_cast<uint64_t>(static_cast<int64_t>(NowUs()) - start));
    return;
  }
  // Already behind the recorded schedule: never catch up (that would compress the
  // following gaps and report a cadence the client never actually ran at). Only
  // remember the excursion, which is the honest "cannot keep up with the live
  // load" number.
  const uint64_t behind = static_cast<uint64_t>(now - target);
  if (behind > realtimeLagUs_.load()) {
    realtimeLagUs_.store(behind);
  }
}

void GfxReplay::OnReplayFrame() {
  const int64_t nowUs = static_cast<int64_t>(NowUs());
  const int64_t capUs = realtimeActive_.load() != 0 ? kMaxRealtimeRunUs : kMaxRunUs;
  if (nowUs - startUs_.load() > capUs) {
    HMRDP_LOGI("gfx replay: %{public}ds cap reached",
               static_cast<int>(capUs / 1000000));
    running_.store(false);
    return;
  }
  const int pw = pendingW_.exchange(0);
  const int ph = pendingH_.exchange(0);
  if (pw > 0 && ph > 0 && desktop_ != nullptr) {
    desktop_->Resize(pw, ph);
  }
  const int64_t presentStart = NowUs();
  const bool presented = desktop_ != nullptr && desktop_->Present();
  RecordPresent(static_cast<uint64_t>(NowUs() - presentStart));
  frames_.fetch_add(1);
  if (presented) {
    presents_.fetch_add(1);
  } else if (desktop_ != nullptr && !desktop_->screenDirty()) {
    // Nothing to show: no surface had a dirty region mapped to the output, so
    // Compose() bailed out before touching the screen. This is the normal
    // "static frame" case (typically the first frame markers before
    // ResetGraphics / before any drawable update), not a failure.
    const uint64_t n = presentSkips_.fetch_add(1) + 1;
    if (n <= 4) {
      HMRDP_LOGI("gfx replay: no present #%{public}llu at frame=%{public}llu (nothing dirty)",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(frames_.load()));
    }
  } else {
    presentFailures_.fetch_add(1);
    const uint64_t n = presentFailures_.load();
    if (n <= 4) {
      HMRDP_LOGW("gfx replay: present failed #%{public}llu at frame=%{public}llu",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(frames_.load()));
    }
  }
  if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
    if (desktop_ != nullptr) {
      const std::string t = desktop_->Summary();
      std::lock_guard<std::mutex> lock(errorMutex_);
      traffic_ = t;
    }
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  PaceFrame(presented);
}

void GfxReplay::Run() {
  switch (static_cast<GfxReplayRoute>(route_.load())) {
    case GfxReplayRoute::kCpu:
      RunCpuReplay(gfxPath_);
      break;
    case GfxReplayRoute::kVulkan:
      RunVulkanReplay(gfxPath_, false);
      break;
    case GfxReplayRoute::kVulkanCompare:
      RunVulkanReplay(gfxPath_, true);
      break;
  }
  endUs_.store(NowUs());
  running_.store(false);
}

void GfxReplay::RunVulkanReplay(const std::string& gfxPath, bool compare) {
  std::unique_ptr<ReplayDesktop> desktop(new ReplayDesktop());
  std::string error;
  if (!desktop->Init(window_, surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error.empty() ? "engine init failed" : error;
    return;
  }
  desktop_ = std::move(desktop);
  // The correctness routes feed the exact same bytes into FreeRDP's own gdi
  // pipeline and compare the two composed screens. The CPU desktop decodes but
  // never presents, so there is no double present and no timing meaning here.
  GfxCpuDesktop cpu;
  if (compare) {
    if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
      std::lock_guard<std::mutex> err(errorMutex_);
      lastError_ = error;
      desktop_.reset();
      return;
    }
    cpuDesktop_ = &cpu;
  }
  ReplaySink sink(desktop_.get(), this);

  // The replayed stream must not re-trigger the capture hook on the recorder.
  hmrdp::GfxDumpSetReplaying(true);
  hmrdp::GfxReplayResetParseUs();

  const int64_t pumpStart = NowUs();
  pumpStartUs_.store(pumpStart);

  // Realtime mode: the pump waits for each record's recorded arrival time before
  // feeding it, so the playback runs on the cadence the live session saw.
  ReplayPaceFn pace;
  if (realtimeActive_.load() != 0) {
    pace = [this](uint64_t tsUs) { PaceRecord(tsUs); };
  }
  // The present (and its pacing sleep) runs inside the recv call, so the parse
  // figures must not count the throttling as decode work.
  const ReplayPaceAccumFn paceAccum = [this]() { return paceUs_.load(); };

  bool ok = false;
  if (compare) {
    // gdi and the engine consume the capture interleaved per PDU, and CompareFrames()
    // runs on gdi's EndFrame - with both sides at the same stream position, which is
    // what makes the pixel A/B meaningful. OnReplayFrame keeps presenting and pacing.
    ok = GfxReplayStreamCompare(gfxPath, &sink, [this]() { OnReplayFrame(); },
                                [this]() { CompareFrames(); }, {}, cpu.gfx(), &running_, &error,
                                pace);
  } else {
    ok = GfxReplayStream(gfxPath, &sink, [this]() { OnReplayFrame(); }, &running_, &error, pace,
                         paceAccum);
  }
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  {
    const std::string summary = desktop_->Summary();
    std::lock_guard<std::mutex> lock(errorMutex_);
    traffic_ = summary;
  }
  hmrdp::GfxDumpSetReplaying(false);
  cpuDesktop_ = nullptr;
  const int64_t releaseStart = NowUs();
  desktop_.reset();
  HMRDP_LOGI("gfx replay: engine teardown %{public}llu ms",
             static_cast<unsigned long long>((NowUs() - releaseStart) / 1000));
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished (route=%{public}s): %{public}s",
             RouteName(static_cast<GfxReplayRoute>(route_.load())), Stats().c_str());
}

void GfxReplay::RunCpuReplay(const std::string& gfxPath) {
  // FreeRDP's own gdi pipeline handles the decoding (clear/progressive/...),
  // exactly like a live session. Only the destination changes: gdi's primary
  // buffer is uploaded through the presenter's CPU frame path instead of the
  // engine's screen texture, so this route is the CPU reference for the engine.
  presenter_->Prepare();
  GfxCpuDesktop cpu;
  std::string error;
  if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
    return;
  }
  cpu.SetFrameFn([this, &cpu]() { OnCpuFrame(&cpu); });

  hmrdp::GfxDumpSetReplaying(true);
  hmrdp::GfxReplayResetParseUs();

  const int64_t pumpStart = NowUs();
  pumpStartUs_.store(pumpStart);

  ReplayPaceFn pace;
  if (realtimeActive_.load() != 0) {
    pace = [this](uint64_t tsUs) { PaceRecord(tsUs); };
  }
  const ReplayPaceAccumFn paceAccum = [this]() { return paceUs_.load(); };
  const bool ok = GfxReplayPump(gfxPath, cpu.gfx(), &running_, &error, pace, paceAccum);
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  hmrdp::GfxDumpSetReplaying(false);
  cpu.SetFrameFn(nullptr);
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished (cpu): %{public}s", Stats().c_str());
}

void GfxReplay::CompareFrames() {
  ReplayDesktop* engine = desktop_.get();
  GfxCpuDesktop* cpu = cpuDesktop_;
  if (engine == nullptr || cpu == nullptr) {
    return;
  }
  // Sample like the live shadow check: a full screen readback is expensive.
  if ((frames_.load() % static_cast<uint64_t>(kCompareEvery)) != 0) {
    return;
  }
  rdpGdi* gdi = cpu->gdi();
  const int w = engine->screenWidth();
  const int h = engine->screenHeight();
  if (gdi == nullptr || gdi->primary_buffer == nullptr || w <= 0 || h <= 0) {
    return;
  }
  const int cmpW = w < static_cast<int>(gdi->width) ? w : static_cast<int>(gdi->width);
  const int cmpH = h < static_cast<int>(gdi->height) ? h : static_cast<int>(gdi->height);
  if (cmpW <= 0 || cmpH <= 0) {
    return;
  }
  std::vector<uint8_t> screen;
  if (!engine->ReadScreen(&screen)) {
    return;
  }
  if (screen.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
    return;
  }
  // Separate RGB differences (a real decoding/composition mismatch) from
  // alpha-only differences (BGRX surfaces: the two decoders may disagree only on
  // the unused byte, which is not a visual difference).
  size_t diffRgb = 0;
  size_t diffAlpha = 0;
  int firstX = -1;
  int firstY = -1;
  // Bounding box of the RGB differences and the largest per-channel delta:
  // "one rectangle" means a command/region the engine failed to paint (or
  // composed differently), while "+/-1 everywhere" means rounding.
  int bx0 = cmpW;
  int by0 = cmpH;
  int bx1 = -1;
  int by1 = -1;
  int maxDelta = 0;
  size_t smallDeltaPx = 0;  // all channels within +/-2: rounding-level
  for (int row = 0; row < cmpH; ++row) {
    const uint8_t* a = screen.data() + static_cast<size_t>(row) * w * 4;
    const uint8_t* b = gdi->primary_buffer + static_cast<size_t>(row) * gdi->stride;
    for (int col = 0; col < cmpW; ++col) {
      const uint8_t* pa = a + col * 4;
      const uint8_t* pb = b + col * 4;
      if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) {
        if (firstX < 0) {
          firstX = col;
          firstY = row;
        }
        if (col < bx0) bx0 = col;
        if (row < by0) by0 = row;
        if (col > bx1) bx1 = col;
        if (row > by1) by1 = row;
        int worst = 0;
        for (int k = 0; k < 3; ++k) {
          const int d = static_cast<int>(pa[k]) - static_cast<int>(pb[k]);
          const int ad = d < 0 ? -d : d;
          if (ad > worst) worst = ad;
        }
        if (worst > maxDelta) maxDelta = worst;
        if (worst <= 2) smallDeltaPx++;
        diffRgb++;
      } else if (pa[3] != pb[3]) {
        diffAlpha++;
      }
    }
  }
  cmpSmallDeltaPx_.fetch_add(smallDeltaPx);
  if (bx1 >= 0) {
    cmpBBoxX0_.store(bx0);
    cmpBBoxY0_.store(by0);
    cmpBBoxX1_.store(bx1);
    cmpBBoxY1_.store(by1);
    cmpMaxDelta_.store(maxDelta);
  }
  cmpChecks_.fetch_add(1);
  cmpRgbDiff_.fetch_add(diffRgb);
  cmpAlphaDiff_.fetch_add(diffAlpha);
  if (diffRgb != 0 || diffAlpha != 0) {
    cmpBad_.fetch_add(1);
    const uint64_t prev = cmpMaxDiff_.load();
    if (static_cast<uint64_t>(diffRgb) > prev) {
      cmpMaxDiff_.store(static_cast<uint64_t>(diffRgb));
    }
    // One line per mismatching sample: enough to tell whether a later run's
    // divergence has the same shape (bbox / delta) or is a new one.
    HMRDP_LOGW(
        "gfx replay: compare diff frame=%{public}llu rgb=%{public}llu alphaOnly=%{public}llu "
        "first=(%{public}d,%{public}d) bbox=(%{public}d,%{public}d)-(%{public}d,%{public}d) "
        "maxDelta=%{public}d small=%{public}llu",
        static_cast<unsigned long long>(frames_.load()),
        static_cast<unsigned long long>(diffRgb), static_cast<unsigned long long>(diffAlpha),
        firstX, firstY, bx0, by0, bx1, by1, maxDelta,
        static_cast<unsigned long long>(smallDeltaPx));
    // Localizing a mismatch (doc_agent/gfx-engine.md §7): this pixel comparison is
    // the *only* valid signal - comparing dirt-rect *coverage* is not, because the
    // reference's rects are a coarse superset of what changed, so "gdi covered it,
    // the engine did not" also happens on frames that match pixel for pixel. To
    // continue from here, dump the differing rect's pixels from all three sources
    // (engine screen via ReadScreen, gdi's primary_buffer, gdi's surface via
    // cpu.gfx()->GetSurfaceData) - that separates "the engine's compose missed the
    // rect" from "a decode difference / a stale reference buffer".
  }
}

void GfxReplay::OnCpuFrame(GfxCpuDesktop* cpu) {
  if (cpu == nullptr) {
    return;
  }
  const int64_t nowUs = static_cast<int64_t>(NowUs());
  const int64_t capUs = realtimeActive_.load() != 0 ? kMaxRealtimeRunUs : kMaxRunUs;
  if (nowUs - startUs_.load() > capUs) {
    HMRDP_LOGI("gfx replay: %{public}ds cap reached",
               static_cast<int>(capUs / 1000000));
    running_.store(false);
    return;
  }
  const int pw = pendingW_.exchange(0);
  const int ph = pendingH_.exchange(0);
  if (pw > 0 && ph > 0) {
    presenter_->ResizeSurface(pw, ph);
  }
  const int64_t presentStart = NowUs();
  PresentUploadInfo upload;
  const bool presented = PresentGdiFrame(cpu->gdi(), presenter_.get(), &upload);
  RecordPresent(static_cast<uint64_t>(NowUs() - presentStart));
  uploadBytes_.fetch_add(static_cast<uint64_t>(upload.uploadedBytes));
  uploadBoxBytes_.fetch_add(static_cast<uint64_t>(upload.boxBytes));
  if (upload.usedRects) {
    uploadRectPresents_.fetch_add(1);
  }
  if (upload.rectListTruncated) {
    uploadTruncated_.fetch_add(1);
  }
  const uint64_t frameRects = static_cast<uint64_t>(upload.totalRects);
  if (frameRects > uploadMaxRects_.load()) {
    uploadMaxRects_.store(frameRects);
  }
  frames_.fetch_add(1);
  if (presented) {
    presents_.fetch_add(1);
  } else {
    presentFailures_.fetch_add(1);
  }
  if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  PaceFrame(presented);
}

}  // namespace hmrdp
