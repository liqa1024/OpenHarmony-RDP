/*
 * HmRdp - dev-only recorded-RDP replay (doc_agent/gfx-engine.md §6).
 *
 * Present-on-screen consumer of the shared replay driver: the capture is read,
 * decompressed and parsed by hmrdp_gfx_driver.cpp (FreeRDP's own ZGX + RDPGFX
 * parsing), the resulting commands go to the Vulkan desktop engine, and this
 * file only presents the composed screen on every EndFrame through
 * GpuVkPresentComposed(); the CPU (gdi) route decodes with FreeRDP's own pipeline
 * and uploads the composed desktop through the presenter instead. Correctness is
 * checked against a stored golden reference (GfxReplayRefMode).
 */
#include "hmrdp_replay.h"

#include <native_window/external_window.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_decode_tuning.h"
#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_rfx.h"  // kGpuCmd / kGpuCodec ids
#include "hmrdp_vk_desktop.h"
#include "hmrdp_vk_renderer.h"
#include "hmrdp_presenter.h"

// DEV-ONLY: phase timers exported by the patched FreeRDP progressive decoder
// (libfreerdp/codec/progressive.c). Weak, so a stock FreeRDP just reports none.
// Slots (see the patch's comment): [0] tile read/parse, [1] pool dispatch,
// [2] pool section, [3] update_tiles, [4] unions, [5] messages, [6] tiles
// composited, [7] the blocking part of [2], [8] tiles decoded, [9] tile-decode
// time (this thread's loop on the serial path, the workers' summed decode time
// on the pool path - see the reporting code).
constexpr int kProgStatSlots = 24;
extern "C" unsigned long long HmrdpProgStat[kProgStatSlots] __attribute__((weak));

// DEV-ONLY: what the patched libwinpr did with the decode worker count -
// out = { requested, workers the pool really has, resizes }. Weak, so a stock
// FreeRDP just reports nothing.
extern "C" void HmrdpGetDecodeThreadsStats(unsigned int out[3]) __attribute__((weak));

// DEV-ONLY: the patched FreeRDP inverse DWT (libfreerdp/codec/rfx_dwt.c,
// codec/progressive.c, codec/neon/rfx_neon.c) is compared against the upstream
// scalar reference on a sample of tiles. `bad=0` cannot cover a decode-side
// change, and the SIMD variants are only allowed in while the difference they
// make is measured: the worst |delta| says whether it is rounding or something
// structural. out = { tiles checked, elements that differed, worst |delta| }
// (doc_agent/gfx-engine.md §8.4).
constexpr int kDwtCheckSlots = 3;
extern "C" unsigned long long HmrdpDwtCheckStat[kDwtCheckSlots] __attribute__((weak));
extern "C" void HmrdpSetDwtCheck(int on) __attribute__((weak));

