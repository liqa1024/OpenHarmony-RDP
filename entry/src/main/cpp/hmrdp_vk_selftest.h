/*
 * HmRdp - Vulkan engine correctness harness (VULKAN-TODO.md §5 V1).
 *
 * Replays one raw GFX capture (hmrdp_gfx.bin) through the Vulkan surface engine
 * and, simultaneously, through FreeRDP's own gdi pipeline - the same
 * per-chunk-feed + sampled pixel comparison the replay page's "compare" route
 * uses, so the acceptance signal (`bad=0`) is produced by an existing, trusted
 * harness rather than a new one.
 *
 * Frames containing a command V1 does not implement yet (Progressive /
 * ClearCodec, i.e. V2/V3) are excluded from the comparison and counted
 * separately, so the V1 claim stays honest: "fill + copy + uncompressed is
 * pixel-exact", no more.
 *
 * Presenting is optional: with an XComponent surface the engine screen is
 * blitted to the swapchain, which is how the on-device visual check runs.
 */
#ifndef HMRDP_VK_SELFTEST_H
#define HMRDP_VK_SELFTEST_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hmrdp_vk_desktop.h"

namespace hmrdp {

class GfxCpuDesktop;
class VkRenderer;

class GfxVkSelfTest {
 public:
  static GfxVkSelfTest& Instance();

  // Starts the run on a worker thread. `nativeWindow` may be null (headless
  // compare only). Blocks until the run has started or failed.
  bool Start(const std::string& gfxPath, void* nativeWindow, int surfaceW, int surfaceH,
             bool present);
  void Resize(int width, int height);
  void Stop();
  // Single-line summary (logs) and multi-line form for the dev panel.
  std::string Stats();
  std::string StatsLines();
  // Deterministic primitive self-test (fill / upload / cache / copy / compose
  // against a CPU model). The capture comparison cannot validate V1 because an
  // RDPGFX capture contains commands V1 does not implement yet.
  std::string RunPrimitives();

 private:
  GfxVkSelfTest() = default;
  ~GfxVkSelfTest();
  GfxVkSelfTest(const GfxVkSelfTest&) = delete;
  GfxVkSelfTest& operator=(const GfxVkSelfTest&) = delete;

  void Run(const std::string& gfxPath, void* window, int surfaceW, int surfaceH, bool present);
  // Called on the replay thread once both decoders consumed one chunk that ended
  // a frame (both are at the same stream position and have composed).
  void CompareFrames();

  std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<int> pendingW_{0};
  std::atomic<int> pendingH_{0};

  std::atomic<uint64_t> frames_{0};
  // Frames skipped because they carried an unimplemented command.
  std::atomic<uint64_t> skipped_{0};
  // Set once a command V1 does not implement was applied: the run's pixel
  // comparison is then no longer evidence (persistent surfaces).
  std::atomic<uint64_t> tainted_{0};
  std::atomic<uint64_t> checks_{0};
  std::atomic<uint64_t> bad_{0};
  std::atomic<uint64_t> rgbDiff_{0};
  std::atomic<uint64_t> alphaDiff_{0};
  std::atomic<uint64_t> maxDelta_{0};
  std::atomic<int> bboxX0_{-1};
  std::atomic<int> bboxY0_{-1};
  std::atomic<int> bboxX1_{-1};
  std::atomic<int> bboxY1_{-1};
  std::atomic<int> firstX_{-1};
  std::atomic<int> firstY_{-1};
  std::atomic<uint64_t> firstEngine_{0};
  std::atomic<uint64_t> firstGdi_{0};
  std::atomic<uint64_t> presents_{0};
  std::atomic<uint64_t> presentSkips_{0};
  std::atomic<uint64_t> feedUs_{0};

  // Replay-thread only.
  std::string primitivesReport_;
  std::unique_ptr<GfxVkDesktop> engine_;
  std::unique_ptr<GfxCpuDesktop> cpu_;
  std::unique_ptr<VkRenderer> renderer_;
  bool present_ = false;
  int64_t lastCompareUs_ = 0;
  std::string engineStats_;

  std::mutex errorMutex_;
  std::string lastError_;
};

}  // namespace hmrdp

#endif  // HMRDP_VK_SELFTEST_H
