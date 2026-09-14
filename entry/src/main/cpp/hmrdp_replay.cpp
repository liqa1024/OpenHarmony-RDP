/*
 * HmRdp - dev-only recorded-RDP replay (PERF-TODO §4).
 *
 * Present-on-screen consumer of the shared replay driver: the capture is read,
 * decompressed and parsed by hmrdp_gfx_driver.cpp (FreeRDP's own ZGX + RDPGFX
 * parsing), the resulting commands go to a desktop engine (GLES or Vulkan), and
 * this file only presents the composed screen on every EndFrame - through the
 * very same Gpu{,Vk}PresentComposed() the live session uses. The GPU routes can
 * additionally run an offline gdi desktop on the same bytes and compare the two
 * screens pixel by pixel.
 */
#include "hmrdp_replay.h"

#include <native_window/external_window.h>

#include <freerdp/codec/clear.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/progressive.h>
#include <freerdp/codec/region.h>

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
#include "hmrdp_renderer.h"
#include "hmrdp_rfx.h"  // GfxGpuDesktop + GpuPresentComposed
#include "hmrdp_vk_desktop.h"
#include "hmrdp_vk_renderer.h"

namespace hmrdp {

// Engine-agnostic view of a desktop engine for the replay harness. GfxGpuDesktop
// (GLES) and GfxVkDesktop (Vulkan) reproduce the same GFX command semantics and
// each is presented through its own renderer, so the harness only needs this
// much of them and its routing/stats code stays free of backend types.
class ReplayDesktop {
 public:
  virtual ~ReplayDesktop() = default;
  // Brings up the engine + presenter on the given XComponent surface; returns
  // false and fills `error` on failure. `clearBatchArea` is the GLES ClearCodec
  // batching cap (ignored by the Vulkan engine).
  virtual bool Init(void* window, int width, int height, int clearBatchArea,
                    std::string* error) = 0;
  virtual void Resize(int width, int height) = 0;
  virtual void Apply(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                     const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                     uint32_t payloadLen) = 0;
  // Total ClearCodec flush work (map/unmap + CPU decode) so far; 0 when the
  // engine has no such round trip (Vulkan).
  virtual uint64_t ClearWorkUs() const = 0;
  // Composes and presents the engine screen. Returns false when nothing was
  // dirty (static frame) or the present failed.
  virtual bool Present() = 0;
  virtual bool screenDirty() const = 0;
  virtual bool ReadScreen(std::vector<uint8_t>* out) = 0;
  // Full surface (top-down, `stride` bytes) as BGRA; dev diagnostics only.
  virtual bool ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out) = 0;
  // Geometry of a GFX surface as the engine stores it (16B-aligned width/height,
  // stride in bytes); dev diagnostics only.
  virtual bool GetSurfaceInfo(uint16_t surfaceId, int* width, int* height, int* stride) const = 0;
  // One surface rect, tightly packed BGRA (`width * 4` per row). Dev only.
  virtual bool ReadSurfaceRect(uint16_t surfaceId, int x, int y, int width, int height,
                               std::vector<uint8_t>* out) = 0;
  // Bytes the engine's bitmap cache holds for `slot`. Dev only.
  virtual bool ReadCacheEntry(uint16_t slot, int* width, int* height,
                              std::vector<uint8_t>* out) = 0;
  virtual int screenWidth() const = 0;
  virtual int screenHeight() const = 0;
  // One-line engine summary for the dev panel.
  virtual std::string Summary() const = 0;
};

namespace {

constexpr int kFrameMs = 16;                      // ~60 Hz playback target
constexpr int kLogEvery = 120;
constexpr int64_t kMaxRunUs = 120ll * 1000000ll;  // safety cap
constexpr int kStartWaitUs = 3000000;
// Compare route: sample a full-screen readback every N frames (same idea as the
// live shadow check - reading the engine screen back is expensive).
constexpr uint64_t kCompareEvery = 30;
// Dev: per-message codec A/B (VULKAN-TODO §8). Off by default: it reads the
// engine's whole surface once per Progressive message / ClearCodec band, which
// costs ~26 MB per command and drags the replay well past real time. Flip to
// true when a decoder divergence has to be pinned to a message.
constexpr bool kCodecAbEnabled = false;

// ClearCodec batch granularity for the GLES route (union-rectangle pixel cap).
// Configurable so the sync-count vs mapped-bytes trade-off can be measured on
// real hardware instead of being fixed to a simulator-derived value.
std::atomic<int> g_clearBatchArea{1 << 20};

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
    case GfxReplayRoute::kGles:
      return "gles";
    case GfxReplayRoute::kVulkan:
      return "vulkan";
    case GfxReplayRoute::kGlesCompare:
      return "gles-compare";
    case GfxReplayRoute::kVulkanCompare:
      return "vulkan-compare";
  }
  return "?";
}

// GLES desktop engine (frozen legacy path, VULKAN-TODO §5 V7) + its EGL
// renderer.
class GlesReplayDesktop : public ReplayDesktop {
 public:
  bool Init(void* window, int width, int height, int clearBatchArea,
            std::string* error) override {
    clear_ = CreateFreeRdpClearDecoder();
    engine_ = std::make_unique<GfxGpuDesktop>(clear_.get());
    engine_->SetClearBatchAreaLimit(clearBatchArea);
    if (!engine_->Init()) {
      if (error != nullptr) {
        *error = "engine init failed";
      }
      engine_.reset();
      clear_.reset();
      return false;
    }
    renderer_ = std::make_unique<Renderer>();
    renderer_->SetSurface(window, width, height);
    renderer_->Prepare();
    return true;
  }

