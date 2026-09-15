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
  // Dev/verification only: the engine's persistent Progressive state for one
  // (tile, component) stream, for the coefficient-level A/B against FreeRDP
  // (see GfxVkDesktop::ReadRfxTileState). Backends without such state return
  // false.
  virtual bool ReadRfxTileState(uint16_t surfaceId, uint32_t tileIndex, int component,
                                std::vector<int16_t>* cur, std::vector<int16_t>* sign,
                                std::vector<uint8_t>* bitPos) {
    return false;
  }
  virtual int screenWidth() const = 0;
  virtual int screenHeight() const = 0;
  // One-line engine summary for the dev panel.
  virtual std::string Summary() const = 0;
};

namespace {

constexpr int kFrameMs = 16;                      // ~60 Hz playback target
constexpr int kLogEvery = 120;
constexpr int64_t kMaxRunUs = 900ll * 1000000ll;  // safety cap (debug: A/B slows the pump)
constexpr int kStartWaitUs = 3000000;
// Compare route: sample a full-screen readback every N frames (same idea as the
// live shadow check - reading the engine screen back is expensive).
constexpr uint64_t kCompareEvery = 30;
// Dev: per-message codec A/B (VULKAN-TODO §8). Off by default: it reads the
// engine's whole surface once per Progressive message / ClearCodec band, which
// costs ~26 MB per command and drags the replay well past real time. Flip to
// true when a decoder divergence has to be pinned to a message.
constexpr bool kCodecAbEnabled = true;

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
  // The Vulkan engine's Progressive state buffers are persistent-mapped, so the
  // per-(tile,component) decoder state can be read back for the A/B.
  bool ReadRfxTileState(uint16_t surfaceId, uint32_t tileIndex, int component,
                        std::vector<int16_t>* cur, std::vector<int16_t>* sign,
                        std::vector<uint8_t>* bitPos) override {
    return engine_->ReadRfxTileState(surfaceId, tileIndex, component, cur, sign, bitPos);
  }
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
      if (cmdId == kGpuCmdStartFrame && scalars != nullptr) {
        // FreeRDP resets its Progressive per-frame tile list when the *wire*
        // frame id changes; the mirror has to use the same id gdi uses, not the
        // harness's frame counter (which the compare route does not even
        // advance), otherwise its update_tiles re-composites a different set.
        owner_->SetMirrorFrameId(scalars[0]);
      } else if (cmdId == kGpuCmdCreateSurface && scalars != nullptr) {
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
      } else if (cmdId == kGpuCmdWireToSurface && codecId == kGpuCodecUncompressed &&
                 payload != nullptr && params != nullptr && paramsLen >= 32) {
        owner_->RefUpload(static_cast<uint16_t>(surfaceId), RdU32(params + 4),
                          static_cast<int>(RdU32(params + 8)), static_cast<int>(RdU32(params + 12)),
                          static_cast<int>(RdU32(params + 24)),
                          static_cast<int>(RdU32(params + 28)), payload, payloadLen);
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
    // Dev A/B state is per run: without this a second replay inside one app
    // session keeps the previous run's "first culprit" and dump flags and logs
    // nothing.
    gdiChecks_.store(0);
    gdiBad_.store(0);
    gdiBadPx_.store(0);
    gdiFirstLogged_ = false;
    gdiBadOp_.clear();
    gdiWatchLogged_ = 0;
    gdiAbPending_.clear();
    gdiAbPayloads_.clear();
    gdiAbPayloadNext_ = 0;
    tileDumpDone_ = false;
    tileBitPos_.clear();
    mirrorFrameId_.store(0);
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
    refBadOp_.clear();
    refCacheMisses_ = 0;
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
    // gdi and the engine/mirror now consume the capture interleaved per PDU, so
    // the per-command A/B (GdiAbFlush) runs right after every command and
    // CompareFrames() on gdi's EndFrame - both with both sides at the same stream
    // position. OnReplayFrame keeps presenting and pacing the replay.
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

// Mirrors the engine's QuantArray / rfx_compose.comp band order: the shader's
// kSubOff order HL1 LH1 HH1 HL2 LH2 HH2 HL3 LH3 HH3 LL3.
void QuantNibbles(const RfxQuant& q, uint8_t out[10]) {
  out[0] = q.HL1;
  out[1] = q.LH1;
  out[2] = q.HH1;
  out[3] = q.HL2;
  out[4] = q.LH2;
  out[5] = q.HH2;
  out[6] = q.HL3;
  out[7] = q.LH3;
  out[8] = q.HH3;
  out[9] = q.LL3;
}

// Dev formatting: "a,b,..." for a 10-entry band array.
std::string JoinNibbles(const int vals[10]) {
  std::string out;
  for (int i = 0; i < 10; ++i) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d,", vals[i]);
    out += buf;
  }
  return out;
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
  if (refCommands_.load() == 0 && refChecks_.load() == 0 && gdiChecks_.load() == 0) {
    return std::string();
  }
  char buf[384];
  std::snprintf(buf, sizeof(buf),
                "refAB cmds=%llu checks=%llu bad=%llu badPx=%llu firstBadOp=%s "
                "gdiAB checks=%llu bad=%llu badPx=%llu firstBadOp=%s",
                static_cast<unsigned long long>(refCommands_.load()),
                static_cast<unsigned long long>(refChecks_.load()),
                static_cast<unsigned long long>(refBad_.load()),
                static_cast<unsigned long long>(refBadPx_.load()),
                refBadOp_.empty() ? "-" : refBadOp_.c_str(),
                static_cast<unsigned long long>(gdiChecks_.load()),
                static_cast<unsigned long long>(gdiBad_.load()),
                static_cast<unsigned long long>(gdiBadPx_.load()),
                gdiBadOp_.empty() ? "-" : gdiBadOp_.c_str());
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
  // NOTE: the GFX bitmap cache is NOT cleared here - gdi's cache lives in the gfx
  // context and survives CreateSurface/DeleteSurface/ResetGraphics (only the
  // *surfaces* are wiped). Clearing it made the reference skip restores the
  // engine correctly performed.
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

void GfxReplay::RefWatchRect(uint16_t surfaceId, int x, int y, int width, int height,
                             const char* op) {
  if (!kCodecAbEnabled || refSurface_.empty() || refWatchLogged_ >= 64) {
    return;
  }
  if (refWatchX_ < x || refWatchX_ >= x + width || refWatchY_ < y || refWatchY_ >= y + height) {
    return;
  }
  std::vector<uint8_t> ea;
  if (!desktop_->ReadSurfaceRect(surfaceId, refWatchX_, refWatchY_, 1, 1, &ea)) {
    return;
  }
  const size_t off =
      static_cast<size_t>(refWatchY_) * refStride_ + static_cast<size_t>(refWatchX_) * 4;
  refWatchLogged_++;
  HMRDP_LOGW(
      "gfx replay: refAB watch(%{public}d,%{public}d) cmd#%{public}llu op=%{public}s rect=(%{public}d,%{public}d)+%{public}dx%{public}d engine=b%{public}u g%{public}u r%{public}u ref=b%{public}u g%{public}u r%{public}u",
      refWatchX_, refWatchY_, static_cast<unsigned long long>(refCommands_.load()), op, x, y, width,
      height, ea[0], ea[1], ea[2], refSurface_[off], refSurface_[off + 1], refSurface_[off + 2]);
}

bool GfxReplay::RefVerifyRect(uint16_t surfaceId, int x, int y, int width, int height,
                              const char* op) {
  GdiAbCheck(surfaceId, x, y, width, height, op);
  if (!kCodecAbEnabled || desktop_ == nullptr || refSurface_.empty() || width <= 0 ||
      height <= 0) {
    return true;
  }
  if (x < 0) {
    width += x;
    x = 0;
  }
  if (y < 0) {
    height += y;
    y = 0;
  }
  if (x + width > refW_) {
    width = refW_ - x;
  }
  if (y + height > refH_) {
    height = refH_ - y;
  }
  if (width <= 0 || height <= 0) {
    return true;
  }
  std::vector<uint8_t> actual;
  if (!desktop_->ReadSurfaceRect(surfaceId, x, y, width, height, &actual)) {
    return true;
  }
  refChecks_++;
  uint64_t bad = 0;
  int firstX = -1;
  int firstY = -1;
  int maxDelta = 0;
  for (int row = 0; row < height; ++row) {
    const uint8_t* a = actual.data() + static_cast<size_t>(row) * width * 4;
    const uint8_t* b =
        refSurface_.data() + static_cast<size_t>(y + row) * refStride_ + static_cast<size_t>(x) * 4;
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
    return true;
  }
  refBad_++;
  refBadPx_ += bad;
  // Record the failing pixel for every failure (not just the first one) so the
  // Progressive path can report the tile of each culprit message.
  refBadX_ = firstX;
  refBadY_ = firstY;
  refBadThisMessage_ = true;
  if (!refFirstLogged_) {
    refFirstLogged_ = true;
    if (refBadOp_.empty()) {
      refBadOp_ = op;
    }
    const size_t off = (static_cast<size_t>(firstY) * refStride_) + static_cast<size_t>(firstX) * 4;
    std::vector<uint8_t> ea;
    desktop_->ReadSurfaceRect(surfaceId, firstX, firstY, 1, 1, &ea);
    // Provenance of the reference pixel: 0 = never written by any mirrored op,
    // 1 = progressive, 2 = clearcodec, 3 = cacheRestore, 4 = fill, 5 = copy,
    // 6 = uncompressed upload.
    const uint8_t rprov =
        refProv_.empty() ? 0xFF : refProv_[static_cast<size_t>(firstY) * refStride_ + firstX];
    HMRDP_LOGW(
        "gfx replay: refAB CULPRIT op=%{public}s cmd#%{public}llu rect=(%{public}d,%{public}d)+%{public}dx%{public}d bad=%{public}llu maxDelta=%{public}d px=(%{public}d,%{public}d) engine=b%{public}u g%{public}u r%{public}u ref=b%{public}u g%{public}u r%{public}u refProv=%{public}u",
        op, static_cast<unsigned long long>(refCommands_.load()), x, y, width, height,
        static_cast<unsigned long long>(bad), maxDelta, firstX, firstY, ea[0], ea[1], ea[2],
        refSurface_[off], refSurface_[off + 1], refSurface_[off + 2], static_cast<unsigned>(rprov));
  }
  return false;
}

int GfxReplay::GdiAbStashPayload(const uint8_t* payload, uint32_t payloadLen) {
  if (payload == nullptr || payloadLen == 0) {
    return -1;
  }
  if (gdiAbPayloads_.empty()) {
    gdiAbPayloads_.resize(16);
  }
  gdiAbPayloads_[gdiAbPayloadNext_ % gdiAbPayloads_.size()] =
      std::vector<uint8_t>(payload, payload + payloadLen);
  return static_cast<int>(gdiAbPayloadNext_++);
}

void GfxReplay::GdiAbCheck(uint16_t surfaceId, int x, int y, int width, int height, const char* op,
                           const std::string& detail, int payloadSlot) {
  // The engine has applied this command, but gdi (fed one chunk behind) has not,
  // so the comparison is deferred to the next chunk boundary. Only the rect is
  // recorded here.
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
  rect.detail = detail;
  rect.payloadSlot = payloadSlot;
  gdiAbPending_.push_back(rect);
}

void GfxReplay::GdiAbFlush() {
  // Authoritative A/B: the engine's surface vs FreeRDP's own gdi surface for the
  // same id, both quiescent on the same chunk. The mirrored `refSurface_` is a
  // hand-written second implementation of the gdi geometry and has produced false
  // positives, so this check - not that one - decides whether the engine diverged.
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
      const size_t off = (static_cast<size_t>(firstY) * gstride) + static_cast<size_t>(firstX) * 4;
      HMRDP_LOGW(
          "gfx replay: gdiAB CULPRIT op=%{public}s rect=(%{public}d,%{public}d)+%{public}dx%{public}d bad=%{public}llu maxDelta=%{public}d px=(%{public}d,%{public}d) engine=b%{public}u g%{public}u r%{public}u gdi=b%{public}u g%{public}u r%{public}u gdiSurf=%{public}dx%{public}d fmt=0x%{public}x",
          rect.op.c_str(), x, y, width, height, static_cast<unsigned long long>(bad), maxDelta,
          firstX, firstY, actual[0], actual[1], actual[2], gdiSurface[off], gdiSurface[off + 1],
          gdiSurface[off + 2], gw, gh, gformat);
      if (!rect.detail.empty()) {
        HMRDP_LOGW("gfx replay: gdiAB CULPRIT tile %{public}s", rect.detail.c_str());
      }
      // Dev: coefficient-level A/B for the failing tile (FreeRDP's own decoder
      // state vs the engine's persistent state buffers, exposed by
      // patch-freerdp.ps1 step 8). `cur` differing points at the pre-DWT path
      // (RLGR / dequant / accumulation); only `bitPos` differing points at the
      // UPGRADE refinement width; nothing differing points at the DWT or the
      // colour conversion.
      GdiAbCompareState(rect.surfaceId, x / 64, y / 64);
      if (rect.payloadSlot >= 0 && !gdiAbPayloads_.empty()) {
        // Offline-verifiable copy of the exact message that diverged.
        const std::vector<uint8_t>& bytes =
            gdiAbPayloads_[static_cast<size_t>(rect.payloadSlot) % gdiAbPayloads_.size()];
        std::ofstream msg(gfxPath_ + ".progmsg", std::ios::out | std::ios::binary | std::ios::trunc);
        if (msg) {
          msg.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        }
        HMRDP_LOGW("gfx replay: gdiAB divergent Progressive message dumped (%{public}zu bytes)",
                   bytes.size());
      }
      // Shape dump: the 64x64 tile around the first failing pixel as a 3-way
      // equality map ('=' all equal, 'g' gdi alone differs, 'm' mirror alone,
      // 'e' engine alone, 'x' all differ) - this separates "gdi wrote where the
      // engine did not" (clip/semantics) from "both wrote, different values"
      // (decode) at a glance.
      const int tx = (firstX / 64) * 64;
      const int ty = (firstY / 64) * 64;
      std::ofstream dump(gfxPath_ + ".gdiadump", std::ios::out | std::ios::trunc);
      if (dump) {
        dump << "# op=" << rect.op << " first=(" << firstX << "," << firstY << ") tile=(" << tx
             << "," << ty << ")\n";
        for (int row = 0; row < 64; ++row) {
          const int py = ty + row;
          if (py < 0 || py >= gh) {
            continue;
          }
          dump << 'r' << py << ' ';
          for (int col = 0; col < 64; ++col) {
            const int px = tx + col;
            if (px < 0 || px >= gw) {
              dump << ' ';
              continue;
            }
            std::vector<uint8_t> e;
            const bool haveE =
                desktop_->ReadSurfaceRect(rect.surfaceId, px, py, 1, 1, &e) && e.size() >= 4;
            const uint8_t* g = gdiSurface + static_cast<size_t>(py) * gstride +
                               static_cast<size_t>(px) * 4;
            const bool haveR = !refSurface_.empty() && px < refW_ && py < refH_;
            const uint8_t* m =
                haveR ? refSurface_.data() + static_cast<size_t>(py) * refStride_ +
                            static_cast<size_t>(px) * 4
                      : nullptr;
            const bool sameEG = haveE && e[0] == g[0] && e[1] == g[1] && e[2] == g[2];
            const bool sameEM = haveE && m != nullptr && e[0] == m[0] && e[1] == m[1] &&
                                e[2] == m[2];
            const bool sameGM = m != nullptr && g[0] == m[0] && g[1] == m[1] && g[2] == m[2];
            char c = '?';
            if (sameEG && sameEM) {
              c = '=';
            } else if (sameEM) {
              c = 'g';
            } else if (sameGM) {
              c = 'e';
            } else if (sameEG) {
              c = 'm';
            } else {
              c = 'x';
            }
            dump << c;
          }
          dump << '\n';
        }
      }
      HMRDP_LOGW("gfx replay: gdiAB shape dump written (%s.gdiadump)", gfxPath_.c_str());
    }
    // Watch trace: log every covered command with both sides, so "engine moved
    // but gdi did not" (or the reverse) is visible directly.
    if (gdiWatchLogged_ < 64 && gdiWatchX_ >= x && gdiWatchX_ < x + width && gdiWatchY_ >= y &&
        gdiWatchY_ < y + height) {
      const size_t eo = static_cast<size_t>(gdiWatchY_ - y) * width * 4 +
                        static_cast<size_t>(gdiWatchX_ - x) * 4;
      const size_t go =
          static_cast<size_t>(gdiWatchY_) * gstride + static_cast<size_t>(gdiWatchX_) * 4;
      gdiWatchLogged_++;
      HMRDP_LOGW(
          "gfx replay: gdiAB watch(%{public}d,%{public}d) op=%{public}s rect=(%{public}d,%{public}d)+%{public}dx%{public}d engine=b%{public}u g%{public}u r%{public}u gdi=b%{public}u g%{public}u r%{public}u",
          gdiWatchX_, gdiWatchY_, rect.op.c_str(), x, y, width, height, actual[eo], actual[eo + 1],
          actual[eo + 2], gdiSurface[go], gdiSurface[go + 1], gdiSurface[go + 2]);
    }
  }
}

