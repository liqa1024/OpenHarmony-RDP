/*
 * HmRdp - Vulkan engine correctness harness. See hmrdp_vk_selftest.h.
 */
#include "hmrdp_vk_selftest.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_vk_renderer.h"

namespace hmrdp {
namespace {

// Minimum gap between two pixel comparisons: each one is a full-screen (25 MB)
// GPU->CPU readback, so clean frames are compared every kCompareMinGapUs at most.
constexpr int64_t kCompareMinGapUs = 200000;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Adapter: the shared replay pump speaks GfxCommandSink, the Vulkan engine owns
// the actual command semantics.
class VkDeskSink : public GfxCommandSink {
 public:
  explicit VkDeskSink(GfxVkDesktop* engine) : engine_(engine) {}

  void ApplyGfx(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                uint32_t payloadLen) override {
    if (engine_ != nullptr) {
      engine_->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
    }
  }

 private:
  GfxVkDesktop* engine_ = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
// Deterministic primitive self-test
//
// The capture-based comparison cannot validate V1: an RDPGFX capture is full of
// progressive/ClearCodec commands, which V1 ignores, and surfaces are persistent,
// so from the first ignored command on the engine's pixels and gdi's are
// different for good - a "clean" frame later in the stream is still wrong. So the
// V1 claim is checked here instead: a scripted sequence of fill / upload / cache /
// copy / compose commands is applied to a small engine and compared against a
// plain CPU model of the same FreeRDP semantics.
// ---------------------------------------------------------------------------
namespace {

// CPU model of one image, in FreeRDP's packed BGRA/X words.
struct RefImage {
  int width = 0;
  int height = 0;
  std::vector<uint32_t> pixels;

  void Reset(int w, int h, uint32_t value) {
    width = w;
    height = h;
    pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), value);
  }
  uint32_t At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + x];
  }
  void FillRect(int left, int top, int right, int bottom, uint32_t value) {
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        pixels[static_cast<size_t>(y) * width + x] = value;
      }
    }
  }
  // Same rect copy as the engine: clip to the destination, then stage so
  // overlapping same-image copies are defined.
  void CopyRect(const RefImage& src, int sx, int sy, int dx, int dy, int w, int h) {
    int cx = dx;
    int cy = dy;
    int cw = w;
    int ch = h;
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx + cw > width) cw = width - cx;
    if (cy + ch > height) ch = height - cy;
    if (cw <= 0 || ch <= 0) return;
    const int ox = sx + (cx - dx);
    const int oy = sy + (cy - dy);
    if (ox < 0 || oy < 0 || ox + cw > src.width || oy + ch > src.height) return;
    std::vector<uint32_t> staged(static_cast<size_t>(cw) * ch);
    for (int y = 0; y < ch; ++y) {
      for (int x = 0; x < cw; ++x) {
        staged[static_cast<size_t>(y) * cw + x] = src.pixels[static_cast<size_t>(oy + y) * src.width + (ox + x)];
      }
    }
    for (int y = 0; y < ch; ++y) {
      for (int x = 0; x < cw; ++x) {
        pixels[static_cast<size_t>(cy + y) * width + (cx + x)] = staged[static_cast<size_t>(y) * cw + x];
      }
    }
  }
};

struct PrimReport {
  int steps = 0;
  int bad = 0;
  // False on a device whose readback does not work: the command path is still
  // exercised (that is what the emulator can cover), but no content verdict is
  // produced instead of a wrong one.
  bool verify = true;
  std::string firstFail;

  void Fail(const std::string& label, int x, int y, uint32_t got, uint32_t want) {
    if (firstFail.empty() && bad == 0) {
      char buf[192];
      std::snprintf(buf, sizeof(buf), "%s at (%d,%d) got=0x%08x want=0x%08x", label.c_str(), x, y,
                    static_cast<unsigned>(got), static_cast<unsigned>(want));
      firstFail = buf;
    }
    bad++;
  }
};