  void Resize(int width, int height) override { renderer_->ResizeSurface(width, height); }

  void Apply(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
             const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
             uint32_t payloadLen) override {
    engine_->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
  }

  uint64_t ClearWorkUs() const override { return engine_->ClearWorkUs(); }
  bool Present() override { return GpuPresentComposed(engine_.get(), renderer_.get()); }
  bool screenDirty() const override { return engine_->screenDirty(); }
  bool ReadScreen(std::vector<uint8_t>* out) override { return engine_->ReadScreen(out); }
  bool ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out) override {
    return engine_->ReadSurface(surfaceId, out);
  }
  bool GetSurfaceInfo(uint16_t surfaceId, int* width, int* height, int* stride) const override {
    const GpuSurface* s = engine_->FindSurface(surfaceId);
    if (s == nullptr) {
      return false;
    }
    *width = s->width;
    *height = s->height;
    *stride = s->stride;
    return true;
  }
  int screenWidth() const override { return engine_->screenWidth(); }
  int screenHeight() const override { return engine_->screenHeight(); }
  bool ReadSurfaceRect(uint16_t surfaceId, int x, int y, int width, int height,
                       std::vector<uint8_t>* out) override {
    return engine_->ReadSurfaceRect(surfaceId, x, y, width, height, out);
  }
  bool ReadCacheEntry(uint16_t slot, int* width, int* height,
                      std::vector<uint8_t>* out) override {
    return engine_->ReadCacheEntry(slot, width, height, out);
  }
  std::string Summary() const override { return engine_->TrafficStats(); }

 private:
  std::unique_ptr<GfxClearDecoder> clear_;
  std::unique_ptr<GfxGpuDesktop> engine_;
  std::unique_ptr<Renderer> renderer_;
};

// Vulkan desktop engine + its swapchain renderer (VULKAN-TODO §5 V2/V6).
class VulkanReplayDesktop : public ReplayDesktop {
 public:
  bool Init(void* window, int width, int height, int /*clearBatchArea*/,
            std::string* error) override {
    renderer_ = std::make_unique<VkRenderer>();
    renderer_->SetSurface(window, width, height);
    // Prepare() creates the swapchain and presents a black frame; it also pins
    // the image format the engine must be created with (a blit cannot convert
    // channel order, so engine and swapchain have to agree).
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

  void Resize(int width, int height) override { renderer_->ResizeSurface(width, height); }

  void Apply(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
             const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
             uint32_t payloadLen) override {
    engine_->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
  }

  // The Vulkan engine decodes ClearCodec directly on the mapped surface, so it
  // has no staging flush to account for.
  uint64_t ClearWorkUs() const override { return 0; }
  bool Present() override { return GpuVkPresentComposed(engine_.get(), renderer_.get()); }
  bool screenDirty() const override { return engine_->screenDirty(); }
  bool ReadScreen(std::vector<uint8_t>* out) override { return engine_->ReadScreen(out); }
  bool ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out) override {
    return engine_->ReadSurface(surfaceId, out);
  }
  bool GetSurfaceInfo(uint16_t surfaceId, int* width, int* height, int* stride) const override {
    const GpuSurface* s = engine_->FindSurface(surfaceId);
    if (s == nullptr) {
      return false;
    }
    *width = s->width;
    *height = s->height;
    *stride = s->stride;
    return true;
  }
  int screenWidth() const override { return engine_->screenWidth(); }
  int screenHeight() const override { return engine_->screenHeight(); }
  bool ReadSurfaceRect(uint16_t surfaceId, int x, int y, int width, int height,
                       std::vector<uint8_t>* out) override {
    return engine_->ReadSurfaceRect(surfaceId, x, y, width, height, out);
  }
  bool ReadCacheEntry(uint16_t slot, int* width, int* height,
                      std::vector<uint8_t>* out) override {
    return engine_->ReadCacheEntry(slot, width, height, out);
  }
  std::string Summary() const override { return engine_->Stats(); }