void GfxReplay::GdiAbCompareState(uint16_t surfaceId, int xIdx, int yIdx) {
  // FreeRDP side: the mirror's PROGRESSIVE_CONTEXT (same command stream position
  // as the engine here, because the mirror is driven per command).
  if (HmrdpProgressiveTileState == nullptr || refProg_ == nullptr || desktop_ == nullptr ||
      xIdx < 0 || yIdx < 0) {
    return;
  }
  int engW = 0;
  int engH = 0;
  int engStride = 0;
  if (!desktop_->GetSurfaceInfo(surfaceId, &engW, &engH, &engStride) || engW <= 0) {
    return;
  }
  const int engGridW = (engW + 63) / 64;
  const int engGridH = (engH + 63) / 64;
  for (int c = 0; c < 3; ++c) {
    HmrdpProgressiveTileStateData st{};
    if (HmrdpProgressiveTileState(refProg_, surfaceId, static_cast<uint16_t>(xIdx),
                                  static_cast<uint16_t>(yIdx), static_cast<uint16_t>(c),
                                  &st) != 1) {
      HMRDP_LOGW("gfx replay: gdiAB state c=%{public}d unavailable on the FreeRDP side", c);
      continue;
    }
    const uint32_t gridMismatch =
        (st.gridWidth != static_cast<uint32_t>(engGridW) ||
         st.gridHeight != static_cast<uint32_t>(engGridH))
            ? 1u
            : 0u;
    std::vector<int16_t> ecur;
    std::vector<int16_t> esign;
    std::vector<uint8_t> ebp;
    if (!desktop_->ReadRfxTileState(surfaceId,
                                    static_cast<uint32_t>(yIdx) * static_cast<uint32_t>(engGridW) +
                                        static_cast<uint32_t>(xIdx),
                                    c, &ecur, &esign, &ebp)) {
      HMRDP_LOGW("gfx replay: gdiAB state c=%{public}d unavailable on the engine side", c);
      continue;
    }
    uint32_t curDiff = 0;
    uint32_t signDiff = 0;
    uint32_t bpDiff = 0;
    int firstIdx = -1;
    int maxAbs = 0;
    for (size_t i = 0; i < ecur.size() && i < 4096u; ++i) {
      const int e = ecur[i];
      const int f = st.current == nullptr ? 0 : st.current[i];
      if (e != f) {
        if (firstIdx < 0) {
          firstIdx = static_cast<int>(i);
        }
        const int d = e > f ? e - f : f - e;
        if (d > maxAbs) {
          maxAbs = d;
        }
        curDiff++;
      }
      const int es = i < esign.size() ? esign[i] : 0;
      const int fs = st.sign == nullptr ? 0 : st.sign[i];
      if (es != fs) {
        signDiff++;
      }
    }
    for (size_t i = 0; i < ebp.size() && i < 10u; ++i) {
      if (ebp[i] != st.bitPos[i]) {
        bpDiff++;
      }
    }
    char engBp[64];
    char fdpBp[64];
    std::snprintf(engBp, sizeof(engBp), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", ebp.size() > 0 ? ebp[0] : 0u,
                  ebp.size() > 1 ? ebp[1] : 0u, ebp.size() > 2 ? ebp[2] : 0u,
                  ebp.size() > 3 ? ebp[3] : 0u, ebp.size() > 4 ? ebp[4] : 0u,
                  ebp.size() > 5 ? ebp[5] : 0u, ebp.size() > 6 ? ebp[6] : 0u,
                  ebp.size() > 7 ? ebp[7] : 0u, ebp.size() > 8 ? ebp[8] : 0u,
                  ebp.size() > 9 ? ebp[9] : 0u);
    std::snprintf(fdpBp, sizeof(fdpBp), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", st.bitPos[0], st.bitPos[1],
                  st.bitPos[2], st.bitPos[3], st.bitPos[4], st.bitPos[5], st.bitPos[6],
                  st.bitPos[7], st.bitPos[8], st.bitPos[9]);
    HMRDP_LOGW(
        "gfx replay: gdiAB state tile(%{public}d,%{public}d) c=%{public}d curDiff=%{public}u/4096 firstIdx=%{public}d engine=%{public}d fdp=%{public}d maxAbs=%{public}d signDiff=%{public}u/4096 bpDiff=%{public}u/10 engBp=[%{public}s] fdpBp=[%{public}s] fdpGrid=(%{public}u,%{public}u) engGrid=(%{public}d,%{public}d) gridMismatch=%{public}u",
        xIdx, yIdx, c, static_cast<unsigned>(curDiff), firstIdx,
        firstIdx >= 0 ? static_cast<int>(ecur[static_cast<size_t>(firstIdx)]) : 0,
        (firstIdx >= 0 && st.current != nullptr) ? static_cast<int>(st.current[firstIdx]) : 0,
        maxAbs, static_cast<unsigned>(signDiff), static_cast<unsigned>(bpDiff), engBp, fdpBp,
        st.gridWidth, st.gridHeight, engGridW, engGridH, static_cast<unsigned>(gridMismatch));
  }
  // Dev: with every pre-DWT state buffer identical, the divergence can only be
  // in the IDWT or the colour conversion. Dump both sides' inputs/results once so
  // the two stages can be bisected offline (see VULKAN_DEBUG-TODO.md §0):
  //   engine `cur` (3x4096 int16) | FreeRDP tile->data (64x64 BGRA) |
  //   engine surface tile (64x64 BGRA) | fdpBp/engBp (3x10) | header.
  if (!tileDumpDone_) {
    std::ofstream dump(gfxPath_ + ".tiledump", std::ios::out | std::ios::binary | std::ios::trunc);
    if (dump) {
      const int32_t header[6] = {static_cast<int32_t>(surfaceId), xIdx, yIdx, engGridW, engGridH, 0};
      dump.write(reinterpret_cast<const char*>(header), sizeof(header));
      bool haveEngineData = true;
      for (int c = 0; c < 3; ++c) {
        std::vector<int16_t> ecur;
        std::vector<int16_t> esign;
        std::vector<uint8_t> ebp;
        if (!desktop_->ReadRfxTileState(
                surfaceId, static_cast<uint32_t>(yIdx) * static_cast<uint32_t>(engGridW) +
                               static_cast<uint32_t>(xIdx),
                c, &ecur, &esign, &ebp) ||
            ecur.size() != 4096) {
          haveEngineData = false;
          break;
        }
        dump.write(reinterpret_cast<const char*>(ecur.data()), 4096 * 2);
      }
      HmrdpProgressiveTileStateData st{};
      const int16_t* fdpCurrent[3] = {nullptr, nullptr, nullptr};
      const uint8_t* fdpTile = nullptr;
      for (int c = 0; c < 3 && haveEngineData; ++c) {
        HmrdpProgressiveTileStateData one{};
        if (HmrdpProgressiveTileState(refProg_, surfaceId, static_cast<uint16_t>(xIdx),
                                      static_cast<uint16_t>(yIdx), static_cast<uint16_t>(c),
                                      &one) != 1) {
          haveEngineData = false;
          break;
        }
        fdpCurrent[c] = one.current;
        fdpTile = one.data;
        if (c == 0) {
          st = one;
        }
        dump.write(reinterpret_cast<const char*>(one.bitPos), 10);
      }
      if (haveEngineData && fdpTile != nullptr && fdpCurrent[0] != nullptr) {
        dump.write(reinterpret_cast<const char*>(fdpTile), 64 * 64 * 4);
        std::vector<uint8_t> engTile;
        if (desktop_->ReadSurfaceRect(surfaceId, xIdx * 64, yIdx * 64, 64, 64, &engTile) &&
            engTile.size() >= 64u * 64u * 4u) {
          dump.write(reinterpret_cast<const char*>(engTile.data()), 64 * 64 * 4);
        }
        // gdi's own surface tile: the authoritative reference (gdi's update_tiles
        // re-composites the whole frame's tile list on every message, the engine
        // only composites the message's own tiles - see VULKAN_DEBUG-TODO §0).
        int gw = 0;
        int gh = 0;
        int gstride = 0;
        uint32_t gformat = 0;
        const uint8_t* gdiSurface =
            cpuDesktop_ == nullptr ? nullptr
                                   : cpuDesktop_->SurfaceData(surfaceId, &gw, &gh, &gstride, &gformat);
        if (gdiSurface != nullptr && gstride > 0 && xIdx * 64 + 64 <= gw && yIdx * 64 + 64 <= gh) {
          for (int row = 0; row < 64; ++row) {
            dump.write(reinterpret_cast<const char*>(gdiSurface +
                                                     static_cast<size_t>(yIdx * 64 + row) * gstride +
                                                     static_cast<size_t>(xIdx * 64) * 4),
                       64 * 4);
          }
        }
      }
      tileDumpDone_ = true;
      HMRDP_LOGW("gfx replay: gdiAB tile dump written (%s.tiledump)", gfxPath_.c_str());
    }
  }
}

