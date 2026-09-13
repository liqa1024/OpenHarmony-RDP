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
    const int64_t t0 = NowUs();
    engine_->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
    if (owner_ != nullptr) {
      owner_->RecordApply(cmdId, surfaceId, codecId, static_cast<uint64_t>(NowUs() - t0));
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
    }
    window_ = nativeWindow;
    surfaceW_ = surfaceW;
    surfaceH_ = surfaceH;
    gfxPath_ = gfxPath;
    route_.store(static_cast<int>(route));
    frames_.store(0);
    presents_.store(0);
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
    clearRunSum_.store(0);
    clearRunCount_.store(0);
    clearRunMax_.store(0);
    clearRunLen_ = 0;
    clearRunSurface_ = 0xFFFFFFFFu;
    clearRunActive_ = false;
    presentUs_.store(0);
    pumpUs_.store(0);
    paceUs_.store(0);
    startUs_.store(NowUs());
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

std::string GfxReplay::Stats() {
  const int64_t elapsedUs = startUs_.load() != 0 ? NowUs() - startUs_.load() : 0;
  const uint64_t presents = presents_.load();
  const double fps = elapsedUs > 0 ? static_cast<double>(presents) * 1000000.0 /
                                         static_cast<double>(elapsedUs)
                                   : 0.0;
  auto avgMs = [](uint64_t total, uint64_t count) {
    return count > 0 ? static_cast<double>(total) / static_cast<double>(count) / 1000.0 : 0.0;
  };
  std::string err;
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    err = lastError_;
  }
  const uint64_t pumpRaw = pumpUs_.load();
  const uint64_t pace = paceUs_.load();
  const uint64_t pumpUs = pumpRaw > pace ? pumpRaw - pace : 0;
  char buf[768];
  std::snprintf(
      buf, sizeof(buf),
      "route=%s running=%d frames=%llu presents=%llu fps=%.1f fail=%llu feedMs=%llu "
      "[ms/count] prog %.2f/%llu clear %.2f/%llu unc %.2f/%llu fill %.2f/%llu "
      "blit %.2f/%llu cache %.2f/%llu other %.2f/%llu [clearRun avg %.1f max %llu n %llu] "
      "present %.2f err=%s",
      route_.load() == static_cast<int>(GfxReplayRoute::kCpu) ? "cpu" : "gpu",
      running_.load() ? 1 : 0, static_cast<unsigned long long>(frames_.load()),
      static_cast<unsigned long long>(presents), fps,
      static_cast<unsigned long long>(presentFailures_.load()),
      static_cast<unsigned long long>(pumpUs / 1000), avgMs(progUs_.load(), progCount_.load()),
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
      clearRunCount_.load() > 0
          ? static_cast<double>(clearRunSum_.load()) / static_cast<double>(clearRunCount_.load())
          : 0.0,
      static_cast<unsigned long long>(clearRunMax_.load()),
      static_cast<unsigned long long>(clearRunCount_.load()), avgMs(presentUs_.load(), presents),
      err.empty() ? "-" : err.c_str());
  return std::string(buf);
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

void GfxReplay::RecordPresent(uint64_t micros) {
  presentUs_.fetch_add(micros);
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
  } else {
    presentFailures_.fetch_add(1);
  }
  if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  PaceFrame(frameStartUs, presented);
}

void GfxReplay::Run() {
  renderer_->Prepare();
  if (route_.load() == static_cast<int>(GfxReplayRoute::kCpu)) {
    RunCpuReplay(gfxPath_);
  } else {
    RunGpuReplay(gfxPath_);
  }
  running_.store(false);
}

void GfxReplay::RunGpuReplay(const std::string& gfxPath) {
  // Inject FreeRDP's ClearCodec decoder like a live session does, so the
  // replayed desktop is complete (ClearCodec bands are not self-contained).
  std::unique_ptr<GfxClearDecoder> clear = CreateFreeRdpClearDecoder();
  GfxGpuDesktop engine(clear.get());
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