namespace hmrdp {

// The replay's desktop engine: the Vulkan GFX engine plus its swapchain
// presenter. It exposes exactly what the pump/stats code needs, so that code
// stays free of the engine's own types.
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
  // Prepare() creates the swapchain and presents a black frame. The engine and the
  // CPU upload both hand the presenter FreeRDP-order BGRA; the presenter's quad does
  // the channel-order conversion, so the engine no longer follows the swapchain
  // format (doc_agent/gfx-engine.md §2.3).
  if (!renderer_->Prepare()) {
    if (error != nullptr) {
      *error = "vulkan surface/swapchain failed: " + renderer_->lastError();
    }
    renderer_.reset();
    return false;
  }
  engine_ = std::make_unique<GfxVkDesktop>();
  if (!engine_->Init()) {
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

constexpr int kLogEvery = 120;
constexpr int64_t kMaxRunUs = 120ll * 1000000ll;  // safety cap, fast mode
// In realtime mode the run lasts as long as the recording did (plus whatever the
// client fell behind), so the cap has to be much higher to avoid cutting a long
// capture short.
constexpr int64_t kMaxRealtimeRunUs = 900ll * 1000000ll;
constexpr int kStartWaitUs = 3000000;


// --- golden reference (GfxReplayRefMode) -----------------------------------
// Fingerprint of a capture file: its size plus the first and last 256KB. Names the
// reference files (`hmrdp_ref_<tag>.hash/.bmp` next to the capture), so a
// reference recorded from a *different* recording can never be picked up silently
// - the device file name is always hmrdp_gfx.bin, and doc_agent/gfx-engine.md §6
// requires every capture to be verified on its own.
std::string CaptureTag(const std::string& capturePath) {
  FILE* f = std::fopen(capturePath.c_str(), "rb");
  if (f == nullptr) {
    return std::string("nofile");
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  uint64_t h = 1469598103934665603ull;
  h = (h ^ static_cast<uint64_t>(size > 0 ? size : 0)) * 1099511628211ull;
  const size_t chunk = 256u * 1024u;
  std::vector<uint8_t> buf(chunk);
  const long last = size > static_cast<long>(chunk) ? size - static_cast<long>(chunk) : 0;
  const long offsets[2] = {0, last};
  for (int i = 0; i < 2; ++i) {
    std::fseek(f, offsets[i], SEEK_SET);
    const size_t got = std::fread(buf.data(), 1, chunk, f);
    for (size_t b = 0; b < got; ++b) {
      h = (h ^ buf[b]) * 1099511628211ull;
    }
  }
  std::fclose(f);
  char out[24];
  std::snprintf(out, sizeof(out), "%08llx", static_cast<unsigned long long>(h & 0xffffffffull));
  return std::string(out);
}

// Sibling of the capture.
std::string RefSibling(const std::string& capturePath, const std::string& name) {
  const size_t slash = capturePath.find_last_of("/\\");
  const std::string dir =
      slash == std::string::npos ? std::string() : capturePath.substr(0, slash + 1);
  return dir + name;
}

// One 64-bit hash over a composed frame's pixels (rows are hashed without the
// stride padding; the geometry is folded in so a resize cannot hash equal). Runs
// once per frame in the reference modes only - it is a correctness tool, not part
// of any measured phase.
uint64_t HashFrame(const uint8_t* data, int stride, int width, int height) {
  // FNV-1a with four independent lanes: the plain scalar chain is dependency-bound
  // (one multiply per word, ~3-4 cycles each), which at this platform's low clock
  // costs several ms for a whole-screen frame. Four lanes hide that latency. Any
  // stable hash does - it is only ever compared against itself - so this needs no
  // particular strength, just determinism.
  uint64_t h[4] = {1469598103934665603ull, 1099511628211ull, 0x9e3779b97f4a7c15ull,
                   0xc2b2ae3d27d4eb4full};
  h[0] ^= static_cast<uint64_t>(static_cast<uint32_t>(width)) * 0x9e3779b97f4a7c15ull;
  h[2] ^= static_cast<uint64_t>(static_cast<uint32_t>(height)) * 0xc2b2ae3d27d4eb4full;
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = data + static_cast<size_t>(y) * static_cast<size_t>(stride);
    size_t i = 0;
    for (; i + 32 <= rowBytes; i += 32) {
      for (int k = 0; k < 4; ++k) {
        uint64_t w = 0;
        std::memcpy(&w, row + i + static_cast<size_t>(k) * 8u, 8);
        h[k] = (h[k] ^ w) * 1099511628211ull;
      }
    }
    for (; i + 8 <= rowBytes; i += 8) {
      uint64_t w = 0;
      std::memcpy(&w, row + i, 8);
      h[0] = (h[0] ^ w) * 1099511628211ull;
    }
    for (; i < rowBytes; ++i) {
      h[1] = (h[1] ^ row[i]) * 1099511628211ull;
    }
  }
  return h[0] ^ (h[1] * 0x9e3779b97f4a7c15ull) ^ (h[2] * 0xc2b2ae3d27d4eb4full) ^ h[3];
}

// 32bpp BI_RGB BMP: viewable everywhere (Windows included) and trivial to write,
// so the reference picture needs no image codec in the app.
bool WriteBmp(const std::string& path, const uint8_t* data, int stride, int width, int height) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const uint32_t rowBytes = static_cast<uint32_t>(width) * 4u;
  const uint32_t imageBytes = rowBytes * static_cast<uint32_t>(height);
  uint8_t header[54] = {0};
  header[0] = 'B';
  header[1] = 'M';
  const uint32_t fileBytes = 54u + imageBytes;
  auto put32 = [&header](size_t off, uint32_t v) { std::memcpy(header + off, &v, 4); };
  put32(2, fileBytes);
  put32(10, 54u);
  put32(14, 40u);                                   // BITMAPINFOHEADER
  put32(18, static_cast<uint32_t>(width));
  put32(22, static_cast<uint32_t>(height));         // positive: rows bottom-up
  put32(26, 1u);                                    // planes
  put32(28, 32u);                                   // bits per pixel
  put32(34, imageBytes);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  bool ok = std::fwrite(header, 1, sizeof(header), f) == sizeof(header);
  for (int y = height - 1; ok && y >= 0; --y) {
    ok = std::fwrite(data + static_cast<size_t>(y) * static_cast<size_t>(stride), 1, rowBytes, f) ==
         rowBytes;
  }
  std::fclose(f);
  return ok;
}

// Reads a 32bpp BI_RGB BMP into a top-down BGRA buffer (row pitch = width * 4).
bool ReadBmp(const std::string& path, std::vector<uint8_t>* out, int* width, int* height) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return false;
  }
  uint8_t header[54] = {0};
  const bool read = std::fread(header, 1, sizeof(header), f) == sizeof(header);
  uint32_t w = 0;
  uint32_t h = 0;
  uint16_t bpp = 0;
  if (read) {
    std::memcpy(&w, header + 18, 4);
    std::memcpy(&h, header + 22, 4);
    std::memcpy(&bpp, header + 28, 2);
  }
  const uint32_t offset = 54u;
  const bool usable = read && header[0] == 'B' && header[1] == 'M' && bpp == 32u && w > 0 &&
                      w <= 16384u && (h != 0u);
  if (!usable) {
    std::fclose(f);
    return false;
  }
  const bool bottomUp = (h & 0x80000000u) == 0u;
  const uint32_t uh = h & 0x7fffffffu;
  const uint32_t rowBytes = w * 4u;
  out->assign(static_cast<size_t>(rowBytes) * uh, 0);
  std::fseek(f, static_cast<long>(offset), SEEK_SET);
  for (uint32_t row = 0; row < uh; ++row) {
    const uint32_t y = bottomUp ? (uh - 1u - row) : row;
    if (std::fread(out->data() + static_cast<size_t>(y) * rowBytes, 1, rowBytes, f) != rowBytes) {
      std::fclose(f);
      return false;
    }
  }
  std::fclose(f);
  *width = static_cast<int>(w);
  *height = static_cast<int>(uh);
  return true;
}