// Compares an engine surface with the model (BGRA words at the engine's stride).
void CheckSurface(GfxVkDesktop* engine, RefImage* ref, uint16_t id, const char* label,
                  PrimReport* report) {
  report->steps++;
  if (!report->verify) {
    return;
  }
  std::vector<uint8_t> got;
  if (!engine->ReadSurface(id, &got)) {
    report->Fail(std::string(label) + ": ReadSurface failed", 0, 0, 0, 0);
    return;
  }
  const GpuSurface* meta = engine->FindSurface(id);
  if (meta == nullptr) {
    report->Fail(std::string(label) + ": no surface meta", 0, 0, 0, 0);
    return;
  }
  const int strideWords = meta->stride / 4;
  if (got.size() != static_cast<size_t>(meta->stride) * meta->height) {
    report->Fail(std::string(label) + ": size", 0, 0, 0, 0);
    return;
  }
  const uint32_t* words = reinterpret_cast<const uint32_t*>(got.data());
  for (int y = 0; y < ref->height; ++y) {
    for (int x = 0; x < ref->width; ++x) {
      const uint32_t g = words[static_cast<size_t>(y) * strideWords + x];
      const uint32_t w = ref->At(x, y);
      if (g != w) {
        report->Fail(label, x, y, g, w);
        return;
      }
    }
  }
}

void CheckScreen(GfxVkDesktop* engine, const RefImage& ref, const char* label,
                 PrimReport* report) {
  report->steps++;
  if (!report->verify) {
    return;
  }
  std::vector<uint8_t> got;
  if (!engine->ReadScreen(&got)) {
    report->Fail(std::string(label) + ": ReadScreen failed", 0, 0, 0, 0);
    return;
  }
  const int w = engine->screenWidth();
  const int h = engine->screenHeight();
  if (w != ref.width || h != ref.height ||
      got.size() != static_cast<size_t>(w) * h * 4) {
    report->Fail(std::string(label) + ": screen size", 0, 0, 0, 0);
    return;
  }
  const uint32_t* words = reinterpret_cast<const uint32_t*>(got.data());
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint32_t g = words[static_cast<size_t>(y) * w + x];
      const uint32_t want = ref.At(x, y);
      if (g != want) {
        report->Fail(label, x, y, g, want);
        return;
      }
    }
  }
}

}  // namespace