void GfxReplay::RefProgressive(uint16_t surfaceId, const uint8_t* payload, size_t size, int left,
                               int top) {
  if (!kCodecAbEnabled || refProg_ == nullptr || payload == nullptr || size == 0 ||
      refSurface_.empty() || surfaceId != refSurfaceId_) {
    return;
  }
  // Dev: log every message that touches the top-left tile (the one the pre-check
  // keeps flagging) together with its tile-kind breakdown, so the message that
  // wrote it can be correlated with FreeRDP dropping it.
  if (refWatchTile_) {
    uint32_t plain = 0;
    uint32_t diff = 0;
    uint32_t up = 0;
    bool touches = false;
    ParseRfxProgressive(
        payload, size,
        [&](const RfxTileRef& t) {
          if (t.xIdx == 0 && t.yIdx == 0) {
            touches = true;
          }
          if (t.type == RfxTileType::kUpgrade) {
            up++;
          } else if ((t.flags & 1u) != 0u) {
            diff++;
          } else {
            plain++;
          }
        },
        nullptr);
    if (touches) {
      HMRDP_LOGW(
          "gfx replay: refAB msg touches tile(0,0) at cmd#%{public}llu: plain=%{public}u diff=%{public}u upgrade=%{public}u",
          static_cast<unsigned long long>(refCommands_.load()), plain, diff, up);
    }
  }
  // Pre-check: is the engine already different from the reference over the tiles
  // this message touches, *before* the reference decodes it? A "yes" means the
  // divergence is older and some earlier command slipped through the per-command
  // checks; a "no" means this message's own decode is the culprit.
  // NOTE: the pre-check compares the engine's *post*-command surface against the
  // reference's *pre*-command one (the engine already applied the command), so it
  // flags this message's own tiles by construction - it is NOT evidence of a
  // divergence. Disabled; the post-command check is the meaningful one.
  if (false && refPreChecks_ < 200000) {
    ParseRfxProgressive(
        payload, size,
        [&](const RfxTileRef& t) {
          const int tx = left + static_cast<int>(t.xIdx) * 64;
          const int ty = top + static_cast<int>(t.yIdx) * 64;
          if (tx < 0 || ty < 0 || tx + 64 > refW_ || ty + 64 > refH_) {
            return;
          }
          refPreChecks_++;
          RefVerifyRect(surfaceId, tx, ty, 64, 64, "progressive-pre");
        },
        nullptr);
    if (refBadOp_ == "progressive-pre") {
      // Report once and stop so the log stays useful.
      refPreChecks_ = 200001;
    }
  }
  REGION16 invalid;
  region16_init(&invalid);
  const INT32 refRc = progressive_decompress(
      static_cast<PROGRESSIVE_CONTEXT*>(refProg_), payload, static_cast<UINT32>(size),
      refSurface_.data(), PIXEL_FORMAT_BGRA32, static_cast<UINT32>(refStride_),
      static_cast<UINT32>(left), static_cast<UINT32>(top), &invalid, surfaceId,
      mirrorFrameId_.load());
  if (refRc < 0 && refRcLogged_ < 8) {
    refRcLogged_++;
    HMRDP_LOGW(
        "gfx replay: refAB reference progressive_decompress rc=%{public}d at cmd#%{public}llu (FreeRDP drops the whole message; the engine decodes it)",
        static_cast<int>(refRc), static_cast<unsigned long long>(refCommands_.load()));
  }
  // The divergent-message dump needs the same payload slot for every rect this
  // message records; stash it once, up front (the chunk buffer is reused by the
  // next chunk, so a pointer would be stale by the time the deferred flush runs).
  const int messageSlot = GdiAbStashPayload(payload, static_cast<uint32_t>(size));
  {
    UINT32 nbRects = 0;
    const RECTANGLE_16* rects = region16_rects(&invalid, &nbRects);
    // Per-command absolute check on exactly the rects FreeRDP composited. Any
    // earlier divergence would already have been caught by its own command, so a
    // failure here names this message as the culprit.
    for (UINT32 i = 0; i < nbRects; ++i) {
      const int rx = rects[i].left;
      const int ry = rects[i].top;
      const int rw = static_cast<int>(rects[i].right) - rx;
      const int rh = static_cast<int>(rects[i].bottom) - ry;
      RefMarkProv(&refProv_, refStride_, refW_, refH_, rx, ry, rw, rh, 1);
      GdiAbCheck(surfaceId, rx, ry, rw, rh, "progressive", std::string(), messageSlot);
      RefVerifyRect(surfaceId, rx, ry, rw, rh, "progressive");
      RefWatchRect(surfaceId, rx, ry, rw, rh, "progressive");
    }
  }
  if (refBadThisMessage_ && refTileLogged_ < 12) {
    // Name the tile that owns the first diverging pixel and the decode sub-path it
    // takes: plain kFirst -> RLGR/dequant/DWT, kFirst+RFX_TILE_DIFFERENCE -> the
    // `current` accumulation, kUpgrade -> SRL/raw + persistent bit positions.
    refBadThisMessage_ = false;
    ++refTileLogged_;
    const int bx = refBadX_ - left;
    const int by = refBadY_ - top;
    ParseRfxProgressive(
        payload, size,
        [&](const RfxTileRef& t) {
          const int tx = static_cast<int>(t.xIdx) * 64;
          const int ty = static_cast<int>(t.yIdx) * 64;
          if (bx < tx || bx >= tx + 64 || by < ty || by >= ty + 64) {
            return;
          }
          const char* kind = (t.type == RfxTileType::kUpgrade)
                                 ? "upgrade"
                                 : ((t.flags & 1u) != 0u ? "first+diff" : "first");
          refTileLogged_ = refTileLogged_ > 0 ? refTileLogged_ : 1;
          // Is the diverging pixel inside the tile's region clip rects? If not,
          // the engine wrote a pixel FreeRDP deliberately leaves alone.
          bool insideClip = false;
          for (uint16_t ri = 0; !insideClip && ri < t.numRects; ++ri) {
            const RfxRect& r = t.rects[ri];
            if (bx >= static_cast<int>(r.x) && bx < static_cast<int>(r.x) + static_cast<int>(r.width) &&
                by >= static_cast<int>(r.y) &&
                by < static_cast<int>(r.y) + static_cast<int>(r.height)) {
              insideClip = true;
            }
          }
          HMRDP_LOGW("gfx replay: refAB failing pixel insideClip=%{public}d (0 = engine wrote outside the region rects)",
                     insideClip ? 1 : 0);
          HMRDP_LOGW(
              "gfx replay: refAB tile (%{public}u,%{public}u) kind=%{public}s q=%{public}u quant=(%{public}u,%{public}u,%{public}u) rects=%{public}u yLen=%{public}u cbLen=%{public}u crLen=%{public}u srl=(%{public}u,%{public}u,%{public}u) raw=(%{public}u,%{public}u,%{public}u) pass-pixel=(%{public}d,%{public}d)",
              t.xIdx, t.yIdx, kind, static_cast<unsigned>(t.quality),
              static_cast<unsigned>(t.quantIdxY), static_cast<unsigned>(t.quantIdxCb),
              static_cast<unsigned>(t.quantIdxCr), static_cast<unsigned>(t.numRects),
              static_cast<unsigned>(t.yLen), static_cast<unsigned>(t.cbLen),
              static_cast<unsigned>(t.crLen), static_cast<unsigned>(t.ySrlLen),
              static_cast<unsigned>(t.cbSrlLen), static_cast<unsigned>(t.crSrlLen),
              static_cast<unsigned>(t.yRawLen), static_cast<unsigned>(t.cbRawLen),
              static_cast<unsigned>(t.crRawLen), bx, by);
        },
        nullptr);
  }
  // Correct whole-tile A/B: after BOTH sides applied the message, compare every
  // tile it touched in full. Unlike the per-invalid-rect check this also sees the
  // pixels FreeRDP did not composite, and for the message that first wrote a tile
  // differently it names that writer.
  {
    // How many tiles the message carries and how often each one appears (a tile
    // may be sent twice - e.g. a FIRST followed by an UPGRADE - and FreeRDP
    // composites it once per update_tiles call).
    std::map<uint32_t, uint32_t> tileOcc;
    uint32_t msgTiles = 0;
    ParseRfxProgressive(
        payload, size,
        [&](const RfxTileRef& t) {
          tileOcc[(static_cast<uint32_t>(t.yIdx) << 16) | t.xIdx]++;
          msgTiles++;
        },
        nullptr);
    ParseRfxProgressive(
        payload, size,
        [&](const RfxTileRef& t) {
          const int tx = left + static_cast<int>(t.xIdx) * 64;
          const int ty = top + static_cast<int>(t.yIdx) * 64;
          if (tx < 0 || ty < 0 || tx + 64 > refW_ || ty + 64 > refH_) {
            return;
          }
          uint8_t quantNibbles[10] = {0};
          uint8_t progNibbles[10] = {0};
          char info[512];
          const char* kind = (t.type == RfxTileType::kUpgrade)
                                 ? "upgrade"
                                 : ((t.flags & 1u) != 0u ? "first+diff" : "first");
          // newBit = quant + progQuant; oldBit is the reconstructed persistent
          // state, so numBits = old - new is the refinement width FreeRDP uses.
          const uint32_t tileStream =
              (static_cast<uint32_t>(t.yIdx) * static_cast<uint32_t>(refW_ / 64) + t.xIdx) * 3u;
          std::string newBits;
          std::string oldBits;
          std::string numBits;
          for (int c = 0; c < 3; ++c) {
            const RfxQuant* q =
                &t.quants[c == 0 ? t.quantIdxY : (c == 1 ? t.quantIdxCb : t.quantIdxCr)];
            RfxQuant pq;
            if (t.quality != 0xFF && t.progQuants != nullptr && t.quality < t.numProgQuant) {
              pq = (c == 0) ? t.progQuants[t.quality].y
                            : ((c == 1) ? t.progQuants[t.quality].cb : t.progQuants[t.quality].cr);
            }
            QuantNibbles(*q, quantNibbles);
            QuantNibbles(pq, progNibbles);
            int newBitVals[10] = {0};
            int oldBitVals[10] = {0};
            int numBitVals[10] = {0};
            for (int i = 0; i < 10; ++i) {
              newBitVals[i] = static_cast<int>(quantNibbles[i]) + static_cast<int>(progNibbles[i]);
              const uint64_t key = (static_cast<uint64_t>(surfaceId) << 32) |
                                   (static_cast<uint64_t>(tileStream + static_cast<uint32_t>(c)));
              const auto it = tileBitPos_.find(key);
              oldBitVals[i] = it == tileBitPos_.end() ? -1 : static_cast<int>(it->second[i]);
              numBitVals[i] = (oldBitVals[i] >= 0 && oldBitVals[i] > newBitVals[i])
                                  ? (oldBitVals[i] - newBitVals[i])
                                  : 0;
            }
            newBits += '[' + JoinNibbles(newBitVals) + ']';
            oldBits += '[' + JoinNibbles(oldBitVals) + ']';
            numBits += '[' + JoinNibbles(numBitVals) + ']';
            std::array<uint8_t, 10> state{};
            for (int i = 0; i < 10; ++i) {
              state[i] = static_cast<uint8_t>(newBitVals[i]);
            }
            const uint64_t key = (static_cast<uint64_t>(surfaceId) << 32) |
                                 (static_cast<uint64_t>(tileStream + static_cast<uint32_t>(c)));
            tileBitPos_[key] = state;
          }
          std::snprintf(info, sizeof(info),
                        "(%u,%u) kind=%s flags=%u q=%u qi=(%u,%u,%u) rects=%u yLen=%u cbLen=%u "
                        "crLen=%u srlY=%u rawY=%u occ=%u/%u",
                        static_cast<unsigned>(t.xIdx), static_cast<unsigned>(t.yIdx), kind,
                        static_cast<unsigned>(t.flags), static_cast<unsigned>(t.quality),
                        static_cast<unsigned>(t.quantIdxY), static_cast<unsigned>(t.quantIdxCb),
                        static_cast<unsigned>(t.quantIdxCr), static_cast<unsigned>(t.numRects),
                        static_cast<unsigned>(t.yLen), static_cast<unsigned>(t.cbLen),
                        static_cast<unsigned>(t.crLen), static_cast<unsigned>(t.ySrlLen),
                        static_cast<unsigned>(t.yRawLen),
                        static_cast<unsigned>(tileOcc[(static_cast<uint32_t>(t.yIdx) << 16) | t.xIdx]),
                        static_cast<unsigned>(msgTiles));
          const std::string detail =
              std::string(info) + " newBit=" + newBits + " oldBit=" + oldBits + " numBits=" + numBits;
          GdiAbCheck(surfaceId, tx, ty, 64, 64, "progressive-tiles", detail, messageSlot);
          RefVerifyRect(surfaceId, tx, ty, 64, 64, "progressive-tiles");
        },
        nullptr);
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
  const INT32 clearRc =
      clear_decompress(static_cast<CLEAR_CONTEXT*>(refClear_), payload, static_cast<UINT32>(size),
                       static_cast<UINT32>(width), static_cast<UINT32>(height), refSurface_.data(),
                       PIXEL_FORMAT_BGRA32, static_cast<UINT32>(refStride_),
                       static_cast<UINT32>(x), static_cast<UINT32>(y), static_cast<UINT32>(refW_),
                       static_cast<UINT32>(refH_), nullptr);
  if (clearRc < 0 && refClearRcLogged_ < 8) {
    refClearRcLogged_++;
    HMRDP_LOGW(
        "gfx replay: refAB reference clear_decompress rc=%{public}d rect=(%{public}d,%{public}d)+%{public}dx%{public}d (FreeRDP leaves the band untouched, the engine decodes it)",
        static_cast<int>(clearRc), x, y, width, height);
  }
  RefMarkProv(&refProv_, refStride_, refW_, refH_, x, y, width, height, 2);
  RefVerifyRect(surfaceId, x, y, width, height, "clearcodec");
  RefWatchRect(surfaceId, x, y, width, height, "clearcodec");
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
    // gdi fails the command when the slot is empty.
    if (refCacheMisses_ < 4) {
      refCacheMisses_++;
      HMRDP_LOGW("gfx replay: refAB cacheRestore MISSING slot=%{public}u cacheSize=%{public}zu",
                 slot, refCache_.size());
    }
    return;
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
    RefVerifyRect(surfaceId, px, py, entry.width, entry.height, "cacheRestore");
    RefWatchRect(surfaceId, px, py, entry.width, entry.height, "cacheRestore");
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
    RefVerifyRect(surfaceId, left, top, right - left, bottom - top, "fill");
    RefWatchRect(surfaceId, left, top, right - left, bottom - top, "fill");
  }
  refCommands_++;
}