// Header of hmrdp_ref.hash: magic + geometry + the frame the image came from +
// the frame count, then the per-frame hashes.
constexpr char kRefMagic[8] = {'H', 'M', 'R', 'D', 'R', 'E', 'F', '1'};

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Process CPU time (all threads), the energy proxy for the thread-count A/B: a
// run that gets no faster while burning more CPU is the wrong side of the
// efficiency curve. Zero when the platform does not provide it.
int64_t ProcessCpuUs() {
  struct timespec ts = {};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
    return 0;
  }
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec) / 1000;
}

// Stable route name for the stats panel / hilog.
const char* RouteName(GfxReplayRoute route) {
  switch (route) {
    case GfxReplayRoute::kCpu:
      return "cpu";
    case GfxReplayRoute::kVulkan:
      return "vulkan";

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
                      const std::string& gfxPath, GfxReplayRoute route, bool realtime,
                      GfxReplayRefMode refMode) {
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
    imgDiffChecks_.store(0);
    imgDiffBad_.store(0);
    imgDiffRgbPx_.store(0);
    imgDiffAlphaPx_.store(0);
    imgDiffBBoxX0_.store(-1);
    imgDiffBBoxY0_.store(-1);
    imgDiffBBoxX1_.store(-1);
    imgDiffBBoxY1_.store(-1);
    imgDiffMaxDelta_.store(0);
    imgDiffSmallPx_.store(0);
    refHashes_.clear();
    refImage_.clear();
    refImageW_ = 0;
    refImageH_ = 0;
    refImageFrame_ = -1;
    refHashUs_.store(0);
    refChecks_.store(0);
    refBad_.store(0);
    refFirstBad_.store(-1);
    refNote_.clear();
    // The reference modes only make sense on the CPU (gdi) route: they hash the
    // composed desktop, which is what that route produces and what the engine
    // route is supposed to reproduce.
    refMode_.store(route == GfxReplayRoute::kCpu ? static_cast<int>(refMode) : 0);
    // This run's number, so a driver can wait for *this* run's end instead of
    // guessing from a state line that looks the same for every round.
    runId_.fetch_add(1);
    aborted_.store(false);
    refTag_ = CaptureTag(gfxPath);
    if (refMode_.load() == static_cast<int>(GfxReplayRefMode::kCompare)) {
      if (!RefLoad(gfxPath)) {
        HMRDP_LOGW("gfx replay: reference unusable (%{public}s)", refNote_.c_str());
      }
    }
    // The inverse DWT is compared against its scalar reference during the
    // reference runs only: that is the run that has to prove bit-exactness, and
    // the extra (sampled) decode work would show up in a performance run.
    if (HmrdpSetDwtCheck != nullptr) {
      HmrdpSetDwtCheck(refMode_.load() == static_cast<int>(GfxReplayRefMode::kCompare) ? 1 : 0);
    }
    presentUs_.store(0);
    uploadBytes_.store(0);
    uploadBoxBytes_.store(0);
    uploadRectPresents_.store(0);
    uploadTruncated_.store(0);
    uploadMaxRects_.store(0);
    cpuStartUs_.store(0);
    cpuEndUs_.store(0);
    pumpUs_.store(0);
    pumpStartUs_.store(0);
    paceUs_.store(0);
    realtimeRequested_.store(realtime ? 1 : 0);
    realtimeActive_.store(effectiveRealtime ? 1 : 0);
    realtimeLagUs_.store(0);
    recordBaseUs_ = 0;
    recordWallBaseUs_ = 0;
    firstFrameEndUs_.store(0);
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
  HMRDP_LOGI("gfx replay: decode %{public}s", DecodeThreadsInfo().c_str());
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
  // fps is the **playback** rate: measured from the first presented frame (see
  // MarkFrameEnd) to the end, so the run's start-up (engine/presenter init, waiting
  // for the first frame) does not dilute it. MarkFrameEnd counts one frame per
  // presented frame, hence `presents - 1` periods.
  const uint64_t presents = presents_.load();
  const int64_t firstFrameEndUs = firstFrameEndUs_.load();
  const int64_t elapsedUs = firstFrameEndUs != 0
                                ? nowUs - firstFrameEndUs
                                : (startUs_.load() != 0 ? nowUs - startUs_.load() : 0);
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
  // "feed" = compute time, excluding the deliberate playback throttling (only the
  // realtime cadence sleeps). Once the pump has returned its exact figure is used;
  // while still running the wall clock minus the paced sleep is a good live
  // approximation.
  const uint64_t elapsed = elapsedUs > 0 ? static_cast<uint64_t>(elapsedUs) : 0;
  const uint64_t measuredPump = pumpUs_.load();
  uint64_t pumpUs = measuredPump > 0
                        ? (measuredPump > pace ? measuredPump - pace : 0)
                        : (elapsed > pace ? elapsed - pace : 0);
  // Exclude the run's start-up (engine/presenter init, first frame wait) so `feed`
  // is the playback's own compute, comparable between runs and routes.
  const int64_t firstFrameEnd = firstFrameEndUs_.load();
  const int64_t pumpStart = pumpStartUs_.load();
  if (firstFrameEnd != 0 && pumpStart != 0 && firstFrameEnd > pumpStart &&
      static_cast<uint64_t>(firstFrameEnd - pumpStart) < pumpUs) {
    pumpUs -= static_cast<uint64_t>(firstFrameEnd - pumpStart);
  }

  const char* routeName = RouteName(static_cast<GfxReplayRoute>(route_.load()));
  // mode=realtime replays the capture's own arrival times; mode=fast is *not
  // throttled at all* - the pump runs flat out, i.e. a pure throughput figure and
  // not the live cadence (a frame budget used to be applied here; it kept the CPU
  // idle between frames, which dropped the SoC clock ~3x and inflated every
  // per-frame number - doc_agent/gfx-engine.md §8.3).
  // `lag` is only meaningful in realtime mode: the worst lateness behind the
  // recorded schedule, i.e. how much of the live load the client could not absorb.
  // "fast(untimed)" = realtime was requested but the capture carries no arrival
  // times, so the unpaced fast mode ran instead (the difference is visible here on
  // purpose: two runs that differ in mode are not comparable).
  const char* modeName = realtimeActive_.load() != 0
                             ? "realtime"
                             : (realtimeRequested_.load() != 0 ? "fast(untimed)" : "fast");
  const unsigned long long frames = static_cast<unsigned long long>(frames_.load());
  // The state and the run number come first, so a test driver can (a) wait for
  // the round it just started to reach "finished" and (b) be sure the figures
  // below are that round's - polling the figures alone cannot tell them apart.
  const char* stateName = running_.load() ? "running" : (aborted_.load() ? "aborted" : "finished");
  char head[420];
  std::snprintf(head, sizeof(head),
                "state=%s  run=%llu  route=%s  mode=%s  frames=%llu  presents=%llu  fps=%.1f  "
                "fail=%llu  skip=%llu\n"
                "feed=%llums   parse=%llums   present=%.2fms   lag=%llums   (running=%d)",
                stateName, static_cast<unsigned long long>(runId_.load()), routeName, modeName,
                frames, static_cast<unsigned long long>(presents), fps,
                static_cast<unsigned long long>(presentFailures_.load()),
                static_cast<unsigned long long>(presentSkips_.load()),
                static_cast<unsigned long long>(pumpUs / 1000),
                static_cast<unsigned long long>(hmrdp::GfxReplayParseUs() / 1000),
                avgMs(presentUs_.load(), presents),
                static_cast<unsigned long long>(realtimeLagUs_.load() / 1000),
                running_.load() ? 1 : 0);
  std::string out(head);

  // Worker count + the run's total process CPU time: the thread-count A/B needs
  // both the wall time (above) and what it cost, or "same speed, more cores
  // woken" looks like a tie (doc_agent/cpu-accel-plan.md §1).
  const int64_t cpuStart = cpuStartUs_.load();
  const int64_t cpuEnd = cpuEndUs_.load();
  if (cpuStart != 0 && cpuEnd > cpuStart) {
    unsigned int pool[3] = {0, 0, 0};
    if (HmrdpGetDecodeThreadsStats != nullptr) {
      HmrdpGetDecodeThreadsStats(pool);
    }
    // cpuKHz is the SoC clock the run actually got: the playback rate decides it
    // (an idle-paced run sits at the lowest frequency and every per-frame figure
    // is ~3x larger), so two runs are only comparable at the same value
    // (doc_agent/gfx-engine.md §8.3).
    const std::string freq = hmrdp::CpuFreqInfo();
    char run[240];
    std::snprintf(run, sizeof(run),
                  "\nrun  threads=%d  libWants=%u  applies=%u resizes=%u  cpu=%.2fs  cpuKHz=%s",
                  hmrdp::DecodeThreads(), pool[0], pool[1], pool[2],
                  static_cast<double>(cpuEnd - cpuStart) / 1000000.0,
                  freq.empty() ? "n/a" : freq.c_str());
    out += run;
  }

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

  // Per-frame client work, measured exactly like the live session's "本机"
  // (hmrdp_gfx_work.h): same hooks, same phases, same denominator. Only the CPU
  // route installs the meter - it is the one that mirrors live (gdi) - so this
  // line is where a live session's toolbar figure can be checked against a
  // replay of the same stream. Whole-run totals, not a per-second window.
  const GfxWorkMeter::Sample work = meter_.Peek();
  if (work.frames > 0) {
    const uint64_t frames = work.frames;
    char wl[400];
    std::snprintf(wl, sizeof(wl),
                  "\nperFrame 本机=%lluus (max %lluus) = zgx+parse %lluus + decode %lluus"
                  " + compose %lluus + present %lluus  (+ sync %lluus blocked)\n"
                  "         frames=%llu cmds/frame=%llu kB/frame=%llu",
                  static_cast<unsigned long long>(work.WorkUs() / frames),
                  static_cast<unsigned long long>(work.maxFrameUs),
                  static_cast<unsigned long long>(work.zgxParseUs / frames),
                  static_cast<unsigned long long>(work.decodeUs / frames),
                  static_cast<unsigned long long>(work.composeUs / frames),
                  static_cast<unsigned long long>(work.presentUs / frames),
                  static_cast<unsigned long long>(work.syncUs / frames),
                  static_cast<unsigned long long>(frames),
                  static_cast<unsigned long long>(work.commands / frames),
                  static_cast<unsigned long long>(work.bytes / frames / 1024));
    out += wl;

    // Non-pixel GFX commands, per frame: these used to land in `zgx+parse`
    // (a CreateSurface / ResetGraphics 0xFF-fills a whole surface).
    const uint64_t* su = work.setupUs;
    const uint64_t* sc = work.setupCount;
    if (work.SetupUs() > 0) {
      char st[400];
      auto ms = [&frames](uint64_t us) {
        return static_cast<double>(us) / static_cast<double>(frames) / 1000.0;
      };
      std::snprintf(
          st, sizeof(st),
          "\nsetup ms/frame: reset=%.2f(%llu) create=%.2f(%llu) delete=%.2f(%llu) map=%.2f(%llu)"
          " fill=%.2f(%llu) blit=%.2f(%llu) cache=%.2f(%llu) imp=%.2f(%llu)  total=%.2fms/frame",
          ms(su[static_cast<int>(GfxSetupKind::kReset)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kReset)]),
          ms(su[static_cast<int>(GfxSetupKind::kCreate)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kCreate)]),
          ms(su[static_cast<int>(GfxSetupKind::kDelete)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kDelete)]),
          ms(su[static_cast<int>(GfxSetupKind::kMap)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kMap)]),
          ms(su[static_cast<int>(GfxSetupKind::kFill)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kFill)]),
          ms(su[static_cast<int>(GfxSetupKind::kBlit)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kBlit)]),
          ms(su[static_cast<int>(GfxSetupKind::kCache)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kCache)]),
          ms(su[static_cast<int>(GfxSetupKind::kImport)]),
          static_cast<unsigned long long>(sc[static_cast<int>(GfxSetupKind::kImport)]),
          ms(work.SetupUs()));
      out += st;
    }

  }

  // DEV-ONLY progressive decode attribution (patched FreeRDP, see the weak
  // declaration above): per-frame milliseconds plus the update_tiles totals.
  // Only the CPU route installs the meter, so the counters are gated on it -
  // otherwise a run that follows a CPU replay would show its totals here.
  if (&HmrdpProgStat[0] != nullptr && work.frames > 0 && HmrdpProgStat[5] > 0) {
    const double d = static_cast<double>(work.frames > 0 ? work.frames : 1);
    // The tile-decode section is timed by a different slot per path and the two
    // are not the same kind of figure: on the pool path [2] is the section's wall
    // clock (its blocking part being [7]), while on the serial path [9] is the
    // time this thread spent in the per-tile loop. Only one of them is ever
    // non-zero in a run, so the reported `dec` picks whichever ran. ([9] is also
    // accumulated per chunk by the pool workers as their own summed decode time -
    // a multi-threaded CPU figure, not a per-frame one - so it must not be
    // reported next to the wall-clock phases.)
    const uint64_t decNs = HmrdpProgStat[2] != 0 ? HmrdpProgStat[2] : HmrdpProgStat[9];
    char ps[320];
    std::snprintf(ps, sizeof(ps),
                  "\nprog  ms/frame: read=%.2f dispatch=%.2f dec=%.2f (blocked=%.2f) "
                  "update=%.2f  (calls=%llu unions=%llu tiles=%llu tilesDec=%llu)",
                  static_cast<double>(HmrdpProgStat[0]) / d / 1e6,
                  static_cast<double>(HmrdpProgStat[1]) / d / 1e6,
                  static_cast<double>(decNs) / d / 1e6,
                  static_cast<double>(HmrdpProgStat[7]) / d / 1e6,
                  static_cast<double>(HmrdpProgStat[3]) / d / 1e6,
                  static_cast<unsigned long long>(HmrdpProgStat[5]),
                  static_cast<unsigned long long>(HmrdpProgStat[4]),
                  static_cast<unsigned long long>(HmrdpProgStat[6]),
                  static_cast<unsigned long long>(HmrdpProgStat[8]));
    out += ps;

    // DEV-ONLY: where the time inside one decoded tile goes. The probe times a
    // phase for one tile in 16 ([16] counts the sampled tiles), so a phase's
    // total is scaled by the tiles decoded per frame to land in the same unit as
    // `dec` above - their sum should come out just under it (the remainder is the
    // per-tile call overhead and the message-level work).
    const uint64_t samples = HmrdpProgStat[16];
    if (samples > 0) {
      const double tilesPerFrame = static_cast<double>(HmrdpProgStat[8]) / d;
      const double scale = tilesPerFrame / static_cast<double>(samples) / 1e6;
      const double phases[6] = {
          static_cast<double>(HmrdpProgStat[10]) * scale, static_cast<double>(HmrdpProgStat[11]) * scale,
          static_cast<double>(HmrdpProgStat[12]) * scale, static_cast<double>(HmrdpProgStat[13]) * scale,
          static_cast<double>(HmrdpProgStat[14]) * scale, static_cast<double>(HmrdpProgStat[15]) * scale};
      const double sum = phases[0] + phases[1] + phases[2] + phases[3] + phases[4] + phases[5];
      char ps2[320];
      std::snprintf(ps2, sizeof(ps2),
                    "\nprog2 ms/frame (sampled 1/16, n=%llu): rlgr=%.2f dequant+diff=%.2f "
                    "idwt=%.2f state=%.2f upgrade=%.2f color=%.2f  sum=%.2f",
                    static_cast<unsigned long long>(samples), phases[0], phases[1], phases[2],
                    phases[3], phases[4], phases[5], sum);
      out += ps2;
    }
  }

  // DEV-ONLY: the inverse-DWT (or any other decode-side rewrite) checked against
  // the upstream scalar reference. Only the reference runs switch it on; the
  // worst |delta| is what separates a rounding difference from a structural one.
  if (refMode_.load() != 0 && &HmrdpDwtCheckStat[0] != nullptr) {
    char dw[192];
    std::snprintf(dw, sizeof(dw), "\ndwt check: tiles=%llu mismatch=%llu maxDelta=%llu",
                  HmrdpDwtCheckStat[0], HmrdpDwtCheckStat[1], HmrdpDwtCheckStat[2]);
    out += dw;
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
  // Golden-reference gate (CPU route; see GfxReplayRefMode). The hashes are the
  // pass/fail; the pixel numbers come from the single frame the stored image was
  // taken from, i.e. they only say *how* it failed, not where every frame differs.
  const int refMode = refMode_.load();
  if (refMode != 0) {
    char rf[320];
    if (refMode == static_cast<int>(GfxReplayRefMode::kExport)) {
      std::snprintf(rf, sizeof(rf),
                    "\nref export: frames=%llu -> hmrdp_ref_%s.hash + .bmp   hashMs=%.1f",
                    static_cast<unsigned long long>(refFrames_.load()), refTag_.c_str(),
                    static_cast<double>(refHashUs_.load()) / 1000.0);
    } else {
      std::snprintf(rf, sizeof(rf),
                    "\nref compare: refFrames=%llu checks=%llu bad=%llu firstBad=%lld "
                    "imageFrame=%lld rgbPx=%llu alphaPx=%llu maxDelta=%d bbox=(%d,%d)-(%d,%d) hashMs=%.1f%s%s",
                    static_cast<unsigned long long>(refFrames_.load()),
                    static_cast<unsigned long long>(refChecks_.load()),
                    static_cast<unsigned long long>(refBad_.load()),
                    static_cast<long long>(refFirstBad_.load()),
                    static_cast<long long>(refImageFrame_),
                    static_cast<unsigned long long>(imgDiffRgbPx_.load()),
                    static_cast<unsigned long long>(imgDiffAlphaPx_.load()),
                    imgDiffMaxDelta_.load(), imgDiffBBoxX0_.load(), imgDiffBBoxY0_.load(),
                    imgDiffBBoxX1_.load(), imgDiffBBoxY1_.load(),
                    static_cast<double>(refHashUs_.load()) / 1000.0,
                    refNote_.empty() ? "" : "  note=", refNote_.c_str());
    }
    out += rf;
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

void GfxReplay::MarkFrameEnd(bool presented) {
  // Cadence bookkeeping only - the fast mode deliberately does not sleep (a frame
  // budget left the CPU idle between frames, which dropped the SoC to its lowest
  // clock and inflated every per-frame cost ~3x, see doc_agent/gfx-engine.md §8.3), and
  // the realtime mode's cadence comes from the capture (PaceRecord).
  //
  // Only frames that produced a picture mark the origin: the stream carries many
  // frame markers with no drawable update (management/ack frames, off-screen
  // surfaces), and those must not be mistaken for the first played frame.
  if (!presented) {
    return;
  }
  if (firstFrameEndUs_.load() == 0) {
    // fps is measured from here, so the run's start-up is not counted as playback.
    firstFrameEndUs_.store(static_cast<int64_t>(NowUs()));
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
  MarkFrameEnd(presented);
}

void GfxReplay::Run() {
  switch (static_cast<GfxReplayRoute>(route_.load())) {
    case GfxReplayRoute::kCpu:
      RunCpuReplay(gfxPath_);
      break;
    case GfxReplayRoute::kVulkan:
      RunVulkanReplay(gfxPath_);
      break;
  }
  endUs_.store(NowUs());
  running_.store(false);
}

void GfxReplay::RunVulkanReplay(const std::string& gfxPath) {
  std::unique_ptr<ReplayDesktop> desktop(new ReplayDesktop());
  std::string error;
  if (!desktop->Init(window_, surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error.empty() ? "engine init failed" : error;
    return;
  }
  desktop_ = std::move(desktop);
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

  bool aborted = false;
  const bool ok = GfxReplayStream(gfxPath, &sink, [this]() { OnReplayFrame(); }, &running_, &error,
                                  pace, paceAccum, &aborted);
  aborted_.store(aborted);
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
  // gdi composes straight into the presenter's desktop buffer when it can hand one
  // over (the presenter is up by now, so it does): the present then costs a copy
  // of the dirty rects out of memory gdi already wrote, with no frame copy at all.
  cpu.SetPresenter(presenter_.get());
  std::string error;
  if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
    return;
  }
  cpu.SetFrameFn([this, &cpu]() { OnCpuFrame(&cpu); });

  // Measure this run with the live session's meter (same hooks, same phases), so
  // the CPU route's per-frame figures can be put next to a live session's. A live
  // session holds the slot while it is connected and is the authoritative
  // reporter, so a replay started next to one runs without per-frame accounting
  // instead of stealing its numbers.
  const bool metered = ActiveWorkMeter() == nullptr;
  if (!metered) {
    HMRDP_LOGW("gfx replay: a live session is measuring, per-frame work not reported");
  } else {
    meter_.Reset();
    SetActiveWorkMeter(&meter_);
  }
  // Installed even while a live session owns the meter: the wrappers are also
  // where the frame-begin wait lives (GfxWorkSetFrameBeginHook), and gdi writes the
  // presenter's desktop buffer in place, so skipping it would bring back the
  // two-frames-in-one-upload race this replay is often run to catch.
  GfxWorkInstall(cpu.gfx());
  GfxWorkSetFrameBeginHook([&cpu]() { cpu.OnFrameBegin(); });

  hmrdp::GfxDumpSetReplaying(true);
  hmrdp::GfxReplayResetParseUs();

  // DEV-ONLY: reset the patched decoder's phase counters so this run's figures
  // are not mixed with a previous one (the library outlives the replay).
  if (&HmrdpProgStat[0] != nullptr) {
    for (int i = 0; i < kProgStatSlots; ++i) {
      HmrdpProgStat[i] = 0;
    }
  }
  if (&HmrdpDwtCheckStat[0] != nullptr) {
    for (int i = 0; i < kDwtCheckSlots; ++i) {
      HmrdpDwtCheckStat[i] = 0;
    }
  }

  const int64_t pumpStart = NowUs();
  pumpStartUs_.store(pumpStart);
  cpuStartUs_.store(ProcessCpuUs());

  ReplayPaceFn pace;
  if (realtimeActive_.load() != 0) {
    pace = [this](uint64_t tsUs) { PaceRecord(tsUs); };
  }
  const ReplayPaceAccumFn paceAccum = [this]() { return paceUs_.load(); };
  bool aborted = false;
  const bool ok =
      GfxReplayPump(gfxPath, cpu.gfx(), &running_, &error, pace, paceAccum, &aborted);
  aborted_.store(aborted);
  cpuEndUs_.store(ProcessCpuUs());
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  // Record the reference before the desktop is torn down: the buffer still holds
  // the last composed frame (the one the hash list's last entry describes). A run
  // that was cut short is not written: its hash list covers only part of the
  // capture, and storing it would replace a good reference with a partial one.
  if (refMode_.load() == static_cast<int>(GfxReplayRefMode::kExport) && !aborted_.load()) {
    RefWrite(gfxPath, &cpu);
  }

  hmrdp::GfxDumpSetReplaying(false);
  cpu.SetFrameFn(nullptr);
  GfxWorkSetFrameBeginHook(nullptr);
  GfxWorkUninstall(cpu.gfx());
  if (metered) {
    SetActiveWorkMeter(nullptr);
  }
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished (cpu): %{public}s", Stats().c_str());
}

// ---- golden reference (see GfxReplayRefMode) ------------------------------

bool GfxReplay::RefLoad(const std::string& capturePath) {
  const std::string hashPath = RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".hash");
  FILE* f = std::fopen(hashPath.c_str(), "rb");
  if (f == nullptr) {
    refNote_ = "no " + hashPath;
    return false;
  }
  char magic[8] = {0};
  uint32_t geom[4] = {0, 0, 0, 0};  // width, height, imageFrame, frameCount
  bool ok = std::fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
            std::memcmp(magic, kRefMagic, sizeof(kRefMagic)) == 0 &&
            std::fread(geom, 1, sizeof(geom), f) == sizeof(geom) && geom[3] > 0u &&
            geom[3] < 1000000u;
  if (ok) {
    refHashes_.resize(geom[3]);
    ok = std::fread(refHashes_.data(), 8, geom[3], f) == geom[3];
  }
  std::fclose(f);
  if (!ok) {
    refHashes_.clear();
    refNote_ = "corrupt " + hashPath;
    return false;
  }
  refFrames_.store(refHashes_.size());
  refImageFrame_ = static_cast<int64_t>(geom[2]);
  // The image is optional: the per-frame hashes are the gate, the image exists so
  // a failure can be looked at (and for the pixel-level numbers on its frame).
  if (!ReadBmp(RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".bmp"), &refImage_, &refImageW_, &refImageH_)) {
    refNote_ = "no reference image (hash-only check)";
  }
  HMRDP_LOGI("gfx replay: reference loaded (%{public}zu frames, image %{public}dx%{public}d "
             "@%{public}lld)",
             refHashes_.size(), refImageW_, refImageH_,
             static_cast<long long>(refImageFrame_));
  return true;
}

void GfxReplay::RefWrite(const std::string& capturePath, GfxCpuDesktop* cpu) {
  rdpGdi* gdi = cpu != nullptr ? cpu->gdi() : nullptr;
  if (gdi == nullptr || gdi->primary_buffer == nullptr || refHashes_.empty()) {
    return;
  }
  const std::string hashPath = RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".hash");
  FILE* f = std::fopen(hashPath.c_str(), "wb");
  if (f == nullptr) {
    HMRDP_LOGW("gfx replay: cannot write %{public}s", hashPath.c_str());
    return;
  }
  // The image is the last composed frame, i.e. the hash list's last entry.
  const uint32_t geom[4] = {static_cast<uint32_t>(gdi->width),
                            static_cast<uint32_t>(gdi->height),
                            static_cast<uint32_t>(refHashes_.size() - 1u),
                            static_cast<uint32_t>(refHashes_.size())};
  std::fwrite(kRefMagic, 1, sizeof(kRefMagic), f);
  std::fwrite(geom, 1, sizeof(geom), f);
  std::fwrite(refHashes_.data(), 8, refHashes_.size(), f);
  std::fclose(f);
  const bool bmp = WriteBmp(RefSibling(capturePath, "hmrdp_ref_" + refTag_ + ".bmp"), gdi->primary_buffer,
                            static_cast<int>(gdi->stride), static_cast<int>(gdi->width),
                            static_cast<int>(gdi->height));
  HMRDP_LOGI("gfx replay: reference written (%{public}zu frames, image %{public}s)",
             refHashes_.size(), bmp ? "ok" : "FAILED");
}

