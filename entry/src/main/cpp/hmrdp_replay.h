/*
 * HmRdp - dev-only recorded-RDP replay (doc_agent/gfx-engine.md §6).
 *
 * Replays a captured raw GFX channel stream (hmrdp_gfx.bin) through FreeRDP's
 * own ZGX + RDPGFX parsing into the Vulkan desktop engine and presents each
 * frame to the XComponent surface. Debug facility only; never replaces the live
 * path.
 */
#ifndef HMRDP_REPLAY_H
#define HMRDP_REPLAY_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hmrdp {

class FramePresenter;
class GfxCpuDesktop;
class ReplayDesktop;

// Which decoder/presenter the replay runs.
//  kCpu           - FreeRDP's own gdi pipeline only (perf reference), presented
//                   through the Vulkan presenter's CPU frame path.
//  kVulkan        - Vulkan desktop engine only.
//  kVulkanCompare - same engine, with an offline gdi desktop fed the same
//                   capture simultaneously and compared per frame (correctness
//                   verification). Perf numbers are not meaningful here.
enum class GfxReplayRoute {
  kCpu = 0,
  kVulkan = 1,
  kVulkanCompare = 2,
};

class GfxReplay {
 public:
  static GfxReplay& Instance();

  // Takes ownership of `nativeWindow` (from the XComponent surface id) and
  // replays `gfxPath` (a raw hmrdp_gfx.bin capture) onto it through `route`.
  // Waits briefly for the first frame or failure.
  bool Start(void* nativeWindow, int surfaceW, int surfaceH, const std::string& gfxPath,
             GfxReplayRoute route);
  void Resize(int width, int height);
  void Stop();
  // Single-line summary (logs) and a multi-line variant for the on-device
  // performance panel on the replay page.
  std::string Stats();
  std::string StatsLines();

  // Called by the replay GFX callbacks on every EndFrame (replay thread).
  void OnReplayFrame();
  // Called from the offline gdi EndPaint hook (CPU route, replay thread).
  void OnCpuFrame(GfxCpuDesktop* cpu);

  // Dev timing: accumulated decode-apply / present time (microseconds), reported
  // per frame by Stats() so the engine and CPU routes can be compared directly.
  // `codecId` (from the surface command) splits the apply time by codec so a
  // slow stream is visible.
  void RecordApply(uint16_t cmdId, uint32_t codecId, uint64_t micros);
  void RecordPresent(uint64_t micros);

  // --- Dev: per-command A/B against FreeRDP's own gdi surface ---------------
  // Only active with `kCodecAbEnabled` (see hmrdp_replay.cpp): the sink records
  // the rects each command claims to write and `GdiAbFlush` - called by the
  // compare route right after every command, with gdi and the engine at the same
  // stream position - diffs exactly those pixels against gdi's own surface. An
  // earlier hand-written mirror of the gdi geometry was removed: it produced
  // false positives, and gdi itself is the only trustworthy reference.
  void GdiAbCheck(uint16_t surfaceId, int x, int y, int width, int height, const char* op);
  void GdiAbFlush();
  std::string GdiAbSummary() const;
  // Dev: if this command's rect covers the watch pixel, log who wrote it and both
  // values - so the command that moved only the engine's surface is visible.
  void RefWatchRect(uint16_t surfaceId, int x, int y, int width, int height, const char* op);
  std::string RefAbSummary() const;

 private:
  GfxReplay() = default;
  ~GfxReplay();
  GfxReplay(const GfxReplay&) = delete;
  GfxReplay& operator=(const GfxReplay&) = delete;

  void Run();
  // Vulkan-desktop-engine route. `compare` additionally feeds the capture into an
  // offline gdi desktop and compares the two screens per frame.
  void RunVulkanReplay(const std::string& gfxPath, bool compare);
  void RunCpuReplay(const std::string& gfxPath);
  void CompareFrames();
  // Throttles playback to ~60 Hz, but only for frames that actually produced a
  // picture (see the implementation for why empty frame markers must not pace).
  void PaceFrame(int64_t frameStartUs, bool presented);