 private:
  std::unique_ptr<GfxVkDesktop> engine_;
  std::unique_ptr<VkRenderer> renderer_;
};

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
    // A command may trigger a ClearCodec flush before it runs; charge that time
    // to its own bucket so the per-class averages stay meaningful.
    const uint64_t flushBefore = engine_->ClearWorkUs();
    const int64_t t0 = NowUs();
    engine_->Apply(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
    const int64_t total = NowUs() - t0;
    const int64_t flushUs = static_cast<int64_t>(engine_->ClearWorkUs() - flushBefore);
    if (owner_ != nullptr) {
      owner_->RecordApply(cmdId, surfaceId, codecId,
                          static_cast<uint64_t>(total > flushUs ? total - flushUs : 0));
      if (flushUs > 0) {
        owner_->RecordFlush(static_cast<uint64_t>(flushUs));
      }
      // Dev: mirror the command into the independent CPU reference surface.
      if (cmdId == kGpuCmdCreateSurface && scalars != nullptr) {
        owner_->RefCreateSurface(static_cast<uint16_t>(surfaceId),
                                 static_cast<int>(scalars[0]), static_cast<int>(scalars[1]));
      } else if (cmdId == kGpuCmdDeleteSurface) {
        owner_->RefDeleteSurface(static_cast<uint16_t>(surfaceId));
      } else if (cmdId == kGpuCmdResetGraphics) {
        owner_->RefResetGraphics();
      } else if (cmdId == kGpuCmdWireToSurface && codecId == kGpuCodecCaprogressive &&
                 payload != nullptr && params != nullptr && paramsLen >= 32) {
        owner_->RefProgressive(static_cast<uint16_t>(surfaceId), payload, payloadLen,
                               static_cast<int>(RdU32(params + 8)),
                               static_cast<int>(RdU32(params + 12)));
      } else if (cmdId == kGpuCmdWireToSurface && codecId == kGpuCodecClearCodec &&
                 payload != nullptr && params != nullptr && paramsLen >= 32) {
        owner_->RefClearCodec(static_cast<uint16_t>(surfaceId), payload, payloadLen,
                              static_cast<int>(RdU32(params + 8)),
                              static_cast<int>(RdU32(params + 12)),
                              static_cast<int>(RdU32(params + 24)),
                              static_cast<int>(RdU32(params + 28)));
      } else if (cmdId == kGpuCmdSurfaceToCache && scalars != nullptr && params != nullptr &&
                 paramsLen >= 16) {
        const int sx = static_cast<int>(RdU16(params + 8));
        const int sy = static_cast<int>(RdU16(params + 10));
        owner_->RefCacheStore(static_cast<uint16_t>(surfaceId),
                              static_cast<uint16_t>(scalars[0]), sx, sy,
                              static_cast<int>(RdU16(params + 12)) - sx,
                              static_cast<int>(RdU16(params + 14)) - sy);
      } else if (cmdId == kGpuCmdCacheToSurface && scalars != nullptr && params != nullptr) {
        owner_->RefCacheRestore(static_cast<uint16_t>(surfaceId),
                                static_cast<uint16_t>(scalars[0]), params, scalars[1]);
      } else if (cmdId == kGpuCmdEvictCacheEntry && scalars != nullptr) {
        owner_->RefCacheEvict(static_cast<uint16_t>(scalars[0]));
      } else if (cmdId == kGpuCmdSolidFill && scalars != nullptr && params != nullptr) {
        owner_->RefFill(static_cast<uint16_t>(surfaceId),
                        (scalars[0] & 0x00FFFFFFu) | 0xFF000000u, params, scalars[1]);
      } else if (cmdId == kGpuCmdSurfaceToSurface && scalars != nullptr && params != nullptr &&
                 paramsLen >= 8) {
        owner_->RefCopy(static_cast<uint16_t>(scalars[0]), static_cast<uint16_t>(surfaceId), params,
                        scalars[1]);
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
    refSurface_.clear();
    refSurfaceId_ = 0xFFFFu;
    refW_ = 0;
    refH_ = 0;
    refStride_ = 0;
    refCache_.clear();
      refCommands_.store(0);
    refChecks_.store(0);
    refBad_.store(0);
    refBadPx_.store(0);
    refFirstLogged_ = false;
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
    // The pure CPU route presents raw gdi frames through the GLES renderer; the
    // desktop-engine routes build their own presenter inside the worker.
    if (route == GfxReplayRoute::kCpu) {
      renderer_ = std::make_unique<Renderer>();
      renderer_->SetSurface(window_, surfaceW_, surfaceH_);
    } else {
      renderer_.reset();
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

  const char* routeName = RouteName(static_cast<GfxReplayRoute>(route_.load()));
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
  const std::string rab = RefAbSummary();
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
      RunDesktopReplay(gfxPath_, true, false);
      break;
    case GfxReplayRoute::kGlesCompare:
      RunDesktopReplay(gfxPath_, false, true);
      break;
    case GfxReplayRoute::kVulkanCompare:
      RunDesktopReplay(gfxPath_, true, true);
      break;
    case GfxReplayRoute::kGles:
    default:
      RunDesktopReplay(gfxPath_, false, false);
      break;
  }
  endUs_.store(NowUs());
  running_.store(false);
}

void GfxReplay::RunDesktopReplay(const std::string& gfxPath, bool vulkan, bool compare) {
  std::unique_ptr<ReplayDesktop> desktop =
      vulkan ? std::unique_ptr<ReplayDesktop>(new VulkanReplayDesktop())
             : std::unique_ptr<ReplayDesktop>(new GlesReplayDesktop());
  std::string error;
  if (!desktop->Init(window_, surfaceW_, surfaceH_, g_clearBatchArea.load(), &error)) {
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
    // Dev: the two FreeRDP decoders behind the independent reference surface.
    refProg_ = progressive_context_new(FALSE);
    refClear_ = clear_context_new(FALSE);
    if (refProg_ == nullptr || refClear_ == nullptr) {
      HMRDP_LOGW("gfx replay: reference codec contexts unavailable");
    }
  }
  ReplaySink sink(desktop_.get(), this);

  // The replayed stream must not re-trigger the capture hook on the recorder.
  hmrdp::GfxDumpSetReplaying(true);

  const int64_t pumpStart = NowUs();
  bool ok = false;
  if (compare) {
    ok = GfxReplayStreamCompare(
        gfxPath, &sink, [this]() { OnReplayFrame(); }, cpu.gfx(),
        [this]() { CompareFrames(); }, &running_, &error);
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
  if (refProg_ != nullptr) {
    progressive_context_free(static_cast<PROGRESSIVE_CONTEXT*>(refProg_));
    refProg_ = nullptr;
  }
  if (refClear_ != nullptr) {
    clear_context_free(static_cast<CLEAR_CONTEXT*>(refClear_));
    refClear_ = nullptr;
  }
  {
    // Dev: independent-reference divergence summary.
    const std::string ab = RefAbSummary();
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
  // buffer is uploaded through the CPU DrawFrame path instead of a shared GPU
  // texture, so this route is the CPU reference for the GPU engine.
  renderer_->Prepare();
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

// Dev: independent CPU reference surface (VULKAN-TODO §8).
//
// The harness keeps a second, complete implementation of the GFX surface:
// FreeRDP's own progressive/clear decoders plus mirrored gdi cache / fill /
// copy semantics. The reference never reads the engine's surface, so diffing it
// against the engine's surface is an absolute end-to-end check - and the first
// mismatching pixel plus the last command that wrote it name the culprit.
// ---------------------------------------------------------------------------

namespace {

// Mirrors gdi_CacheToSurface / gdi_SolidFill / gdi_SurfaceToSurface geometry:
// `is_rect_valid` against the (16B-aligned) surface size.
bool RefRectValid(int x, int y, int w, int h, int limitW, int limitH) {
  return w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= limitW && y + h <= limitH;
}

void RefMarkProv(std::vector<uint8_t>* prov, int stride, int W, int H, int x, int y, int w, int h,
                 uint8_t code) {
  if (prov == nullptr || prov->empty()) {
    return;
  }
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) { w = W - x; }
  if (y + h > H) { h = H - y; }
  for (int row = 0; row < h; ++row) {
    std::memset(prov->data() + static_cast<size_t>(y + row) * stride + x, code,
                static_cast<size_t>(w));
  }
}

void RefFillRect(uint8_t* base, int stride, int left, int top, int width, int height,
                 uint32_t texel) {
  for (int row = 0; row < height; ++row) {
    uint32_t* line =
        reinterpret_cast<uint32_t*>(base + static_cast<size_t>(top + row) * stride) + left;
    for (int col = 0; col < width; ++col) {
      line[col] = texel;
    }
  }
}

void RefCopyRect(const uint8_t* src, int srcStride, int srcX, int srcY, uint8_t* dst,
                 int dstStride, int dstX, int dstY, int width, int height,
                 std::vector<uint8_t>* scratch) {
  scratch->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  for (int row = 0; row < height; ++row) {
    std::memcpy(scratch->data() + static_cast<size_t>(row) * rowBytes,
                src + static_cast<size_t>(srcY + row) * srcStride + static_cast<size_t>(srcX) * 4,
                rowBytes);
  }
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst + static_cast<size_t>(dstY + row) * dstStride + static_cast<size_t>(dstX) * 4,
                scratch->data() + static_cast<size_t>(row) * rowBytes, rowBytes);
  }
}

}  // namespace

std::string GfxReplay::RefAbSummary() const {
  if (refCommands_.load() == 0 && refChecks_.load() == 0) {
    return std::string();
  }
  char buf[224];
  std::snprintf(buf, sizeof(buf), "refAB cmds=%llu checks=%llu bad=%llu badPx=%llu",
                static_cast<unsigned long long>(refCommands_.load()),
                static_cast<unsigned long long>(refChecks_.load()),
                static_cast<unsigned long long>(refBad_.load()),
                static_cast<unsigned long long>(refBadPx_.load()));
  return std::string(buf);
}

void GfxReplay::RefCreateSurface(uint16_t surfaceId, int width, int height) {
  if (!kCodecAbEnabled) {
    return;
  }
  const int w = (width + 15) & ~15;
  const int h = (height + 15) & ~15;
  refSurfaceId_ = surfaceId;
  refW_ = w;
  refH_ = h;
  refStride_ = (w * 4 + 15) & ~15;
  refSurface_.assign(static_cast<size_t>(refStride_) * static_cast<size_t>(h), 0xFF);
  refProv_.assign(static_cast<size_t>(refStride_) * static_cast<size_t>(h), 0);
  refCache_.clear();
  refCommands_.store(0);
  refChecks_.store(0);
  refBad_.store(0);
  refBadPx_.store(0);
  refFirstLogged_ = false;
  if (refProg_ != nullptr) {
    PROGRESSIVE_CONTEXT* ctx = static_cast<PROGRESSIVE_CONTEXT*>(refProg_);
    progressive_delete_surface_context(ctx, surfaceId);
    progressive_create_surface_context(ctx, surfaceId, static_cast<UINT32>(w),
                                       static_cast<UINT32>(h));
  }
}

void GfxReplay::RefDeleteSurface(uint16_t surfaceId) {
  if (!kCodecAbEnabled) {
    return;
  }
  if (refProg_ != nullptr) {
    progressive_delete_surface_context(static_cast<PROGRESSIVE_CONTEXT*>(refProg_), surfaceId);
  }
  if (surfaceId == refSurfaceId_) {
    refSurface_.clear();
    refW_ = 0;
    refH_ = 0;
    refStride_ = 0;
    refSurfaceId_ = 0xFFFFu;
  }
}

void GfxReplay::RefResetGraphics() {
  if (!kCodecAbEnabled) {
    return;
  }
  // gdi_ResetGraphics wipes every surface to 0xFF and drops the invalid regions;
  // the ClearCodec sequence number restarts, the progressive state does not.
  if (!refSurface_.empty()) {
    std::fill(refSurface_.begin(), refSurface_.end(), 0xFF);
  }
  if (refClear_ != nullptr) {
    clear_context_reset(static_cast<CLEAR_CONTEXT*>(refClear_));
  }
}

void GfxReplay::RefProgressive(uint16_t surfaceId, const uint8_t* payload, size_t size, int left,
                               int top) {
  if (!kCodecAbEnabled || refProg_ == nullptr || payload == nullptr || size == 0 ||
      refSurface_.empty() || surfaceId != refSurfaceId_) {
    return;
  }
  REGION16 invalid;
  region16_init(&invalid);
  progressive_decompress(static_cast<PROGRESSIVE_CONTEXT*>(refProg_), payload,
                         static_cast<UINT32>(size), refSurface_.data(), PIXEL_FORMAT_BGRA32,
                         static_cast<UINT32>(refStride_), static_cast<UINT32>(left),
                         static_cast<UINT32>(top), &invalid, surfaceId,
                         static_cast<UINT32>(frames_.load()));
  {
    UINT32 nbRects = 0;
    const RECTANGLE_16* rects = region16_rects(&invalid, &nbRects);
    // Per-message A/B on exactly the rects FreeRDP composited (its own
    // updated-region): a difference here is a Progressive decode divergence, and
    // this names the message and rectangle.
    std::vector<uint8_t> actual;
    uint64_t bad = 0;
    int badX = -1;
    int badY = -1;
    for (UINT32 i = 0; i < nbRects; ++i) {
      const int rx = rects[i].left;
      const int ry = rects[i].top;
      const int rw = static_cast<int>(rects[i].right) - rx;
      const int rh = static_cast<int>(rects[i].bottom) - ry;
      RefMarkProv(&refProv_, refStride_, refW_, refH_, rx, ry, rw, rh, 1);
      if (rw <= 0 || rh <= 0 ||
          !desktop_->ReadSurfaceRect(surfaceId, rx, ry, rw, rh, &actual)) {
        continue;
      }
      for (int row = 0; row < rh; ++row) {
        const uint8_t* a = actual.data() + static_cast<size_t>(row) * rw * 4;
        const uint8_t* b = refSurface_.data() + static_cast<size_t>(ry + row) * refStride_ +
                           static_cast<size_t>(rx) * 4;
        for (int col = 0; col < rw; ++col) {
          if (a[col * 4 + 0] == b[col * 4 + 0] && a[col * 4 + 1] == b[col * 4 + 1] &&
              a[col * 4 + 2] == b[col * 4 + 2]) {
            continue;
          }
          if (badX < 0) {
            badX = rx + col;
            badY = ry + row;
          }
          bad++;
        }
      }
    }
    if (bad != 0 && !refFirstLogged_) {
      // Classify by tile kind so the failing decode sub-path is identified:
      // plain kFirst -> RLGR/dequant/DWT, kFirst+RFX_TILE_DIFFERENCE -> the
      // `current` add, kUpgrade -> SRL/raw + the persistent bit positions.
      uint64_t tilesFirst = 0;
      uint64_t tilesDiff = 0;
      uint64_t tilesUpgrade = 0;
      uint64_t badFirst = 0;
      uint64_t badDiff = 0;
      uint64_t badUpgrade = 0;
      uint64_t tilesSeen = 0;
      std::vector<uint8_t> ta;
      ParseRfxProgressive(
          payload, size,
          [&](const RfxTileRef& t) {
            tilesSeen++;
            const int tx = left + static_cast<int>(t.xIdx) * 64;
            const int ty = top + static_cast<int>(t.yIdx) * 64;
            const int tw = 64;
            const int th = 64;
            if (tx < 0 || ty < 0 || tx + tw > refW_ || ty + th > refH_) {
              return;
            }
            if (!desktop_->ReadSurfaceRect(surfaceId, tx, ty, tw, th, &ta)) {
              return;
            }
            uint64_t tileBad = 0;
            for (int row = 0; row < th; ++row) {
              const uint8_t* a = ta.data() + static_cast<size_t>(row) * tw * 4;
              const uint8_t* b = refSurface_.data() + static_cast<size_t>(ty + row) * refStride_ +
                                 static_cast<size_t>(tx) * 4;
              for (int col = 0; col < tw; ++col) {
                if (a[col * 4 + 0] == b[col * 4 + 0] && a[col * 4 + 1] == b[col * 4 + 1] &&
                    a[col * 4 + 2] == b[col * 4 + 2]) {
                  continue;
                }
                tileBad++;
              }
            }
            const bool upgrade = (t.type == RfxTileType::kUpgrade);
            const bool diff = !upgrade && ((t.flags & 1u) != 0u);
            if (upgrade) {
              tilesUpgrade++;
              badUpgrade += tileBad;
            } else if (diff) {
              tilesDiff++;
              badDiff += tileBad;
            } else {
              tilesFirst++;
              badFirst += tileBad;
            }
          },
          nullptr);
      {
        // Does this message's tile list contain the tile of the first bad pixel?
        const int bxIdx = (badX - left) / 64;
        const int byIdx = (badY - top) / 64;
        bool contains = false;
        ParseRfxProgressive(
            payload, size,
            [&](const RfxTileRef& t) {
              if (static_cast<int>(t.xIdx) == bxIdx && static_cast<int>(t.yIdx) == byIdx) {
                contains = true;
              }
            },
            nullptr);
      HMRDP_LOGW(
          "gfx replay: refAB bad pixel tile (%{public}d,%{public}d) in this message's tile list=%{public}d",
          bxIdx, byIdx, contains ? 1 : 0);
      if (!contains) {
        // Dump both tile sets: what our parser claims vs the tiles FreeRDP's own
        // updated rects imply (rect / 64).
        std::string mine;
        ParseRfxProgressive(
            payload, size,
            [&](const RfxTileRef& t) {
              if (mine.size() < 220) {
                mine += " (" + std::to_string(t.xIdx) + "," + std::to_string(t.yIdx) + ")" +
                        (t.type == RfxTileType::kUpgrade ? "u" : "f");
              }
            },
            nullptr);
        std::string theirs;
        for (UINT32 i = 0; i < nbRects && theirs.size() < 220; ++i) {
          theirs += " (" + std::to_string(rects[i].left / 64) + "," +
                    std::to_string(rects[i].top / 64) + ")->(" +
                    std::to_string((rects[i].right - 1) / 64) + "," +
                    std::to_string((rects[i].bottom - 1) / 64) + ")";
        }
        HMRDP_LOGW("gfx replay: refAB ourTiles:%{public}s", mine.c_str());
        HMRDP_LOGW("gfx replay: refAB freerdpRects:%{public}s", theirs.c_str());
        // Block sequence: FreeRDP *skips* (silently, no error) a REGION that comes
        // before FRAME_BEGIN or after FRAME_END (progressive_wb_region), while our
        // parser decodes every region.
        std::string blocks;
        size_t q = 0;
        while (q + 6 <= size && blocks.size() < 160) {
          const uint16_t bt = RdU16(payload + q);
          const uint32_t bl = RdU32(payload + q + 2);
          if (bl < 6 || q + bl > size) {
            break;
          }
          blocks += " " + std::to_string(bt);
          q += bl;
        }
        HMRDP_LOGW("gfx replay: refAB blocks:%{public}s", blocks.c_str());
      }
      }
      HMRDP_LOGW(
          "gfx replay: refAB progmsg tiles=%{public}llu (plain %{public}llu/%{public}llu px, diff %{public}llu/%{public}llu px, up %{public}llu/%{public}llu px) bad total=%{public}llu",
          static_cast<unsigned long long>(tilesSeen),
          static_cast<unsigned long long>(tilesFirst), static_cast<unsigned long long>(badFirst),
          static_cast<unsigned long long>(tilesDiff), static_cast<unsigned long long>(badDiff),
          static_cast<unsigned long long>(tilesUpgrade), static_cast<unsigned long long>(badUpgrade),
          static_cast<unsigned long long>(bad));
      std::vector<uint8_t> ea;
      desktop_->ReadSurfaceRect(surfaceId, badX, badY, 1, 1, &ea);
      const size_t off =
          static_cast<size_t>(badY) * refStride_ + static_cast<size_t>(badX) * 4;
      HMRDP_LOGW(
          "gfx replay: refAB progressive MISMATCH frame=%{public}llu msg#%{public}llu rects=%{public}u badPx=%{public}llu px=(%{public}d,%{public}d) engine=b%{public}u g%{public}u r%{public}u ref=b%{public}u g%{public}u r%{public}u origin=(%{public}d,%{public}d)",
          static_cast<unsigned long long>(frames_.load()),
          static_cast<unsigned long long>(refCommands_.load()), static_cast<unsigned>(nbRects),
          static_cast<unsigned long long>(bad), badX, badY, ea[0], ea[1], ea[2],
          refSurface_[off], refSurface_[off + 1], refSurface_[off + 2], left, top);
      std::ofstream rd(gfxPath_ + ".refdump", std::ios::out | std::ios::app);
      if (rd) {
        rd << "progmsg rects:\n";
        for (UINT32 i = 0; i < nbRects; ++i) {
          rd << "  rect " << rects[i].left << "," << rects[i].top << " - " << rects[i].right << ","
             << rects[i].bottom << "\n";
        }
      }
    }
  }
  region16_uninit(&invalid);
  refCommands_++;
}

void GfxReplay::RefClearCodec(uint16_t surfaceId, const uint8_t* payload, size_t size, int left,
                              int top, int width, int height) {
  if (!kCodecAbEnabled || refClear_ == nullptr || payload == nullptr || size == 0 ||
      refSurface_.empty() || surfaceId != refSurfaceId_ || width <= 0 || height <= 0) {
    return;
  }
  const int x = left < 0 ? 0 : left;
  const int y = top < 0 ? 0 : top;
  if (x + width > refW_ || y + height > refH_) {
    return;  // the engine rejects these too
  }
  clear_decompress(static_cast<CLEAR_CONTEXT*>(refClear_), payload, static_cast<UINT32>(size),
                   static_cast<UINT32>(width), static_cast<UINT32>(height), refSurface_.data(),
                   PIXEL_FORMAT_BGRA32, static_cast<UINT32>(refStride_), static_cast<UINT32>(x),
                   static_cast<UINT32>(y), static_cast<UINT32>(refW_), static_cast<UINT32>(refH_),
                   nullptr);
  RefMarkProv(&refProv_, refStride_, refW_, refH_, x, y, width, height, 2);
  refCommands_++;
}

void GfxReplay::RefCacheStore(uint16_t surfaceId, uint16_t slot, int x, int y, int width,
                              int height) {
  if (!kCodecAbEnabled || refSurface_.empty() || surfaceId != refSurfaceId_) {
    return;
  }
  if (!RefRectValid(x, y, width, height, refW_, refH_)) {
    return;  // gdi_SurfaceToCache fails and keeps the slot's old entry
  }
  RefCacheEntry entry;
  entry.width = width;
  entry.height = height;
  entry.data.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  for (int row = 0; row < height; ++row) {
    std::memcpy(entry.data.data() + static_cast<size_t>(row) * rowBytes,
                refSurface_.data() + static_cast<size_t>(y + row) * refStride_ +
                    static_cast<size_t>(x) * 4,
                rowBytes);
  }
  refCache_[slot] = std::move(entry);
  refCommands_++;
}

void GfxReplay::RefCacheEvict(uint16_t slot) {
  if (!kCodecAbEnabled) {
    return;
  }
  refCache_.erase(slot);
  refCommands_++;
}

void GfxReplay::RefCacheRestore(uint16_t surfaceId, uint16_t slot, const uint8_t* pts,
                                uint32_t count) {
  if (!kCodecAbEnabled || pts == nullptr || refSurface_.empty() || surfaceId != refSurfaceId_) {
    return;
  }
  const auto it = refCache_.find(slot);
  if (it == refCache_.end()) {
    return;  // gdi fails the command when the slot is empty
  }
  const RefCacheEntry& entry = it->second;
  std::vector<uint8_t> scratch;
  for (uint32_t i = 0; i < count; ++i) {
    const int px = static_cast<int>(RdU16(pts + static_cast<size_t>(i) * 4));
    const int py = static_cast<int>(RdU16(pts + static_cast<size_t>(i) * 4 + 2));
    if (!RefRectValid(px, py, entry.width, entry.height, refW_, refH_)) {
      return;  // gdi fails the whole command on the first invalid destination
    }
    RefCopyRect(entry.data.data(), entry.width * 4, 0, 0, refSurface_.data(), refStride_, px, py,
                entry.width, entry.height, &scratch);
    RefMarkProv(&refProv_, refStride_, refW_, refH_, px, py, entry.width, entry.height, 3);
  }
  refCommands_++;
}

void GfxReplay::RefFill(uint16_t surfaceId, uint32_t pixel, const uint8_t* rects, uint32_t count) {
  if (!kCodecAbEnabled || rects == nullptr || refSurface_.empty() || surfaceId != refSurfaceId_) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    int left = static_cast<int>(RdU16(rects + i * 8));
    int top = static_cast<int>(RdU16(rects + i * 8 + 2));
    int right = static_cast<int>(RdU16(rects + i * 8 + 4));
    int bottom = static_cast<int>(RdU16(rects + i * 8 + 6));
    // gdi_SolidFill intersects the rect with the surface and fills the result.
    if (right > refW_) right = refW_;
    if (bottom > refH_) bottom = refH_;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right <= left || bottom <= top) {
      continue;
    }
    RefFillRect(refSurface_.data(), refStride_, left, top, right - left, bottom - top, pixel);
    RefMarkProv(&refProv_, refStride_, refW_, refH_, left, top, right - left, bottom - top, 4);
  }
  refCommands_++;
}

