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
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_rfx.h"  // kGpuCmd / kGpuCodec / ParseRfxProgressive
#include "hmrdp_vk_desktop.h"
#include "hmrdp_vk_renderer.h"
#include "hmrdp_win_presenter.h"

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
  bool ReadScreen(std::vector<uint8_t>* out) { return engine_->ReadScreen(out); }
  // Full surface (top-down, `stride` bytes) as BGRA; dev diagnostics only.
  bool ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out) {
    return engine_->ReadSurface(surfaceId, out);
  }
  // One surface rect, tightly packed BGRA (`width * 4` per row). Dev only: the
  // per-command A/B reads a command's rect instead of the whole surface.
  bool ReadSurfaceRect(uint16_t surfaceId, int x, int y, int width, int height,
                       std::vector<uint8_t>* out) {
    return engine_->ReadSurfaceRect(surfaceId, x, y, width, height, out);
  }
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
constexpr int64_t kMaxRunUs = 120ll * 1000000ll;  // safety cap
constexpr int kStartWaitUs = 3000000;
// Compare route: sample a full-screen readback every N frames (same idea as the
// live shadow check - reading the engine screen back is expensive).
constexpr uint64_t kCompareEvery = 30;
// Dev: per-message codec A/B. Off by default: it reads the engine's whole surface
// once per Progressive message / ClearCodec band / cache command, which costs a
// GPU readback per command and drags the replay far past real time. Flip to true
// only while a decoder divergence has to be pinned to a message (the replay then
// looks hung / the picture stays black, so never leave it on).
constexpr bool kCodecAbEnabled = false;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Little-endian u32 out of a command's params blob (same layout the engines and
// hmrdp_gfx_driver use for surface commands).
uint32_t RdU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t RdU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
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
      // Dev (kCodecAbEnabled): record the rects this command claims to write, so
      // GdiAbFlush - one callback later, with gdi and the engine at the same
      // stream position - can diff exactly those pixels against gdi's own surface.
      // Only the command's own rects are checked: the pixels a Progressive message
      // also *re-composites* (FreeRDP's update_tiles) are covered by the full-tile
      // check of the message that decoded them.
      if (kCodecAbEnabled && params != nullptr) {
        if (cmdId == kGpuCmdWireToSurface && paramsLen >= 32 && payload != nullptr) {
          if (codecId == kGpuCodecCaprogressive) {
            const int ox = static_cast<int>(RdU32(params + 8));
            const int oy = static_cast<int>(RdU32(params + 12));
            ParseRfxProgressive(
                payload, payloadLen,
                [&](const RfxTileRef& t) {
                  owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId), ox + t.xIdx * 64,
                                     oy + t.yIdx * 64, 64, 64, "progressive-tiles");
                },
                nullptr);
          } else if (codecId == kGpuCodecClearCodec || codecId == kGpuCodecUncompressed) {
            const char* op = (codecId == kGpuCodecClearCodec) ? "clearcodec" : "upload";
            owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId),
                               static_cast<int>(RdU32(params + 8)),
                               static_cast<int>(RdU32(params + 12)),
                               static_cast<int>(RdU32(params + 24)),
                               static_cast<int>(RdU32(params + 28)), op);
          }
        } else if (cmdId == kGpuCmdSolidFill && scalars != nullptr) {
          for (uint32_t i = 0; i < scalars[1]; ++i) {
            const int left = static_cast<int>(RdU16(params + i * 8));
            const int top = static_cast<int>(RdU16(params + i * 8 + 2));
            const int right = static_cast<int>(RdU16(params + i * 8 + 4));
            const int bottom = static_cast<int>(RdU16(params + i * 8 + 6));
            owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId), left, top, right - left,
                               bottom - top, "fill");
          }
        } else if ((cmdId == kGpuCmdSurfaceToSurface || cmdId == kGpuCmdCacheToSurface) &&
                   scalars != nullptr && (cmdId == kGpuCmdSurfaceToSurface ? paramsLen >= 8 : true)) {
          // The dest points are u16 pairs; the blitted size comes from the engine's
          // surface rect for a copy and from the cache entry for a restore, so only
          // the points are known here - use a 64x64 window like the tile checks.
          const uint32_t count = scalars[1];
          const size_t base = (cmdId == kGpuCmdSurfaceToSurface) ? 8u : 0u;
          const char* op = (cmdId == kGpuCmdSurfaceToSurface) ? "surfaceToSurface" : "cacheRestore";
          for (uint32_t i = 0; i < count && (base + i * 4 + 4) <= paramsLen; ++i) {
            const int px = static_cast<int>(RdU16(params + base + i * 4));
            const int py = static_cast<int>(RdU16(params + base + i * 4 + 2));
            owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId), px, py, 64, 64, op);
          }
        }
      }
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
    cmpFirstX_.store(-1);
    cmpFirstY_.store(-1);
    cmpDumpDone_ = false;
    // Dev A/B state is per run: without this a second replay inside one app
    // session keeps the previous run's "first culprit" and dump flags and logs
    // nothing.
    gdiChecks_.store(0);
    gdiBad_.store(0);
    gdiBadPx_.store(0);
    gdiFirstLogged_ = false;
    gdiBadOp_.clear();
    gdiFirstLine_.clear();
    gdiAbPending_.clear();
    presentUs_.store(0);
    pumpUs_.store(0);
    paceUs_.store(0);
    startUs_.store(NowUs());
    endUs_.store(0);
    // The pure CPU route presents raw gdi frames through the Vulkan presenter;
    // the engine route builds its own presenter inside the worker.
    if (route == GfxReplayRoute::kCpu) {
      presenter_ = std::make_unique<WinPresenter>();
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
  HMRDP_LOGI("gfx replay: started route=%{public}s surface=%{public}dx%{public}d path=%{public}s",
             RouteName(static_cast<GfxReplayRoute>(route_.load())), surfaceW, surfaceH,
             gfxPath.c_str());
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

  const char* routeName = RouteName(static_cast<GfxReplayRoute>(route_.load()));
  const unsigned long long frames = static_cast<unsigned long long>(frames_.load());
  char head[320];
  std::snprintf(head, sizeof(head),
                "route=%s  frames=%llu  presents=%llu  fps=%.1f  fail=%llu  skip=%llu\n"
                "feed=%llums   parse=%llums   present=%.2fms   (running=%d)",
                routeName, frames,
                static_cast<unsigned long long>(presents), fps,
                static_cast<unsigned long long>(presentFailures_.load()),
                static_cast<unsigned long long>(presentSkips_.load()),
                static_cast<unsigned long long>(pumpUs / 1000),
                static_cast<unsigned long long>(hmrdp::GfxReplayParseUs() / 1000),
                avgMs(presentUs_.load(), presents),
                running_.load() ? 1 : 0);
  std::string out(head);

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
  const std::string rab = GdiAbSummary();
  if (!rab.empty()) {
    out += "\n";
    out += rab;
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
  PaceFrame(frameStartUs, presented);
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
  bool ok = false;
  if (compare) {
    // gdi and the engine consume the capture interleaved per PDU, so the
    // per-command A/B (GdiAbFlush, only with kCodecAbEnabled) runs right after
    // every command and CompareFrames() on gdi's EndFrame - both with both sides
    // at the same stream position. OnReplayFrame keeps presenting and pacing.
    ok = GfxReplayStreamCompare(gfxPath, &sink, [this]() { OnReplayFrame(); },
                                [this]() { CompareFrames(); }, [this]() { GdiAbFlush(); },
                                cpu.gfx(), &running_, &error);
  } else {
    ok = GfxReplayStream(gfxPath, &sink, [this]() { OnReplayFrame(); }, &running_, &error);
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
  if (!gdiFirstLine_.empty()) {
    // The periodic stats line floods hilog within seconds, so repeat the first
    // culprit once here (it is the entry point for every follow-up session).
    HMRDP_LOGW("gfx replay: gdiAB FIRST CULPRIT (rerun) %{public}s", gdiFirstLine_.c_str());
  }
  {
    // Dev: independent-reference divergence summary.
    const std::string ab = GdiAbSummary();
    if (!ab.empty()) {
      HMRDP_LOGI("gfx replay: %{public}s", ab.c_str());
    }
  }
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

std::string GfxReplay::GdiAbSummary() const {
  if (gdiChecks_.load() == 0) {
    return std::string();
  }
  char buf[256];
  std::snprintf(buf, sizeof(buf), "gdiAB checks=%llu bad=%llu badPx=%llu firstBadOp=%s",
                static_cast<unsigned long long>(gdiChecks_.load()),
                static_cast<unsigned long long>(gdiBad_.load()),
                static_cast<unsigned long long>(gdiBadPx_.load()),
                gdiBadOp_.empty() ? "-" : gdiBadOp_.c_str());
  return std::string(buf);
}

void GfxReplay::GdiAbCheck(uint16_t surfaceId, int x, int y, int width, int height,
                           const char* op) {
  // Records the rect this command claims to write; GdiAbFlush compares it against
  // gdi's own surface right after the command (both sides are at the same stream
  // position there, so the first failure names the culprit command).
  if (cpuDesktop_ == nullptr || width <= 0 || height <= 0 || gdiAbPending_.size() >= 4096) {
    return;
  }
  GdiAbRect rect;
  rect.x = x;
  rect.y = y;
  rect.w = width;
  rect.h = height;
  rect.surfaceId = surfaceId;
  rect.op = op;
  gdiAbPending_.push_back(rect);
}

void GfxReplay::GdiAbFlush() {
  // Authoritative A/B: the engine's surface vs FreeRDP's own gdi surface, for the
  // same id and at the same stream position. (An earlier, hand-written mirror of
  // the gdi geometry was removed: it produced false positives.)
  if (cpuDesktop_ == nullptr || gdiAbPending_.empty()) {
    return;
  }
  std::vector<GdiAbRect> pending;
  pending.swap(gdiAbPending_);
  for (const GdiAbRect& rect : pending) {
    int x = rect.x;
    int y = rect.y;
    int width = rect.w;
    int height = rect.h;
    int gw = 0;
    int gh = 0;
    int gstride = 0;
    uint32_t gformat = 0;
    const uint8_t* gdiSurface =
        cpuDesktop_->SurfaceData(rect.surfaceId, &gw, &gh, &gstride, &gformat);
    if (gdiSurface == nullptr || gstride <= 0 || gw <= 0 || gh <= 0) {
      continue;
    }
    if (x < 0) {
      width += x;
      x = 0;
    }
    if (y < 0) {
      height += y;
      y = 0;
    }
    if (x + width > gw) {
      width = gw - x;
    }
    if (y + height > gh) {
      height = gh - y;
    }
    if (width <= 0 || height <= 0) {
      continue;
    }
    std::vector<uint8_t> actual;
    if (!desktop_->ReadSurfaceRect(rect.surfaceId, x, y, width, height, &actual)) {
      continue;
    }
    if (actual.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
      continue;
    }
    gdiChecks_.fetch_add(1);
    uint64_t bad = 0;
    int firstX = -1;
    int firstY = -1;
    int maxDelta = 0;
    for (int row = 0; row < height; ++row) {
      const uint8_t* a = actual.data() + static_cast<size_t>(row) * width * 4;
      const uint8_t* b =
          gdiSurface + static_cast<size_t>(y + row) * gstride + static_cast<size_t>(x) * 4;
      for (int col = 0; col < width; ++col) {
        if (a[col * 4 + 0] == b[col * 4 + 0] && a[col * 4 + 1] == b[col * 4 + 1] &&
            a[col * 4 + 2] == b[col * 4 + 2]) {
          continue;
        }
        if (firstX < 0) {
          firstX = x + col;
          firstY = y + row;
        }
        for (int k = 0; k < 3; ++k) {
          const int d = static_cast<int>(a[col * 4 + k]) - static_cast<int>(b[col * 4 + k]);
          const int ad = d < 0 ? -d : d;
          if (ad > maxDelta) {
            maxDelta = ad;
          }
        }
        bad++;
      }
    }
    if (bad == 0) {
      continue;
    }
    gdiBad_.fetch_add(1);
    gdiBadPx_.fetch_add(bad);
    if (!gdiFirstLogged_) {
      gdiFirstLogged_ = true;
      if (gdiBadOp_.empty()) {
        gdiBadOp_ = rect.op;
      }
      // Report the first failing *pixel* (both sides), not the rect origin.
      const size_t off = (static_cast<size_t>(firstY) * gstride) + static_cast<size_t>(firstX) * 4;
      const size_t eoff =
          (static_cast<size_t>(firstY - y) * static_cast<size_t>(width) +
           static_cast<size_t>(firstX - x)) *
          4;
      char firstLine[320];
      std::snprintf(firstLine, sizeof(firstLine),
                    "op=%s rect=(%d,%d)+%dx%d bad=%llu maxDelta=%d px=(%d,%d) engine=b%u g%u r%u gdi=b%u g%u r%u",
                    rect.op.c_str(), x, y, width, height, static_cast<unsigned long long>(bad),
                    maxDelta, firstX, firstY, actual[eoff], actual[eoff + 1], actual[eoff + 2],
                    gdiSurface[off], gdiSurface[off + 1], gdiSurface[off + 2]);
      gdiFirstLine_ = firstLine;
      HMRDP_LOGW("gfx replay: gdiAB CULPRIT %{public}s", firstLine);
    }
  }
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
  // Attribution counters (dev diagnostics): distinguishes "the engine never
  // painted" from "it painted a different colour" from "channels swapped".
  size_t engWhiteOnly = 0;  // engine white (0xFFFFFFFF), gdi not: missing paint
  size_t gdiWhiteOnly = 0;  // gdi white, engine not: extra paint / wrong clip
  size_t chanSwap = 0;      // engine pixel == gdi with R/B swapped
  size_t delta255 = 0;      // at least one channel off by 255
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
        if (worst == 255) delta255++;
        const bool engWhite = (pa[0] == 0xFF && pa[1] == 0xFF && pa[2] == 0xFF);
        const bool gdiWhite = (pb[0] == 0xFF && pb[1] == 0xFF && pb[2] == 0xFF);
        if (engWhite && !gdiWhite) {
          engWhiteOnly++;
        } else if (gdiWhite && !engWhite) {
          gdiWhiteOnly++;
        } else if (pa[0] == pb[2] && pa[1] == pb[1] && pa[2] == pb[0]) {
          chanSwap++;
        }
        diffRgb++;
      } else if (pa[3] != pb[3]) {
        diffAlpha++;
      }
    }
  }
  cmpSmallDeltaPx_.fetch_add(smallDeltaPx);
  // Dev diagnosis: on the first small (analysable) divergence, dump every
  // differing pixel so the pattern can be inspected offline.
  if (firstX >= 0 && cmpDumpDone_ == false) {
    cmpDumpDone_ = true;
    std::ofstream out(gfxPath_ + ".cmpdump", std::ios::out | std::ios::trunc);
    if (out) {
      out << "# frame=" << frames_.load() << " diff=" << diffRgb << " bbox=(" << bx0 << "," << by0
          << ")-(" << bx1 << "," << by1 << ")\n";
      for (int row = by0; row <= by1 && static_cast<int>(out.tellp()) < 200000; ++row) {
        const uint8_t* a2 = screen.data() + static_cast<size_t>(row) * w * 4;
        const uint8_t* b2 = gdi->primary_buffer + static_cast<size_t>(row) * gdi->stride;
        for (int col = bx0; col <= bx1; ++col) {
          const uint8_t* pa2 = a2 + col * 4;
          const uint8_t* pb2 = b2 + col * 4;
          if (pa2[0] == pb2[0] && pa2[1] == pb2[1] && pa2[2] == pb2[2]) {
            continue;
          }
          char line[128];
          std::snprintf(line, sizeof(line), "%d %d e=%02x%02x%02x g=%02x%02x%02x\n", col, row,
                        pa2[0], pa2[1], pa2[2], pb2[0], pb2[1], pb2[2]);
          out << line;
        }
      }
    }
    HMRDP_LOGW("gfx replay: compare dump written (frame=%{public}llu diff=%{public}llu)",
               static_cast<unsigned long long>(frames_.load()),
               static_cast<unsigned long long>(diffRgb));
  }
  if (firstX >= 0) {
    HMRDP_LOGW(
        "gfx replay: compare first diff (%{public}d,%{public}d) engine=0x%{public}x "
        "gdi=0x%{public}x",
        firstX, firstY, static_cast<unsigned>(firstEngine), static_cast<unsigned>(firstGdi));
  }
  if (bx1 >= 0) {
    cmpBBoxX0_.store(bx0);
    cmpBBoxY0_.store(by0);
    cmpBBoxX1_.store(bx1);
    cmpBBoxY1_.store(by1);
    cmpMaxDelta_.store(maxDelta);
    HMRDP_LOGW("gfx replay: compare bbox frame=%{public}llu diff=%{public}llu bbox=(%{public}d,%{public}d)-(%{public}d,%{public}d) maxDelta=%{public}d",
               static_cast<unsigned long long>(frames_.load()),
               static_cast<unsigned long long>(diffRgb), bx0, by0, bx1, by1, maxDelta);
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
    HMRDP_LOGW("gfx replay: compare diff rgb=%{public}llu alphaOnly=%{public}llu first=(%{public}d,%{public}d) frame=%{public}llu engWhite=%{public}llu gdiWhite=%{public}llu chanSwap=%{public}llu d255=%{public}llu maxDelta=%{public}d small=%{public}llu",
               static_cast<unsigned long long>(diffRgb),
               static_cast<unsigned long long>(diffAlpha), firstX, firstY,
               static_cast<unsigned long long>(frames_.load()),
               static_cast<unsigned long long>(engWhiteOnly),
               static_cast<unsigned long long>(gdiWhiteOnly),
               static_cast<unsigned long long>(chanSwap),
               static_cast<unsigned long long>(delta255), maxDelta,
               static_cast<unsigned long long>(smallDeltaPx));
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
    presenter_->ResizeSurface(pw, ph);
  }
  const int64_t presentStart = NowUs();
  const bool presented = PresentGdiFrame(cpu->gdi(), presenter_.get());
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