  std::mutex mutex_;
  // Presenter for the CPU (gdi) route (Vulkan, or GLES on devices whose Vulkan
  // cannot present); the engine route owns its own Vulkan presenter inside
  // `desktop_`.
  std::unique_ptr<FramePresenter> presenter_;
  // Engine adapter for the Vulkan routes (null on the CPU route).
  std::unique_ptr<ReplayDesktop> desktop_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  void* window_ = nullptr;
  std::string gfxPath_;
  std::atomic<int> route_{0};
  int surfaceW_ = 0;
  int surfaceH_ = 0;
  std::atomic<int> pendingW_{0};  std::atomic<int> pendingH_{0};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> presents_{0};
  // EndFrame markers that produced no present because the engine had nothing
  // dirty mapped to the output (normal "static frame"; the GPU route must not
  // count these as failures).
  std::atomic<uint64_t> presentSkips_{0};
  // EndFrame markers where a present was attempted and did not reach the screen
  // (window/surface not ready, GL error) - this is the real failure counter.
  std::atomic<uint64_t> presentFailures_{0};
  std::atomic<uint64_t> applyUs_{0};
  std::atomic<uint64_t> applyCount_{0};
  // Per-command apply time/count (GPU route), split by GFX command kind so a
  // single slow group is visible: Progressive / ClearCodec / uncompressed
  // bitmap (WireToSurface), solid fill, surface blit, cache, and the rest
  // (surface lifecycle / mapping / frame markers).
  std::atomic<uint64_t> progUs_{0};
  std::atomic<uint64_t> progCount_{0};
  std::atomic<uint64_t> clearUs_{0};
  std::atomic<uint64_t> clearCount_{0};
  std::atomic<uint64_t> uncompUs_{0};
  std::atomic<uint64_t> uncompCount_{0};
  std::atomic<uint64_t> fillUs_{0};
  std::atomic<uint64_t> fillCount_{0};
  std::atomic<uint64_t> blitUs_{0};
  std::atomic<uint64_t> blitCount_{0};
  std::atomic<uint64_t> cacheUs_{0};
  std::atomic<uint64_t> cacheCount_{0};
  std::atomic<uint64_t> otherUs_{0};
  std::atomic<uint64_t> otherCount_{0};
  std::atomic<uint64_t> presentUs_{0};
  std::atomic<uint64_t> pumpUs_{0};
  // Time spent deliberately sleeping in PaceFrame(); subtracted from pumpUs_ so
  // the reported feed cost is compute, not playback throttling.
  std::atomic<uint64_t> paceUs_{0};
  std::atomic<int64_t> startUs_{0};
  // Set when the replay loop ends, so Stats() keeps reporting the run's last
  // figures instead of letting fps/feed decay while the page sits idle.
  std::atomic<int64_t> endUs_{0};
  // Compare-route counters (engine vs gdi, sampled per frame).
  std::atomic<uint64_t> cmpChecks_{0};
  std::atomic<uint64_t> cmpBad_{0};
  std::atomic<uint64_t> cmpMaxDiff_{0};
  std::atomic<uint64_t> cmpRgbDiff_{0};
  std::atomic<uint64_t> cmpAlphaDiff_{0};
  // Diagnostics for the last mismatching frame: difference bounding box and the
  // largest per-channel delta (tells "unpainted rectangle" from "rounding").
  std::atomic<int> cmpBBoxX0_{-1};
  std::atomic<int> cmpBBoxY0_{-1};
  std::atomic<int> cmpBBoxX1_{-1};
  std::atomic<int> cmpBBoxY1_{-1};
  std::atomic<int> cmpMaxDelta_{0};
  // Pixels whose worst channel delta is <= 2 (rounding-level, not content).
  std::atomic<uint64_t> cmpSmallDeltaPx_{0};
  std::atomic<int> cmpFirstX_{-1};
  std::atomic<int> cmpFirstY_{-1};
  // Dev diagnosis: the one-shot per-pixel dump has already been written.
  bool cmpDumpDone_ = false;

  // Dev: per-command A/B against gdi's own surface (kCodecAbEnabled only).
  struct GdiAbRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    uint16_t surfaceId = 0;
    std::string op;
  };
  std::vector<GdiAbRect> gdiAbPending_;
  std::atomic<uint64_t> gdiChecks_{0};
  std::atomic<uint64_t> gdiBad_{0};
  std::atomic<uint64_t> gdiBadPx_{0};
  bool gdiFirstLogged_ = false;
  std::string gdiBadOp_;
  // Dev: the first culprit line, repeated at the end of the run (hilog rotates).
  std::string gdiFirstLine_;

  // Replay-thread only (no locking needed).
  GfxCpuDesktop* cpuDesktop_ = nullptr;
  std::mutex errorMutex_;
  std::string lastError_;
  // Latest engine summary (Vulkan stats), snapshotted periodically on the replay
  // thread and read by StatsLines on the UI thread.
  std::string traffic_;
};

}  // namespace hmrdp

#endif  // HMRDP_REPLAY_H