std::string GfxVkSelfTest::RunPrimitives() {
  PrimReport report;
  GfxVkDesktop engine;
  if (!engine.Init()) {
    return "primitives: engine init FAILED";
  }

  // Step 0: can this device move data GPU->CPU at all? (No image involved.)
  // Every content check below needs it, so when it is unavailable the run still
  // exercises the whole command path but reports no pixel verdict at all.
  const bool bufferOk = engine.readbackAvailable();
  report.verify = bufferOk;

  // V2 groundwork: prove the compute path (SPIR-V + dispatch + SSBO + host
  // readback) works on this driver before the RFX kernels depend on it. Runs
  // regardless of the transfer verdict above: the two exercise different parts.
  std::string computeDetail;
  const bool computeOk = engine.ComputeSelfTest(4096, &computeDetail);

  const uint16_t kSurf = 0;
  const uint16_t kSurf2 = 1;
  const uint32_t kRed = 0xFF0000FFu;    // BGRA
  const uint32_t kGreen = 0xFF00FF00u;
  const uint32_t kWhite = 0xFFFFFFFFu;
  const uint32_t kBlack = 0x00000000u;

  // 64x48 and 32x32 are already 16-aligned, so engine and model agree on size.
  // Every engine call is asserted: a silently-false return is the difference
  // between "the copy is wrong" and "the call never ran".
  int apiFailures = 0;
  auto expectOk = [&report, &apiFailures](bool ok, const char* label) {
    if (!ok) {
      apiFailures++;
      report.Fail(std::string(label) + ": call returned false", 0, 0, 0, 0);
    }
  };

  if (!engine.CreateSurface(kSurf, 64, 48, 0x21) || !engine.CreateSurface(kSurf2, 32, 32, 0x21) ||
      !engine.ResetGraphics(128, 96)) {
    return "primitives: setup FAILED";
  }

  RefImage ref0;
  ref0.Reset(64, 48, kWhite);  // FreeRDP's 0xFF initialisation
  RefImage ref1;
  ref1.Reset(32, 32, kWhite);
  RefImage screen;
  screen.Reset(128, 96, kBlack);  // ResetGraphics clears to transparent black

  // 1. Surface creation contract: every pixel 0xFF.
  CheckSurface(&engine, &ref0, kSurf, "create 0xFF init", &report);
  CheckSurface(&engine, &ref1, kSurf2, "create 0xFF init (2)", &report);

  // 2. SolidFill: in-bounds, edge-clipped and fully outside rects.
  const uint16_t fill0[] = {4, 4, 20, 20, 60, 40, 80, 60, 70, 46, 90, 60, 0, 0, 3, 3};
  expectOk(engine.SolidFill(kSurf, kRed, fill0, 4), "SolidFill");
  ref0.FillRect(4, 4, 20, 20, kRed);
  ref0.FillRect(60, 40, 80, 60, kRed);
  ref0.FillRect(70, 46, 90, 60, kRed);  // clipped to 64x48
  ref0.FillRect(0, 0, 3, 3, kRed);
  CheckSurface(&engine, &ref0, kSurf, "solid fill", &report);

  // 3. Uncompressed 32bpp upload, partially outside the surface.
  {
    std::vector<uint8_t> bgra(static_cast<size_t>(40) * 12 * 4);
    for (size_t i = 0; i < bgra.size() / 4; ++i) {
      const uint32_t word = 0xFF000000u | static_cast<uint32_t>(i & 0xFFFFFFu);
      std::memcpy(&bgra[i * 4], &word, 4);
    }
    expectOk(engine.UploadBgra(kSurf, 50, 40, 40, 12, bgra.data(), 40 * 4),  // clips to 64x48
             "UploadBgra");
    // Model: only the in-bounds part lands.
    for (int y = 0; y < 48 - 40; ++y) {
      for (int x = 0; x < 64 - 50; ++x) {
        uint32_t word = 0;
        std::memcpy(&word, &bgra[(static_cast<size_t>(y) * 40 + x) * 4], 4);
        ref0.pixels[static_cast<size_t>(40 + y) * 64 + (50 + x)] = word;
      }
    }
    CheckSurface(&engine, &ref0, kSurf, "uncompressed 32bpp upload", &report);
  }

  // 4. Uncompressed 24bpp upload through the command dispatcher (the 24->32bpp
  //    expansion lives there, not in UploadBgra).
  {
    const uint32_t cmd[4] = {kGpuCodecUncompressed, 0, 0, 0};
    std::vector<uint8_t> params;
    const uint32_t fields[8] = {0, (24u << 24), 2, 30, 0, 0, 12, 6};
    for (uint32_t v : fields) {
      params.push_back(static_cast<uint8_t>(v & 0xFF));
      params.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
      params.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
      params.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }
    std::vector<uint8_t> bgr24(static_cast<size_t>(12) * 6 * 3, 0x20);
    engine.ApplyCommand(kGpuCmdWireToSurface, kSurf, cmd, params.data(),
                        static_cast<uint32_t>(params.size()), bgr24.data(),
                        static_cast<uint32_t>(bgr24.size()));
    expectOk(engine.FindSurface(kSurf) != nullptr, "wire to surface keeps surface");
    ref0.FillRect(2, 30, 14, 36, 0xFF202020u);  // B=G=R=0x20, A forced 0xFF
    CheckSurface(&engine, &ref0, kSurf, "uncompressed 24bpp upload", &report);
  }

  // 5. Surface -> cache -> surface, including a clipped/off-surface destination.
  expectOk(engine.SurfaceToCache(kSurf, 7, 4, 4, 16, 16), "SurfaceToCache");
  // Probe: copy the slot onto (0,0), which is still the 0xFF initialisation, so
  // "did the cache actually receive the block?" is directly observable (it
  // separates a failing copy *into* the cache from a failing copy *out of* it).
  expectOk(engine.CacheToSurface(kSurf, 7, 0, 0), "CacheToSurface (probe)");
  ref0.CopyRect(ref0, 4, 4, 0, 0, 16, 16);
  CheckSurface(&engine, &ref0, kSurf, "cache probe to (0,0)", &report);
  expectOk(engine.CacheToSurface(kSurf, 7, -6, 70), "CacheToSurface (outside)");
  CheckSurface(&engine, &ref0, kSurf, "cache to surface (clipped away)", &report);
  expectOk(engine.CacheToSurface(kSurf, 7, 40, 20), "CacheToSurface");
  ref0.CopyRect(ref0, 4, 4, 40, 20, 16, 16);
  CheckSurface(&engine, &ref0, kSurf, "cache to surface", &report);

  // 6. Surface -> surface, self-overlapping (the temp-image staging path).
  expectOk(engine.SurfaceToSurface(kSurf, 4, 4, 16, 16, kSurf, 8, 8), "SurfaceToSurface");
  ref0.CopyRect(ref0, 4, 4, 8, 8, 16, 16);
  CheckSurface(&engine, &ref0, kSurf, "surface to surface (overlapping)", &report);

  // 7. Second surface mapped to the output, composed into the screen. The map
  //    comes first because MapSurfaceToOutput clears the invalid region (FreeRDP
  //    semantics), so a fill before the map would never be composed.
  {
    engine.MapSurfaceToOutput(kSurf2, 64, 32);
    const uint16_t fill1[] = {0, 0, 32, 32};
    expectOk(engine.SolidFill(kSurf2, kGreen, fill1, 1), "SolidFill (2)");
    ref1.FillRect(0, 0, 32, 32, kGreen);
    expectOk(engine.Compose(), "Compose");
    screen.CopyRect(ref1, 0, 0, 64, 32, 32, 32);
    engine.ClearScreenDirty();
    CheckScreen(&engine, screen, "compose mapped surface", &report);
  }

  // 8. Evicted cache slots must not resurrect content.
  engine.EvictCache(7);
  expectOk(!engine.CacheToSurface(kSurf, 7, 0, 0), "CacheToSurface (evicted)");
  CheckSurface(&engine, &ref0, kSurf, "cache to surface after evict", &report);

  // 9. Deleted surfaces disappear.
  engine.DeleteSurface(kSurf);
  report.steps++;
  if (engine.FindSurface(kSurf) != nullptr) {
    report.Fail("delete surface", 0, 0, 1, 0);
  }

  HMRDP_LOGI("vk selftest: %{public}s", engine.Stats().c_str());
  char head[256];
  std::snprintf(head, sizeof(head),
                "primitives: readback=%s steps=%d bad=%d apiFailures=%d compute=%s",
                bufferOk ? "ok" : "NONE(此设备无法读回，见 VULKAN-TODO 3.5)", report.steps,
                bufferOk ? report.bad : 0, apiFailures, computeOk ? "ok" : "FAIL");
  std::string line = head;
  if (!computeOk && !computeDetail.empty()) {
    line += " [" + computeDetail + "]";
  }
  if (!report.firstFail.empty()) {
    line += " firstFail: " + report.firstFail;
  }
  HMRDP_LOGI("vk selftest: %{public}s", line.c_str());
  return line;
}