void GfxReplay::RefCopy(uint16_t srcSurfaceId, uint16_t dstSurfaceId, const uint8_t* params,
                        uint32_t count) {
  if (!kCodecAbEnabled || params == nullptr || refSurface_.empty() ||
      dstSurfaceId != refSurfaceId_ || srcSurfaceId != refSurfaceId_) {
    return;
  }
  const int sx = static_cast<int>(RdU16(params));
  const int sy = static_cast<int>(RdU16(params + 2));
  const int w = static_cast<int>(RdU16(params + 4)) - sx;
  const int h = static_cast<int>(RdU16(params + 6)) - sy;
  if (!RefRectValid(sx, sy, w, h, refW_, refH_)) {
    return;  // gdi fails the whole command when rectSrc is outside the surface
  }
  std::vector<uint8_t> scratch;
  for (uint32_t i = 0; i < count; ++i) {
    const int px = static_cast<int>(RdU16(params + 8 + static_cast<size_t>(i) * 4));
    const int py = static_cast<int>(RdU16(params + 8 + static_cast<size_t>(i) * 4 + 2));
    if (!RefRectValid(px, py, w, h, refW_, refH_)) {
      return;  // gdi fails the whole command on the first invalid destination
    }
    // Sequential per destination, like gdi: a same-surface overlapping copy reads
    // what an earlier destination already wrote.
    RefCopyRect(refSurface_.data(), refStride_, sx, sy, refSurface_.data(), refStride_, px, py, w,
                h, &scratch);
    RefMarkProv(&refProv_, refStride_, refW_, refH_, px, py, w, h, 5);
  }
  refCommands_++;
}