void GfxReplay::RefUpload(uint16_t surfaceId, uint32_t format, int left, int top, int width,
                          int height, const uint8_t* payload, uint32_t payloadLen) {
  if (!kCodecAbEnabled || payload == nullptr || refSurface_.empty() ||
      surfaceId != refSurfaceId_ || width <= 0 || height <= 0) {
    return;
  }
  const uint32_t bpp = format >> 24;
  const uint64_t need = static_cast<uint64_t>(bpp / 8) * static_cast<uint64_t>(width) *
                        static_cast<uint64_t>(height);
  if ((bpp != 24 && bpp != 32) || need > payloadLen) {
    return;
  }
  int sx = left < 0 ? 0 : left;
  int sy = top < 0 ? 0 : top;
  int ex = left + width;
  int ey = top + height;
  if (ex > refW_) ex = refW_;
  if (ey > refH_) ey = refH_;
  if (ex <= sx || ey <= sy) {
    return;
  }
  for (int row = sy; row < ey; ++row) {
    const int srcRow = row - top;
    const uint8_t* src =
        payload + (static_cast<size_t>(srcRow) * static_cast<size_t>(width) +
                   static_cast<size_t>(sx - left)) * (bpp / 8);
    uint8_t* dst = refSurface_.data() + static_cast<size_t>(row) * refStride_ +
                   static_cast<size_t>(sx) * 4;
    if (bpp == 32) {
      std::memcpy(dst, src, static_cast<size_t>(ex - sx) * 4);
    } else {
      for (int col = 0; col < ex - sx; ++col) {
        dst[col * 4 + 0] = src[col * 3 + 0];
        dst[col * 4 + 1] = src[col * 3 + 1];
        dst[col * 4 + 2] = src[col * 3 + 2];
        dst[col * 4 + 3] = 0xFF;
      }
    }
  }
  RefMarkProv(&refProv_, refStride_, refW_, refH_, sx, sy, ex - sx, ey - sy, 6);
  RefVerifyRect(surfaceId, sx, sy, ex - sx, ey - sy, "upload");
  RefWatchRect(surfaceId, sx, sy, ex - sx, ey - sy, "upload");
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
    RefVerifyRect(dstSurfaceId, px, py, w, h, "surfaceToSurface");
    RefWatchRect(dstSurfaceId, px, py, w, h, "surfaceToSurface");
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
