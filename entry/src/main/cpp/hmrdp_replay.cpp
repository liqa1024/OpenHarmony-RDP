/*
 * HmRdp - dev-only recorded-RDP replay (see hmrdp_replay.h).
 */
#include "hmrdp_replay.h"

#include <native_window/external_window.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "hmrdp_gfx_capture.h"
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

}  // namespace

GfxReplay& GfxReplay::Instance() {
  static GfxReplay instance;
  return instance;
}

GfxReplay::~GfxReplay() {
  Stop();
}

bool GfxReplay::Start(void* nativeWindow, int surfaceW, int surfaceH,
                      const std::string& gfxPath) {
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
    frames_.store(0);
    presents_.store(0);
    presentFailures_.store(0);
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
  HMRDP_LOGI("gfx replay: started surface=%{public}dx%{public}d path=%{public}s", surfaceW,
             surfaceH, gfxPath.c_str());
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
  std::string err;
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    err = lastError_;
  }
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "running=%d frames=%llu presents=%llu fps=%.1f presentFail=%llu err=%s",
                running_.load() ? 1 : 0,
                static_cast<unsigned long long>(frames_.load()),
                static_cast<unsigned long long>(presents), fps,
                static_cast<unsigned long long>(presentFailures_.load()),
                err.empty() ? "-" : err.c_str());
  return std::string(buf);
}

void GfxReplay::Run() {
  renderer_->Prepare();
  RunReplay(gfxPath_);
  running_.store(false);
}

void GfxReplay::RunReplay(const std::string& gfxPath) {
  auto fail = [this](const char* why) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = why;
  };

  // Inject FreeRDP's ClearCodec decoder like a live session does, so the
  // replayed desktop is complete (ClearCodec bands are not self-contained).
  std::unique_ptr<GfxClearDecoder> clear = CreateFreeRdpClearDecoder();
  GfxGpuDesktop engine(clear.get());
  if (!engine.Init()) {
    fail("engine init failed");
    return;
  }
  GfxCapture capture;
  if (!capture.Open(gfxPath)) {
    fail("cannot open capture");
    return;
  }
  auto nextFrame = std::chrono::steady_clock::now();
  GfxCaptureRecord rec;
  while (running_.load() && capture.Next(&rec)) {
    engine.ApplyCommand(rec.cmdId, rec.surfaceId, rec.scalars, rec.params, rec.paramsLen,
                        rec.payload, rec.payloadLen);
    if (rec.cmdId == kGpuCmdEndFrame) {
      const int pw = pendingW_.exchange(0);
      const int ph = pendingH_.exchange(0);
      if (pw > 0 && ph > 0) {
        renderer_->ResizeSurface(pw, ph);
      }
      if (GpuPresentComposed(&engine, renderer_.get())) {
        presents_.fetch_add(1);
      } else {
        presentFailures_.fetch_add(1);
      }
      frames_.fetch_add(1);
      if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
        HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
      }
      nextFrame += std::chrono::milliseconds(kFrameMs);
      std::this_thread::sleep_until(nextFrame);
    }
    if (NowUs() - startUs_.load() > kMaxRunUs) {
      HMRDP_LOGI("gfx replay: 120s cap reached");
      break;
    }
  }
  HMRDP_LOGI("gfx replay: finished: %{public}s", Stats().c_str());
}

}  // namespace hmrdp
