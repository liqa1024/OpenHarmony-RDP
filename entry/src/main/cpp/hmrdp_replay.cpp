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
      // Dev: per-message Progressive A/B against FreeRDP's own decoder. Runs
      // after the engine applied the command, so the engine surface is current.
      if (cmdId == kGpuCmdCreateSurface && scalars != nullptr) {
        owner_->ProgAbCreateSurface(static_cast<uint16_t>(surfaceId),
                                    static_cast<int>(scalars[0]), static_cast<int>(scalars[1]));
      } else if (cmdId == kGpuCmdDeleteSurface) {
        owner_->ProgAbDeleteSurface(static_cast<uint16_t>(surfaceId));
      } else if (cmdId == kGpuCmdWireToSurface && codecId == kGpuCodecCaprogressive &&
                 payload != nullptr && params != nullptr && paramsLen >= 32) {
        owner_->ProgAbMessage(static_cast<uint16_t>(surfaceId), payload, payloadLen,
                              static_cast<int>(RdU32(params + 8)),
                              static_cast<int>(RdU32(params + 12)));
      } else if (cmdId == kGpuCmdResetGraphics) {
        owner_->ClearAbResetGraphics();
      } else if (cmdId == kGpuCmdWireToSurface && codecId == kGpuCodecClearCodec &&
                 payload != nullptr && params != nullptr && paramsLen >= 32) {
        owner_->ClearAbBand(static_cast<uint16_t>(surfaceId), payload, payloadLen,
                            static_cast<int>(RdU32(params + 8)),
                            static_cast<int>(RdU32(params + 12)),
                            static_cast<int>(RdU32(params + 24)),
                            static_cast<int>(RdU32(params + 28)));
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
    progAbMessages_ = 0;
    progAbBadMessages_ = 0;
    progAbBadTiles_ = 0;
    progAbBadPx_ = 0;
    progAbFirstLogged_ = false;
    progAbSurface_.clear();
    progAbSurfaceId_ = 0xFFFFu;
    progAbW_ = 0;
    progAbH_ = 0;
    progAbStride_ = 0;
    clearAbBands_.store(0);
    clearAbBadBands_.store(0);
    clearAbBadPx_.store(0);
    clearAbFirstLogged_ = false;
    clearAbSurface_.clear();
    clearAbSurfaceId_ = 0xFFFFu;
    clearAbW_ = 0;
    clearAbH_ = 0;
    clearAbStride_ = 0;
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
  const std::string ab = ProgAbSummary();
  if (!ab.empty()) {
    out += "\n";
    out += ab;
  }
  const std::string cab = ClearAbSummary();
  if (!cab.empty()) {
    out += "\n";
    out += cab;
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
    // Dev: reference Progressive decoder for the per-message A/B.
    progAb_ = progressive_context_new(FALSE);
    if (progAb_ == nullptr) {
      HMRDP_LOGW("gfx replay: progAB reference context unavailable");
    }
    clearAb_ = clear_context_new(FALSE);
    if (clearAb_ == nullptr) {
      HMRDP_LOGW("gfx replay: clearAB reference context unavailable");
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
  if (progAb_ != nullptr) {
    // Dev: per-message Progressive A/B result (empty when no message diverged).
    const std::string ab = ProgAbSummary();
    if (!ab.empty()) {
      HMRDP_LOGI("gfx replay: %{public}s", ab.c_str());
    }
    progressive_context_free(static_cast<PROGRESSIVE_CONTEXT*>(progAb_));
    progAb_ = nullptr;
  }
  if (clearAb_ != nullptr) {
    const std::string ab = ClearAbSummary();
    if (!ab.empty()) {
      HMRDP_LOGI("gfx replay: %{public}s", ab.c_str());
    }
    clear_context_free(static_cast<CLEAR_CONTEXT*>(clearAb_));
    clearAb_ = nullptr;
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

// ---------------------------------------------------------------------------
// Dev: Progressive per-message A/B (VULKAN-TODO §8)
//
// The reference decoder is a stock FreeRDP PROGRESSIVE_CONTEXT fed the very same
// payloads. Its per-tile output depends only on the progressive stream (the
// per-(tile,component) `current`/`sign`/bit-position state), never on the
// surface pixels, so any per-tile mismatch is an engine-side decode divergence -
// and the log names the message, the tile and the first differing pixel.
//
// `-O0`-friendly and replay-thread only: no locking, no cross-thread access.
// ---------------------------------------------------------------------------

std::string GfxReplay::ProgAbSummary() const {
  if (progAbMessages_.load() == 0) {
    return std::string();
  }
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "progAB msgs=%llu badMsgs=%llu badTiles=%llu badPx=%llu",
                static_cast<unsigned long long>(progAbMessages_.load()),
                static_cast<unsigned long long>(progAbBadMessages_.load()),
                static_cast<unsigned long long>(progAbBadTiles_.load()),
                static_cast<unsigned long long>(progAbBadPx_.load()));
  return std::string(buf);
}

void GfxReplay::ProgAbCreateSurface(uint16_t surfaceId, int width, int height) {
  if (progAb_ == nullptr || width <= 0 || height <= 0) {
    return;
  }
  // gdi: gdi_CreateSurface -> progressive_create_surface_context with the
  // surface's (16B-aligned) size.
  PROGRESSIVE_CONTEXT* ctx = static_cast<PROGRESSIVE_CONTEXT*>(progAb_);
  progressive_delete_surface_context(ctx, surfaceId);
  const int w = (width + 15) & ~15;
  const int h = (height + 15) & ~15;
  const INT32 rc = progressive_create_surface_context(ctx, surfaceId, static_cast<UINT32>(w),
                                                      static_cast<UINT32>(h));
  if (rc < 0) {
    HMRDP_LOGW("gfx replay: progAB create surface %{public}u failed rc=%{public}d", surfaceId,
               static_cast<int>(rc));
  }
  if (surfaceId == progAbSurfaceId_) {
    progAbSurface_.clear();
    progAbW_ = 0;
    progAbH_ = 0;
    progAbStride_ = 0;
  }
}

void GfxReplay::ProgAbDeleteSurface(uint16_t surfaceId) {
  if (progAb_ == nullptr) {
    return;
  }
  // gdi: gdi_DeleteSurface -> progressive_delete_surface_context.
  progressive_delete_surface_context(static_cast<PROGRESSIVE_CONTEXT*>(progAb_), surfaceId);
  if (surfaceId == progAbSurfaceId_) {
    progAbSurface_.clear();
    progAbW_ = 0;
    progAbH_ = 0;
    progAbStride_ = 0;
  }
}

void GfxReplay::ProgAbMessage(uint16_t surfaceId, const uint8_t* payload, size_t size, int left,
                              int top) {
  if (!kCodecAbEnabled || progAb_ == nullptr || desktop_ == nullptr || payload == nullptr ||
      size == 0) {
    return;
  }
  int w = 0;
  int h = 0;
  int stride = 0;
  if (!desktop_->GetSurfaceInfo(surfaceId, &w, &h, &stride) || w <= 0 || h <= 0 || stride <= 0) {
    return;
  }
  if (surfaceId != progAbSurfaceId_ || progAbW_ != w || progAbH_ != h || progAbStride_ != stride) {
    progAbSurfaceId_ = surfaceId;
    progAbW_ = w;
    progAbH_ = h;
    progAbStride_ = stride;
    progAbSurface_.assign(static_cast<size_t>(stride) * static_cast<size_t>(h), 0xFF);
  }

  PROGRESSIVE_CONTEXT* ctx = static_cast<PROGRESSIVE_CONTEXT*>(progAb_);
  REGION16 invalid;
  region16_init(&invalid);  // zeroes the struct
  // gdi passes the *frame* id, not the context id; it only groups tiles for the
  // output update, so the frame counter reproduces the reference behaviour.
  const INT32 rc = progressive_decompress(
      ctx, payload, static_cast<UINT32>(size), progAbSurface_.data(), PIXEL_FORMAT_BGRA32,
      static_cast<UINT32>(progAbStride_), static_cast<UINT32>(left), static_cast<UINT32>(top),
      &invalid, surfaceId, static_cast<UINT32>(frames_.load()));
  region16_uninit(&invalid);
  progAbMessages_++;
  if (rc < 0) {
    HMRDP_LOGW("gfx replay: progAB reference decode failed msg=%{public}llu rc=%{public}d",
               static_cast<unsigned long long>(progAbMessages_), static_cast<int>(rc));
    return;
  }

  std::vector<uint8_t> engine;
  if (!desktop_->ReadSurface(surfaceId, &engine) ||
      engine.size() < progAbSurface_.size()) {
    return;
  }

  uint64_t badTiles = 0;
  uint64_t badPx = 0;
  ParseRfxProgressive(
      payload, size,
      [&](const RfxTileRef& t) {
        const int ox = left + static_cast<int>(t.xIdx) * 64;
        const int oy = top + static_cast<int>(t.yIdx) * 64;
        bool tileBad = false;
        for (int row = 0; row < 64; ++row) {
          const int py = oy + row;
          if (py < 0 || py >= progAbH_) {
            continue;
          }
          for (int col = 0; col < 64; ++col) {
            const int px = ox + col;
            if (px < 0 || px >= progAbW_) {
              continue;
            }
            // FreeRDP composites a tile only inside the region's clip rects; the
            // rest keeps the previous surface content in both decoders.
            bool inside = (t.numRects == 0);
            for (uint16_t ri = 0; !inside && ri < t.numRects; ++ri) {
              const RfxRect& r = t.rects[ri];
              const int rx = left + static_cast<int>(r.x);
              const int ry = top + static_cast<int>(r.y);
              if (px >= rx && px < rx + static_cast<int>(r.width) && py >= ry &&
                  py < ry + static_cast<int>(r.height)) {
                inside = true;
              }
            }
            if (!inside) {
              continue;
            }
            const size_t off =
                static_cast<size_t>(py) * progAbStride_ + static_cast<size_t>(px) * 4;
            const uint8_t* a = engine.data() + off;
            const uint8_t* b = progAbSurface_.data() + off;
            if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) {
              continue;
            }
            badPx++;
            tileBad = true;
            if (!progAbFirstLogged_) {
              progAbFirstLogged_ = true;
              HMRDP_LOGW(
                  "gfx replay: progAB FIRST mismatch msg=%{public}llu tile=(%{public}u,%{public}u) "
                  "px=(%{public}d,%{public}d) engine=b%{public}u g%{public}u r%{public}u ref=b%{public}u g%{public}u r%{public}u "
                  "flags=%{public}u q=%{public}u",
                  static_cast<unsigned long long>(progAbMessages_), t.xIdx, t.yIdx, px, py, a[0],
                  a[1], a[2], b[0], b[1], b[2], static_cast<unsigned>(t.flags),
                  static_cast<unsigned>(t.quality));
            }
          }
        }
        if (tileBad) {
          badTiles++;
        }
      },
      nullptr);

  if (badPx != 0) {
    progAbBadMessages_++;
    progAbBadTiles_ += badTiles;
    progAbBadPx_ += badPx;
    HMRDP_LOGW(
        "gfx replay: progAB msg=%{public}llu bad tiles=%{public}llu px=%{public}llu (totals: msgs=%{public}llu badMsgs=%{public}llu tiles=%{public}llu px=%{public}llu)",
        static_cast<unsigned long long>(progAbMessages_), static_cast<unsigned long long>(badTiles),
        static_cast<unsigned long long>(badPx),
        static_cast<unsigned long long>(progAbMessages_),
        static_cast<unsigned long long>(progAbBadMessages_),
        static_cast<unsigned long long>(progAbBadTiles_),
        static_cast<unsigned long long>(progAbBadPx_));
  }
}

std::string GfxReplay::ClearAbSummary() const {
  if (clearAbBands_.load() == 0) {
    return std::string();
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), "clearAB bands=%llu badBands=%llu badPx=%llu",
                static_cast<unsigned long long>(clearAbBands_.load()),
                static_cast<unsigned long long>(clearAbBadBands_.load()),
                static_cast<unsigned long long>(clearAbBadPx_.load()));
  return std::string(buf);
}

void GfxReplay::ClearAbResetGraphics() {
  if (clearAb_ != nullptr) {
    clear_context_reset(static_cast<CLEAR_CONTEXT*>(clearAb_));
  }
}

void GfxReplay::ClearAbBand(uint16_t surfaceId, const uint8_t* payload, size_t size, int left,
                            int top, int width, int height) {
  if (!kCodecAbEnabled || clearAb_ == nullptr || desktop_ == nullptr || payload == nullptr ||
      size == 0 || width <= 0 || height <= 0) {
    return;
  }
  int w = 0;
  int h = 0;
  int stride = 0;
  if (!desktop_->GetSurfaceInfo(surfaceId, &w, &h, &stride) || w <= 0 || h <= 0 || stride <= 0) {
    return;
  }
  const int x = left < 0 ? 0 : left;
  const int y = top < 0 ? 0 : top;
  if (x + width > w || y + height > h) {
    return;  // the engine rejects these too
  }
  if (surfaceId != clearAbSurfaceId_ || clearAbW_ != w || clearAbH_ != h ||
      clearAbStride_ != stride) {
    clearAbSurfaceId_ = surfaceId;
    clearAbW_ = w;
    clearAbH_ = h;
    clearAbStride_ = stride;
    clearAbSurface_.assign(static_cast<size_t>(stride) * static_cast<size_t>(h), 0xFF);
  }
  std::vector<uint8_t> engine;
  if (!desktop_->ReadSurface(surfaceId, &engine) || engine.size() < clearAbSurface_.size()) {
    return;
  }
  // Align the reference input with the engine's surface for exactly the band (the
  // only pixels clear_decompress can touch).
  for (int row = 0; row < height; ++row) {
    const size_t off = (static_cast<size_t>(y + row) * stride) + static_cast<size_t>(x) * 4;
    std::memcpy(clearAbSurface_.data() + off, engine.data() + off,
                static_cast<size_t>(width) * 4);
  }
  CLEAR_CONTEXT* ctx = static_cast<CLEAR_CONTEXT*>(clearAb_);
  const INT32 rc = clear_decompress(ctx, payload, static_cast<UINT32>(size),
                                    static_cast<UINT32>(width), static_cast<UINT32>(height),
                                    clearAbSurface_.data(), PIXEL_FORMAT_BGRA32,
                                    static_cast<UINT32>(stride), static_cast<UINT32>(x),
                                    static_cast<UINT32>(y), static_cast<UINT32>(w),
                                    static_cast<UINT32>(h), nullptr);
  clearAbBands_++;
  if (rc < 0) {
    HMRDP_LOGW("gfx replay: clearAB reference decode failed band=%{public}llu rc=%{public}d",
               static_cast<unsigned long long>(clearAbBands_.load()), static_cast<int>(rc));
    return;
  }
  uint64_t bad = 0;
  for (int row = 0; row < height; ++row) {
    const size_t off = (static_cast<size_t>(y + row) * stride) + static_cast<size_t>(x) * 4;
    const uint8_t* a = engine.data() + off;
    const uint8_t* b = clearAbSurface_.data() + off;
    for (int col = 0; col < width; ++col) {
      if (a[col * 4 + 0] == b[col * 4 + 0] && a[col * 4 + 1] == b[col * 4 + 1] &&
          a[col * 4 + 2] == b[col * 4 + 2]) {
        continue;
      }
      bad++;
      if (!clearAbFirstLogged_) {
        clearAbFirstLogged_ = true;
        HMRDP_LOGW(
            "gfx replay: clearAB FIRST mismatch band=%{public}llu rect=(%{public}d,%{public}d)-(%{public}d,%{public}d) px=(%{public}d,%{public}d) engine=b%{public}u g%{public}u r%{public}u ref=b%{public}u g%{public}u r%{public}u",
            static_cast<unsigned long long>(clearAbBands_.load()), x, y, x + width, y + height,
            x + col, y + row, a[col * 4 + 0], a[col * 4 + 1], a[col * 4 + 2], b[col * 4 + 0],
            b[col * 4 + 1], b[col * 4 + 2]);
      }
    }
  }
  if (bad != 0) {
    clearAbBadBands_++;
    clearAbBadPx_ += bad;
    HMRDP_LOGW(
        "gfx replay: clearAB band=%{public}llu bad px=%{public}llu rect=(%{public}d,%{public}d)+%{public}dx%{public}d (totals: bands=%{public}llu badBands=%{public}llu px=%{public}llu)",
        static_cast<unsigned long long>(clearAbBands_.load()), static_cast<unsigned long long>(bad),
        x, y, width, height, static_cast<unsigned long long>(clearAbBands_.load()),
        static_cast<unsigned long long>(clearAbBadBands_.load()),
        static_cast<unsigned long long>(clearAbBadPx_.load()));
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