GfxVkSelfTest& GfxVkSelfTest::Instance() {
  static GfxVkSelfTest instance;
  return instance;
}

GfxVkSelfTest::~GfxVkSelfTest() {
  Stop();
}

bool GfxVkSelfTest::Start(const std::string& gfxPath, void* nativeWindow, int surfaceW,
                          int surfaceH, bool present) {
  Stop();
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_.clear();
  }
  frames_.store(0);
  skipped_.store(0);
  checks_.store(0);
  bad_.store(0);
  rgbDiff_.store(0);
  alphaDiff_.store(0);
  maxDelta_.store(0);
  bboxX0_.store(-1);
  bboxY0_.store(-1);
  bboxX1_.store(-1);
  bboxY1_.store(-1);
  firstX_.store(-1);
  firstY_.store(-1);
  presents_.store(0);
  presentSkips_.store(0);
  feedUs_.store(0);
  present_ = present;
  running_.store(true);
  thread_ = std::thread([this, gfxPath, nativeWindow, surfaceW, surfaceH, present]() {
    Run(gfxPath, nativeWindow, surfaceW, surfaceH, present);
  });
  // Let the engine come up (or fail) before the caller reads the status.
  for (int i = 0; i < 200; ++i) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    if (!lastError_.empty()) {
      Stop();
      return false;
    }
    if (frames_.load() > 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return running_.load();
}