void GfxReplay::RefCompareSurfaces() {
  if (!kCodecAbEnabled || desktop_ == nullptr || refSurface_.empty() || refW_ <= 0 ||
      refH_ <= 0) {
    return;
  }
  std::vector<uint8_t> engine;
  if (!desktop_->ReadSurface(refSurfaceId_, &engine)) {
    return;
  }
  if (engine.size() < refSurface_.size()) {
    return;
  }
  refChecks_++;
  uint64_t bad = 0;
  int firstX = -1;
  int firstY = -1;
  int maxDelta = 0;
  for (int row = 0; row < refH_; ++row) {
    const uint8_t* a = engine.data() + static_cast<size_t>(row) * refStride_;
    const uint8_t* b = refSurface_.data() + static_cast<size_t>(row) * refStride_;
    for (int col = 0; col < refW_; ++col) {
      if (a[col * 4 + 0] == b[col * 4 + 0] && a[col * 4 + 1] == b[col * 4 + 1] &&
          a[col * 4 + 2] == b[col * 4 + 2]) {
        continue;
      }
      if (firstX < 0) {
        firstX = col;
        firstY = row;
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
    return;
  }
  refBad_++;
  refBadPx_ += bad;
  if (!refFirstLogged_) {
    refFirstLogged_ = true;
    const uint8_t* a = engine.data() + (static_cast<size_t>(firstY) * refStride_) + firstX * 4;
    const uint8_t* b = refSurface_.data() + (static_cast<size_t>(firstY) * refStride_) + firstX * 4;
    const uint8_t prov =
        refProv_.empty()
            ? 0
            : refProv_[static_cast<size_t>(firstY) * static_cast<size_t>(refStride_) +
                       static_cast<size_t>(firstX)];
    HMRDP_LOGW(
        "gfx replay: refAB FIRST divergence frame=%{public}llu prov=%{public}u (1=prog 2=clear 3=cacheRestore 4=fill 5=copy) px=(%{public}d,%{public}d) engine=b%{public}u g%{public}u r%{public}u ref=b%{public}u g%{public}u r%{public}u",
        static_cast<unsigned long long>(frames_.load()), static_cast<unsigned>(prov), firstX,
        firstY, a[0], a[1], a[2], b[0], b[1], b[2]);
    // One-shot surroundings dump: the tile-aligned window around the first
    // divergence, from both surfaces, so the shape of the difference is visible
    // (a shifted tile, a rounded value, a missing clip, ...).
    const int x0 = (firstX / 64) * 64;
    const int y0 = (firstY / 64) * 64;
    const int x1 = x0 + 128 < refW_ ? x0 + 128 : refW_;
    const int y1 = y0 + 128 < refH_ ? y0 + 128 : refH_;
    std::ofstream dump(gfxPath_ + ".refdump", std::ios::out | std::ios::trunc);
    if (dump) {
      dump << "# frame=" << frames_.load() << " first=(" << firstX << "," << firstY
           << ") window=(" << x0 << "," << y0 << ")-(" << x1 << "," << y1 << ")\n";
      for (int row = y0; row < y1; ++row) {
        dump << "r" << row << " e:";
        for (int col = x0; col < x1; ++col) {
          const uint8_t* p = engine.data() + static_cast<size_t>(row) * refStride_ + col * 4;
          dump << ' ' << static_cast<unsigned>(p[0]) << '.' << static_cast<unsigned>(p[1]) << '.'
               << static_cast<unsigned>(p[2]);
        }
        dump << "\nr" << row << " f:";
        for (int col = x0; col < x1; ++col) {
          const uint8_t* p = refSurface_.data() + static_cast<size_t>(row) * refStride_ + col * 4;
          dump << ' ' << static_cast<unsigned>(p[0]) << '.' << static_cast<unsigned>(p[1]) << '.'
               << static_cast<unsigned>(p[2]);
        }
        dump << "\n";
      }
    }
  }
  HMRDP_LOGW(
      "gfx replay: refAB frame=%{public}llu badPx=%{public}llu maxDelta=%{public}d first=(%{public}d,%{public}d)",
      static_cast<unsigned long long>(frames_.load()), static_cast<unsigned long long>(bad),
      maxDelta, firstX, firstY);
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
  // Dev: the independent CPU reference surface is diffed against the engine's
  // surface on every compare; it does not depend on how (or whether) gdi
  // composed its primary, so it is the absolute correctness signal.
  RefCompareSurfaces();
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
  // Dev diagnosis: the surface is 1:1 mapped to the whole screen, so a diff that
  // is also a *surface* diff points at the decoder, while one where screen and
  // surface agree points at the compose/screen update (stale picture).
  std::vector<uint8_t> surface;
  const bool haveSurface = engine->ReadSurface(0, &surface);
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
  // Diffs where the engine's *surface* already holds the screen's (wrong) pixel:
  // the screen faithfully shows a wrongly decoded surface (decoder bug) versus
  // surface == gdi there, i.e. only the screen update went stale.
  size_t screenVsSurface = 0;
  size_t surfaceMatchesGdi = 0;
  const bool cmpSurface =
      haveSurface && surface.size() >= static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
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
        if (cmpSurface) {
          const uint8_t* ps = surface.data() + (static_cast<size_t>(row) * w + col) * 4;
          if (ps[0] == pa[0] && ps[1] == pa[1] && ps[2] == pa[2]) {
            screenVsSurface++;
          }
          if (ps[0] == pb[0] && ps[1] == pb[1] && ps[2] == pb[2]) {
            surfaceMatchesGdi++;
          }
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
    HMRDP_LOGW("gfx replay: compare diff rgb=%{public}llu alphaOnly=%{public}llu first=(%{public}d,%{public}d) frame=%{public}llu engWhite=%{public}llu gdiWhite=%{public}llu chanSwap=%{public}llu d255=%{public}llu maxDelta=%{public}d small=%{public}llu surfEqScreen=%{public}llu surfEqGdi=%{public}llu",
               static_cast<unsigned long long>(diffRgb),
               static_cast<unsigned long long>(diffAlpha), firstX, firstY,
               static_cast<unsigned long long>(frames_.load()),
               static_cast<unsigned long long>(engWhiteOnly),
               static_cast<unsigned long long>(gdiWhiteOnly),
               static_cast<unsigned long long>(chanSwap),
               static_cast<unsigned long long>(delta255), maxDelta,
               static_cast<unsigned long long>(smallDeltaPx),
               static_cast<unsigned long long>(screenVsSurface),
               static_cast<unsigned long long>(surfaceMatchesGdi));
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
