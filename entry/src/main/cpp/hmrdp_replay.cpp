/*
 * HmRdp - dev-only recorded-RDP replay (PERF-TODO §4).
 *
 * Present-on-screen consumer of the shared replay driver: the capture is read,
 * decompressed and parsed by hmrdp_gfx_driver.cpp (FreeRDP's own ZGX + RDPGFX
 * parsing), the resulting commands go to the GPU desktop engine, and this file
 * only presents the composed screen on every EndFrame - through the very same
 * GpuPresentComposed() the live session uses.
 */
#include "hmrdp_replay.h"

#include <native_window/external_window.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_renderer.h"
#include "hmrdp_rfx.h"  // GfxGpuDesktop + GpuPresentComposed

namespace hmrdp {

namespace {

constexpr int kFrameMs = 16;                      // ~60 Hz playback target
constexpr int kLogEvery = 120;
constexpr int64_t kMaxRunUs = 120ll * 1000000ll;  // safety cap
constexpr int kStartWaitUs = 3000000;
// Compare route: sample a full-screen readback every N frames (same idea as the
// live shadow check - reading the engine screen back is expensive).
constexpr uint64_t kCompareEvery = 30;

// ClearCodec batch granularity for the GPU route (union-rectangle pixel cap).
// Configurable so the sync-count vs mapped-bytes trade-off can be measured on
// real hardware instead of being fixed to a simulator-derived value.
std::atomic<int> g_clearBatchArea{1 << 20};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Sends the replayed commands into the GPU engine; the driver supplies them.
class ReplaySink : public GfxCommandSink {
 public:
  ReplaySink(GfxGpuDesktop* engine, GfxReplay* owner) : engine_(engine), owner_(owner) {}

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
    // A command may trigger a ClearCodec flush before it runs; charge that time
    // to its own bucket so the per-class averages stay meaningful.
    const uint64_t flushBefore = engine_->ClearWorkUs();
    const int64_t t0 = NowUs();
    engine_->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
    const int64_t total = NowUs() - t0;
    const int64_t flushUs = static_cast<int64_t>(engine_->ClearWorkUs() - flushBefore);
    if (owner_ != nullptr) {
      owner_->RecordApply(cmdId, surfaceId, codecId,
                          static_cast<uint64_t>(total > flushUs ? total - flushUs : 0));
      if (flushUs > 0) {
        owner_->RecordFlush(static_cast<uint64_t>(flushUs));
      }
    }
  }