void GfxVkSelfTest::Run(const std::string& gfxPath, void* window, int surfaceW, int surfaceH,
                        bool present) {
  auto fail = [this](const std::string& why) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = why;
    HMRDP_LOGE("vk selftest: %{public}s", why.c_str());
  };

  // Deterministic primitive check first: it is the part of V1 that can actually
  // be verified on a real capture (see RunPrimitives).
  primitivesReport_ = RunPrimitives();

  if (surfaceW <= 0 || surfaceH <= 0) {
    surfaceW = 1280;
    surfaceH = 720;
  }

  // The gdi reference desktop: identical bytes, decoded by FreeRDP itself.
  cpu_ = std::make_unique<GfxCpuDesktop>();
  std::string error;
  if (!cpu_->Init(surfaceW, surfaceH, &error)) {
    fail("gdi desktop init failed: " + error);
    running_.store(false);
    return;
  }

  // Optional presentation: the engine image format must match the swapchain,
  // because a blit cannot convert channel order.
  VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
  if (present && window != nullptr) {
    renderer_ = std::make_unique<VkRenderer>();
    renderer_->SetSurface(window, surfaceW, surfaceH);
    if (!renderer_->Prepare()) {
      fail("vulkan surface/swapchain failed: " + renderer_->lastError());
      renderer_.reset();
      cpu_.reset();
      running_.store(false);
      return;
    }
    format = renderer_->format();
  } else {
    renderer_.reset();
  }

  engine_ = std::make_unique<GfxVkDesktop>();
  if (!engine_->Init(format)) {
    fail("vulkan engine init failed");
    engine_.reset();
    renderer_.reset();
    cpu_.reset();
    running_.store(false);
    return;
  }
  HMRDP_LOGI("vk selftest: engine up (format=%{public}u swapRb=%{public}d present=%{public}d)",
             static_cast<unsigned>(format), engine_->swapRb() ? 1 : 0, present ? 1 : 0);

  VkDeskSink sink(engine_.get());
  const int64_t pumpStart = NowUs();
  const bool ok = GfxReplayStreamCompare(
      gfxPath, &sink,
      [this]() {
        frames_.fetch_add(1);
        if (engine_ == nullptr) {
          return;
        }
        // Compose on every frame: ReadScreen (the comparison) reads the screen
        // image, which only Compose() writes. A static frame just returns false.
        const bool dirty = engine_->Compose();
        if (!dirty || renderer_ == nullptr) {
          return;
        }
        // Present directly instead of via GpuVkPresentComposed, which would call
        // Compose() a second time and then find nothing dirty.
        if (renderer_->PresentImage(engine_->screenImage(), engine_->format(),
                                    engine_->screenWidth(), engine_->screenHeight())) {
          presents_.fetch_add(1);
          engine_->ClearScreenDirty();
        } else {
          presentSkips_.fetch_add(1);
        }
      },
      cpu_->gfx(), [this]() { CompareFrames(); }, &running_, &error);
  feedUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));

  engineStats_ = engine_->Stats();
  if (!ok) {
    fail("replay failed: " + error);
  }
  HMRDP_LOGI("vk selftest: finished: %{public}s", Stats().c_str());
  engine_.reset();
  renderer_.reset();
  cpu_.reset();
  running_.store(false);
}

void GfxVkSelfTest::CompareFrames() {
  GfxVkDesktop* engine = engine_.get();
  GfxCpuDesktop* cpu = cpu_.get();
  if (engine == nullptr || cpu == nullptr) {
    return;
  }
  if (!engine->readbackAvailable()) {
    // Comparing would need a readback that this device cannot do.
    readbackMissing_.store(true);
    return;
  }
  rdpGdi* gdi = cpu->gdi();
  const int w = engine->screenWidth();
  const int h = engine->screenHeight();
  // RDPGFX surfaces are persistent: as soon as one command the V1 engine does
  // not implement is applied (progressive / ClearCodec), the surface state and
  // gdi's have diverged for good, so neither this frame nor any later one can be
  // judged. The comparison still runs (it costs one readback) and its numbers are
  // reported, but `tainted` marks them as not evidence.
  if (engine->unsupportedSeen()) {
    engine->resetUnsupportedSeen();
    skipped_.fetch_add(1);
    tainted_.store(true);
    return;
  }
  // Only clean frames are comparable, and they are rare in a progressive/clear
  // heavy capture, so rate-limit by time instead of sampling every Nth frame:
  // every clean frame is checked, but a full-screen readback is expensive.
  const int64_t now = NowUs();
  if (now - lastCompareUs_ < kCompareMinGapUs) {
    return;
  }
  if (gdi == nullptr || gdi->primary_buffer == nullptr || w <= 0 || h <= 0) {
    return;
  }
  const int cmpW = w < static_cast<int>(gdi->width) ? w : static_cast<int>(gdi->width);
  const int cmpH = h < static_cast<int>(gdi->height) ? h : static_cast<int>(gdi->height);
  if (cmpW <= 0 || cmpH <= 0) {
    return;
  }
  lastCompareUs_ = now;
  std::vector<uint8_t> screen;
  if (!engine->ReadScreen(&screen)) {
    return;
  }
  if (screen.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
    return;
  }

  size_t diffRgb = 0;
  size_t diffAlpha = 0;
  int firstX = -1;
  int firstY = -1;
  int bx0 = cmpW;
  int by0 = cmpH;
  int bx1 = -1;
  int by1 = -1;
  int maxDelta = 0;
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
        diffRgb++;
      } else if (pa[3] != pb[3]) {
        diffAlpha++;
      }
    }
  }
  checks_.fetch_add(1);
  if (firstX >= 0) {
    firstX_.store(firstX);
    firstY_.store(firstY);
    firstEngine_.store(firstEngine);
    firstGdi_.store(firstGdi);
    HMRDP_LOGW("vk selftest: first diff (%d,%d) engine=0x%08x gdi=0x%08x", firstX, firstY,
               static_cast<unsigned>(firstEngine), static_cast<unsigned>(firstGdi));
  }
  if (bx1 >= 0) {
    bboxX0_.store(bx0);
    bboxY0_.store(by0);
    bboxX1_.store(bx1);
    bboxY1_.store(by1);
    const int prev = maxDelta_.load();
    if (maxDelta > prev) {
      maxDelta_.store(maxDelta);
    }
  }
  rgbDiff_.fetch_add(diffRgb);
  alphaDiff_.fetch_add(diffAlpha);
  if (diffRgb != 0) {
    bad_.fetch_add(1);
  }
}

