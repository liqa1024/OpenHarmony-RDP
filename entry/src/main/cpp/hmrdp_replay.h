/*
 * HmRdp - dev-only recorded-RDP replay (PERF-TODO §4).
 *
 * Replays a captured raw GFX channel stream (hmrdp_gfx.bin) through FreeRDP's
 * own ZGX + RDPGFX parsing into the GPU desktop engine and presents each frame
 * to the XComponent surface. Debug facility only; never replaces the gdi path.
 */
#ifndef HMRDP_REPLAY_H
#define HMRDP_REPLAY_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hmrdp {

class Renderer;
class GfxGpuDesktop;
class GfxCpuDesktop;

// Which decoder the replay runs.
//  kGpu     - GPU desktop engine only (perf measurement).
//  kCpu     - FreeRDP's own gdi pipeline only (perf reference).
//  kCompare - both fed the same capture simultaneously, with a per-frame pixel
//             comparison (correctness verification). Perf numbers are not
//             meaningful in this mode.
enum class GfxReplayRoute { kGpu = 0, kCpu = 1, kCompare = 2 };

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

  // ClearCodec batch granularity for the next/current GPU replay: maximum union
  // rectangle area (pixels) a queued run may cover. 0 = one command per flush.
  // Applies on the next Start() (the page restarts the replay when it changes).
  void SetClearBatchArea(int pixels);
  int ClearBatchArea() const;

  // Called by the replay GFX callbacks on every EndFrame (replay thread).
  void OnReplayFrame();
  // Called from the offline gdi EndPaint hook (CPU route, replay thread).
  void OnCpuFrame(GfxCpuDesktop* cpu);

  // Dev timing: accumulated decode-apply / present time (microseconds), reported
  // per frame by Stats() so the GPU and CPU routes can be compared directly.
  // `codecId` (from the surface command) splits the apply time by codec so a
  // slow stream (e.g. ClearCodec's CPU read-modify-write) is visible.
  void RecordApply(uint16_t cmdId, uint32_t surfaceId, uint32_t codecId, uint64_t micros);
  // Time spent inside a ClearCodec flush that a command triggered. Kept out of
  // the per-class figures so "prog"/"fill" report their own cost.
  void RecordFlush(uint64_t micros);
  void RecordPresent(uint64_t micros);

 private:
  GfxReplay() = default;
  ~GfxReplay();
  GfxReplay(const GfxReplay&) = delete;
  GfxReplay& operator=(const GfxReplay&) = delete;

  void Run();
  void RunGpuReplay(const std::string& gfxPath);
  void RunCpuReplay(const std::string& gfxPath);
  // Compare route: GPU engine + offline gdi desktop fed the same stream, with a
  // sampled per-frame pixel comparison (engine screen vs gdi primary buffer).
  void RunCompareReplay(const std::string& gfxPath);
  void CompareFrames();
  // Throttles playback to ~60 Hz, but only for frames that actually produced a
  // picture (see the implementation for why empty frame markers must not pace).
  void PaceFrame(int64_t frameStartUs, bool presented);

  std::mutex mutex_;
  std::unique_ptr<Renderer> renderer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  void* window_ = nullptr;
  std::string gfxPath_;
  std::atomic<int> route_{0};
  int surfaceW_ = 0;
  int surfaceH_ = 0;
  std::atomic<int> pendingW_{0};
  std::atomic<int> pendingH_{0};
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
  // ClearCodec run statistics: how many ClearCodec commands in a row target the
  // same surface (they could share one GPU map instead of one each).
  std::atomic<uint64_t> clearRunSum_{0};
  std::atomic<uint64_t> clearRunCount_{0};
  std::atomic<uint64_t> clearRunMax_{0};
  // Replay-thread-only current-run bookkeeping (RecordApply is only called from
  // the pump thread).
  uint32_t clearRunLen_ = 0;
  uint32_t clearRunSurface_ = 0xFFFFFFFFu;
  bool clearRunActive_ = false;
  std::atomic<uint64_t> flushUs_{0};
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

  // Replay-thread only (no locking needed).
  GfxGpuDesktop* engine_ = nullptr;
  GfxCpuDesktop* cpuDesktop_ = nullptr;
  std::mutex errorMutex_;
  std::string lastError_;
  // Latest ClearCodec traffic summary from the engine (snapshotted periodically
  // on the replay thread, read by StatsLines on the UI thread).
  std::string traffic_;
};

}  // namespace hmrdp

#endif  // HMRDP_REPLAY_H