 private:
  GfxGpuDesktop* engine_ = nullptr;
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
                      const std::string& gfxPath, GfxReplayRoute route) {
  Stop();
  if (nativeWindow == nullptr || surfaceW <= 0 || surfaceH <= 0 || gfxPath.empty()) {
    if (nativeWindow != nullptr) {
      OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(nativeWindow));
    }
    return false;
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
    engine_ = nullptr;
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
    cmpFirstX_.store(-1);
    cmpFirstY_.store(-1);
    clearRunSum_.store(0);
    clearRunCount_.store(0);
    clearRunMax_.store(0);
    clearRunLen_ = 0;
    clearRunSurface_ = 0xFFFFFFFFu;
    clearRunActive_ = false;
    flushUs_.store(0);
    presentUs_.store(0);
    pumpUs_.store(0);
    paceUs_.store(0);
    startUs_.store(NowUs());
    endUs_.store(0);
    renderer_.reset(new Renderer());
    renderer_->SetSurface(window_, surfaceW_, surfaceH_);
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
  HMRDP_LOGI("gfx replay: started route=%{public}s surface=%{public}dx%{public}d path=%{public}s",
             route == GfxReplayRoute::kCpu ? "cpu" : "gpu", surfaceW, surfaceH, gfxPath.c_str());
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
  renderer_.reset();
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
  const int64_t elapsedUs = startUs_.load() != 0 ? nowUs - startUs_.load() : 0;
  const uint64_t presents = presents_.load();
  const double fps = elapsedUs > 0 ? static_cast<double>(presents) * 1000000.0 /
                                         static_cast<double>(elapsedUs)
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
  const uint64_t pumpUs = measuredPump > 0
                              ? (measuredPump > pace ? measuredPump - pace : 0)
                              : (elapsed > pace ? elapsed - pace : 0);

  const char* routeName = "gpu";
  switch (static_cast<GfxReplayRoute>(route_.load())) {
    case GfxReplayRoute::kCpu:
      routeName = "cpu";
      break;
    case GfxReplayRoute::kCompare:
      routeName = "compare";
      break;
    default:
      break;
  }
  const unsigned long long frames = static_cast<unsigned long long>(frames_.load());
  char head[320];
  std::snprintf(head, sizeof(head),
                "route=%s  frames=%llu  presents=%llu  fps=%.1f  fail=%llu  skip=%llu\n"
                "feed=%llums   present=%.2fms   (running=%d)",
                routeName, frames,
                static_cast<unsigned long long>(presents), fps,
                static_cast<unsigned long long>(presentFailures_.load()),
                static_cast<unsigned long long>(presentSkips_.load()),
                static_cast<unsigned long long>(pumpUs / 1000), avgMs(presentUs_.load(), presents),
                running_.load() ? 1 : 0);
  std::string out(head);

  // Per-command-class breakdown only exists on the GPU route (the CPU route
  // decodes inside FreeRDP and never calls the sink).
  if (applyCount_.load() > 0) {
    char cls[640];
    std::snprintf(
        cls, sizeof(cls),
        "\nprog  %.2fms x%llu\nclear %.2fms x%llu\nunc   %.2fms x%llu\n"
        "fill  %.2fms x%llu\nblit  %.2fms x%llu\ncache %.2fms x%llu\nother %.2fms x%llu\n"
        "flush %.0fms (ClearCodec round trip, excluded above)\n"
        "clearRun avg %.1f max %llu n %llu",
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
        static_cast<unsigned long long>(otherCount_.load()),
        static_cast<double>(flushUs_.load()) / 1000.0,
        clearRunCount_.load() > 0
            ? static_cast<double>(clearRunSum_.load()) / static_cast<double>(clearRunCount_.load())
            : 0.0,
        static_cast<unsigned long long>(clearRunMax_.load()),
        static_cast<unsigned long long>(clearRunCount_.load()));
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

void GfxReplay::RecordApply(uint16_t cmdId, uint32_t surfaceId, uint32_t codecId,
                            uint64_t micros) {
  applyUs_.fetch_add(micros);
  applyCount_.fetch_add(1);
  const bool isClear =
      (cmdId == kGpuCmdWireToSurface && codecId == kGpuCodecClearCodec);
  if (isClear) {
    if (clearRunActive_ && surfaceId == clearRunSurface_) {
      clearRunLen_++;
    } else {
      if (clearRunActive_) {
        clearRunSum_.fetch_add(clearRunLen_);
        clearRunCount_.fetch_add(1);
        if (clearRunLen_ > clearRunMax_.load()) {
          clearRunMax_.store(clearRunLen_);
        }
      }
      clearRunActive_ = true;
      clearRunSurface_ = surfaceId;
      clearRunLen_ = 1;
    }
  } else if (clearRunActive_) {
    clearRunSum_.fetch_add(clearRunLen_);
    clearRunCount_.fetch_add(1);
    if (clearRunLen_ > clearRunMax_.load()) {
      clearRunMax_.store(clearRunLen_);
    }
    clearRunActive_ = false;
  }

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

void GfxReplay::RecordFlush(uint64_t micros) {
  flushUs_.fetch_add(micros);
}

void GfxReplay::RecordPresent(uint64_t micros) {
  presentUs_.fetch_add(micros);
}

void GfxReplay::SetClearBatchArea(int pixels) {
  g_clearBatchArea.store(pixels < 0 ? 0 : pixels);
}

int GfxReplay::ClearBatchArea() const {
  return g_clearBatchArea.load();
}

void GfxReplay::PaceFrame(int64_t frameStartUs, bool presented) {
  // Only frames that actually produced a picture are paced: the GFX stream
  // carries many frame markers with no drawable update (management/ack frames,
  // off-screen surfaces), and sleeping on every EndFrame made the replay run far
  // slower than real time - dragging the reported fps down with it. An empty
  // frame is allowed to pass through at pump speed. The deadline is anchored to
  // the frame start so a slow frame never accumulates a sleep debt.
  if (!presented) {
    return;
  }
  const int64_t target = frameStartUs + static_cast<int64_t>(kFrameMs) * 1000;
  const int64_t before = NowUs();
  if (target > before) {
    std::this_thread::sleep_until(
        std::chrono::steady_clock::time_point(std::chrono::microseconds(target)));
    paceUs_.fetch_add(static_cast<uint64_t>(NowUs() - before));
  }
}

void GfxReplay::OnReplayFrame() {
  const int64_t frameStartUs = NowUs();
  if (frameStartUs - startUs_.load() > kMaxRunUs) {
    HMRDP_LOGI("gfx replay: 120s cap reached");
    running_.store(false);
    return;
  }
  const int pw = pendingW_.exchange(0);
  const int ph = pendingH_.exchange(0);
  if (pw > 0 && ph > 0) {
    renderer_->ResizeSurface(pw, ph);
  }
  const int64_t presentStart = NowUs();
  const bool presented = GpuPresentComposed(engine_, renderer_.get());
  RecordPresent(static_cast<uint64_t>(NowUs() - presentStart));
  frames_.fetch_add(1);
  if (presented) {
    presents_.fetch_add(1);
  } else if (engine_ != nullptr && !engine_->screenDirty()) {
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
    if (engine_ != nullptr) {
      const std::string t = engine_->TrafficStats();
      std::lock_guard<std::mutex> lock(errorMutex_);
      traffic_ = t;
    }
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  PaceFrame(frameStartUs, presented);
}

void GfxReplay::Run() {
  renderer_->Prepare();
  switch (static_cast<GfxReplayRoute>(route_.load())) {
    case GfxReplayRoute::kCpu:
      RunCpuReplay(gfxPath_);
      break;
    case GfxReplayRoute::kCompare:
      RunCompareReplay(gfxPath_);
      break;
    case GfxReplayRoute::kGpu:
    default:
      RunGpuReplay(gfxPath_);
      break;
  }
  endUs_.store(NowUs());
  running_.store(false);
}

void GfxReplay::RunGpuReplay(const std::string& gfxPath) {
  // Inject FreeRDP's ClearCodec decoder like a live session does, so the
  // replayed desktop is complete (ClearCodec bands are not self-contained).
  std::unique_ptr<GfxClearDecoder> clear = CreateFreeRdpClearDecoder();
  GfxGpuDesktop engine(clear.get());
  engine.SetClearBatchAreaLimit(g_clearBatchArea.load());
  if (!engine.Init()) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = "engine init failed";
    return;
  }
  ReplaySink sink(&engine, this);
  engine_ = &engine;

  // The replayed stream must not re-trigger the capture hook on the recorder.
  hmrdp::GfxDumpSetReplaying(true);

  std::string error;
  const int64_t pumpStart = NowUs();
  const bool ok = GfxReplayStream(gfxPath, &sink, [this]() { OnReplayFrame(); }, &running_,
                                  &error);
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  if (engine_ != nullptr) {
    const std::string t = engine_->TrafficStats();
    std::lock_guard<std::mutex> lock(errorMutex_);
    traffic_ = t;
  }
  hmrdp::GfxDumpSetReplaying(false);
  engine_ = nullptr;
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished: %{public}s", Stats().c_str());
}

void GfxReplay::RunCpuReplay(const std::string& gfxPath) {
  // FreeRDP's own gdi pipeline handles the decoding (clear/progressive/...),
  // exactly like a live session. Only the destination changes: gdi's primary
  // buffer is uploaded through the CPU DrawFrame path instead of a shared GPU
  // texture, so this route is the CPU reference for the GPU engine.
  GfxCpuDesktop cpu;
  std::string error;
  if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
    return;
  }
  cpu.SetFrameFn([this, &cpu]() { OnCpuFrame(&cpu); });

  hmrdp::GfxDumpSetReplaying(true);

  const int64_t pumpStart = NowUs();
  const bool ok = GfxReplayPump(gfxPath, cpu.gfx(), &running_, &error);
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

void GfxReplay::RunCompareReplay(const std::string& gfxPath) {
  // Correctness route: the GPU engine and FreeRDP's own gdi pipeline are fed the
  // exact same capture, and (sampled) frames are compared pixel by pixel. The
  // screen keeps showing the GPU route; the CPU desktop decodes but does not
  // present, so there is no double present and no timing meaning in this mode.
  GfxCpuDesktop cpu;
  std::string error;
  if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
    return;
  }
  std::unique_ptr<GfxClearDecoder> clear = CreateFreeRdpClearDecoder();
  GfxGpuDesktop engine(clear.get());
  engine.SetClearBatchAreaLimit(g_clearBatchArea.load());
  if (!engine.Init()) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = "engine init failed";
    return;
  }
  ReplaySink sink(&engine, this);
  engine_ = &engine;
  cpuDesktop_ = &cpu;

  hmrdp::GfxDumpSetReplaying(true);
  const int64_t pumpStart = NowUs();
  const bool ok = GfxReplayStreamCompare(
      gfxPath, &sink, [this]() { OnReplayFrame(); }, cpu.gfx(),
      [this]() { CompareFrames(); }, &running_, &error);
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  if (engine_ != nullptr) {
    const std::string t = engine_->TrafficStats();
    std::lock_guard<std::mutex> lock(errorMutex_);
    traffic_ = t;
  }
  hmrdp::GfxDumpSetReplaying(false);
  cpuDesktop_ = nullptr;
  engine_ = nullptr;
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished (compare): %{public}s", Stats().c_str());
}

void GfxReplay::CompareFrames() {
  GfxGpuDesktop* engine = engine_;
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
  uint32_t firstEngine = 0;
  uint32_t firstGdi = 0;
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
          std::memcpy(&firstEngine, pa, 4);
          std::memcpy(&firstGdi, pb, 4);
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
  if (firstX >= 0) {
    HMRDP_LOGW("gfx replay: compare first diff (%d,%d) engine=0x%08x gdi=0x%08x",
               firstX, firstY, static_cast<unsigned>(firstEngine), static_cast<unsigned>(firstGdi));
  }
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
    if (cmpFirstX_.load() < 0 && firstX >= 0) {
      cmpFirstX_.store(firstX);
      cmpFirstY_.store(firstY);
    }
    HMRDP_LOGW("gfx replay: compare diff rgb=%{public}llu alphaOnly=%{public}llu first=(%{public}d,%{public}d) frame=%{public}llu",
               static_cast<unsigned long long>(diffRgb),
               static_cast<unsigned long long>(diffAlpha), firstX, firstY,
               static_cast<unsigned long long>(frames_.load()));
  }
}

void GfxReplay::OnCpuFrame(GfxCpuDesktop* cpu) {
  if (cpu == nullptr) {
    return;
  }
  const int64_t frameStartUs = NowUs();
  if (frameStartUs - startUs_.load() > kMaxRunUs) {
    HMRDP_LOGI("gfx replay: 120s cap reached");
    running_.store(false);
    return;
  }
  const int pw = pendingW_.exchange(0);
  const int ph = pendingH_.exchange(0);
  if (pw > 0 && ph > 0) {
    renderer_->ResizeSurface(pw, ph);
  }
  // DrawFrame() letterboxes against the desktop size and refuses to draw until
  // it is known; the GPU route passes it to PresentTexture instead.
  renderer_->SetDesktopSize(cpu->width(), cpu->height());
  const int64_t presentStart = NowUs();
  const bool presented = PresentGdiFrame(cpu->gdi(), renderer_.get());
  RecordPresent(static_cast<uint64_t>(NowUs() - presentStart));
  frames_.fetch_add(1);
  if (presented) {
    presents_.fetch_add(1);
  } else {
    presentFailures_.fetch_add(1);
  }
  if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  PaceFrame(frameStartUs, presented);
}

}  // namespace hmrdp