void GfxReplay::RefCompareImage(const uint8_t* data, int stride, int width, int height) {
  const size_t rowBytes = static_cast<size_t>(width) * 4u;
  if (refImageW_ != width || refImageH_ != height ||
      refImage_.size() != rowBytes * static_cast<size_t>(height)) {
    refNote_ = "stored image geometry differs";
    return;
  }
  uint64_t rgbPx = 0;
  uint64_t alphaPx = 0;
  uint64_t smallPx = 0;
  int maxDelta = 0;
  int x0 = -1;
  int y0 = -1;
  int x1 = -1;
  int y1 = -1;
  for (int y = 0; y < height; ++y) {
    const uint8_t* cur = data + static_cast<size_t>(y) * static_cast<size_t>(stride);
    const uint8_t* ref = refImage_.data() + static_cast<size_t>(y) * rowBytes;
    for (int x = 0; x < width; ++x) {
      const uint8_t* a = cur + static_cast<size_t>(x) * 4u;  // BGRA
      const uint8_t* b = ref + static_cast<size_t>(x) * 4u;
      const int dR = std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2]));
      const int dG = std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1]));
      const int dB = std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0]));
      const int d = std::max(dR, std::max(dG, dB));
      if (d > 0) {
        ++rgbPx;
        if (d > maxDelta) {
          maxDelta = d;
        }
        if (d <= 2) {
          ++smallPx;
        }
        if (x0 < 0 || x < x0) {
          x0 = x;
        }
        if (x1 < x) {
          x1 = x;
        }
        if (y0 < 0 || y < y0) {
          y0 = y;
        }
        y1 = y;
      }
      if (a[3] != b[3]) {
        ++alphaPx;
      }
    }
  }
  imgDiffChecks_.fetch_add(1);
  if (rgbPx > 0) {
    imgDiffBad_.fetch_add(1);
  }
  imgDiffRgbPx_.store(rgbPx);
  imgDiffAlphaPx_.store(alphaPx);
  imgDiffSmallPx_.store(smallPx);
  imgDiffMaxDelta_.store(maxDelta);
  imgDiffBBoxX0_.store(x0);
  imgDiffBBoxY0_.store(y0);
  imgDiffBBoxX1_.store(x1);
  imgDiffBBoxY1_.store(y1);
  HMRDP_LOGI("gfx replay: reference image compare: rgbPx=%{public}llu maxDelta=%{public}d",
             static_cast<unsigned long long>(rgbPx), maxDelta);
}