void GfxVkSelfTest::Resize(int width, int height) {
  pendingW_.store(width);
  pendingH_.store(height);
  if (renderer_ != nullptr) {
    renderer_->ResizeSurface(width, height);
  }
}

void GfxVkSelfTest::Stop() {
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::string GfxVkSelfTest::Stats() {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "vk selftest: frames=%llu checks=%llu bad=%llu rgbPx=%llu alphaPx=%llu "
                "skipped=%llu tainted=%llu presents=%llu feed=%llums",
                static_cast<unsigned long long>(frames_.load()),
                static_cast<unsigned long long>(checks_.load()),
                static_cast<unsigned long long>(bad_.load()),
                static_cast<unsigned long long>(rgbDiff_.load()),
                static_cast<unsigned long long>(alphaDiff_.load()),
                static_cast<unsigned long long>(skipped_.load()),
                static_cast<unsigned long long>(tainted_.load()),
                static_cast<unsigned long long>(presents_.load()),
                static_cast<unsigned long long>(feedUs_.load() / 1000));
  return std::string(buf);
}

std::string GfxVkSelfTest::StatsLines() {
  std::string out = primitivesReport_ + "\n" + Stats() + "\n";
  if (tainted_.load() != 0) {
    out += "INVALID: the capture contains progressive/clearCodec, which V1 does "
           "not implement, so the engine and gdi diverged and every later frame "
           "is incomparable (V2/V3, or a capture made of only fill/copy/uncompressed)\n";
  }
  char buf[256];
  const int x0 = bboxX0_.load();
  const int y0 = bboxY0_.load();
  const int x1 = bboxX1_.load();
  const int y1 = bboxY1_.load();
  if (x1 >= 0) {
    std::snprintf(buf, sizeof(buf), "maxDelta=%llu bbox=(%d,%d)-(%d,%d) first=(%d,%d)\n",
                  static_cast<unsigned long long>(maxDelta_.load()), x0, y0, x1, y1,
                  firstX_.load(), firstY_.load());
  } else {
    std::snprintf(buf, sizeof(buf), "maxDelta=0 no diff\n");
  }
  out += buf;
  if (firstX_.load() >= 0) {
    std::snprintf(buf, sizeof(buf), "engine=0x%08llx gdi=0x%08llx\n",
                  static_cast<unsigned long long>(firstEngine_.load()),
                  static_cast<unsigned long long>(firstGdi_.load()));
    out += buf;
  }
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    if (!lastError_.empty()) {
      out += "error: " + lastError_ + "\n";
    }
  }
  if (engine_ != nullptr) {
    out += engine_->Stats() + "\n";
  } else if (!engineStats_.empty()) {
    out += engineStats_ + "\n";
  }
  if (readbackMissing_.load()) {
    out += "像素对比已停用：本设备无法把 GPU 写入读回 CPU（VULKAN-TODO 3.5），"
           "本轮不产出 bad 结论\n";
  }
  out += "note: frames carrying progressive/clear are skipped (V2/V3)";
  return out;
}

}  // namespace hmrdp