void GfxReplay::RefCollectFrame(GfxCpuDesktop* cpu) {
  const int mode = refMode_.load();
  if (mode == 0 || cpu == nullptr) {
    return;
  }
  rdpGdi* gdi = cpu->gdi();
  if (gdi == nullptr || gdi->primary_buffer == nullptr || gdi->width == 0 || gdi->height == 0) {
    return;
  }
  const int width = static_cast<int>(gdi->width);
  const int height = static_cast<int>(gdi->height);
  const int stride = static_cast<int>(gdi->stride);
  // This runs inside gdi's EndFrame (OnCpuFrame is called from its EndPaint hook),
  // so the time it takes is charged to the `compose` phase: it is reported
  // separately in the reference line so a reference run is never mistaken for a
  // performance run.
  const int64_t hashStart = NowUs();
  const uint64_t hash = HashFrame(gdi->primary_buffer, stride, width, height);
  // `frames_` was already incremented for this frame by the caller, so the index
  // is frames - 1: the same numbering the reference file was written with.
  const int64_t index = static_cast<int64_t>(frames_.load()) - 1;
  refHashUs_.fetch_add(static_cast<uint64_t>(NowUs() - hashStart));
  if (mode == static_cast<int>(GfxReplayRefMode::kExport)) {
    refHashes_.push_back(hash);
    refFrames_.store(refHashes_.size());
    return;
  }
  refChecks_.fetch_add(1);
  if (index >= 0 && static_cast<size_t>(index) < refHashes_.size()) {
    if (refHashes_[static_cast<size_t>(index)] != hash) {
      if (refBad_.fetch_add(1) == 0) {
        refFirstBad_.store(index);
        // Dump the frame that failed so it can be looked at next to the reference.
        WriteBmp(RefSibling(gfxPath_, "hmrdp_ref_" + refTag_ + "_fail.bmp"), gdi->primary_buffer, stride, width,
                 height);
      }
    }
  } else {
    // This run produced more frames than the reference recorded.
    refBad_.fetch_add(1);
  }
  if (index == refImageFrame_) {
    RefCompareImage(gdi->primary_buffer, stride, width, height);
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
  // The frame's wait for the GPU to release the desktop buffer happened in
  // BeginPaint, before this; report it so it is not mistaken for compose work
  // (hmrdp_gfx_work.h).
  meter_.OnPresentSync(cpu->TakePresentSyncUs());
  const int64_t presentStart = NowUs();
  PresentUploadInfo upload;
  const bool presented = PresentGdiFrame(cpu->gdi(), presenter_.get(), &upload);
  const uint64_t presentUs = static_cast<uint64_t>(NowUs() - presentStart);
  RecordPresent(presentUs);
  meter_.OnPresent(presentUs);
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
  // The fast mode does not sleep here any more, so this is 0; it stays reported
  // because the meter must never charge a deliberate in-EndFrame throttle to
  // `compose` (hmrdp_gfx_work.h) if one is ever reintroduced.
  const int64_t paceStart = NowUs();
  MarkFrameEnd(presented);
  meter_.OnPace(static_cast<uint64_t>(NowUs() - paceStart));
  // Golden-reference bookkeeping, last so it cannot land in any measured phase
  // (`present` was measured above; the hash is not present work).
  RefCollectFrame(cpu);
}

}  // namespace hmrdp
