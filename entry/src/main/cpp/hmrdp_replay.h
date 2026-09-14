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
#include <vector>

namespace hmrdp {

class Renderer;
class GfxCpuDesktop;
class ReplayDesktop;

// Which decoder/presenter the replay runs.
//  kCpu           - FreeRDP's own gdi pipeline only (perf reference).
//  kGles          - GLES desktop engine only (legacy; frozen, removed in V7).
//  kVulkan        - Vulkan desktop engine only.
//  kGlesCompare   - GLES engine + gdi fed the same capture simultaneously, with
//                   a per-frame pixel comparison (correctness verification).
//  kVulkanCompare - same comparison with the Vulkan engine.
// Perf numbers are not meaningful on the compare routes.
enum class GfxReplayRoute {
  kCpu = 0,
  kGles = 1,
  kVulkan = 2,
  kGlesCompare = 3,
  kVulkanCompare = 4,
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

  // ClearCodec batch granularity for the next/current GLES replay: maximum union
  // rectangle area (pixels) a queued run may cover. 0 = one command per flush.
  // Applies on the next Start() (the page restarts the replay when it changes).
  // The Vulkan engine decodes ClearCodec directly on the mapped surface, so it
  // ignores this knob.
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

  // --- Dev: Progressive per-message A/B (VULKAN-TODO §8) -------------------
  // The compare routes additionally run every Progressive payload through
  // FreeRDP's own progressive_decompress into a shadow surface and compare the
  // tiles it writes against the engine's surface. FreeRDP's decoder output
  // depends only on the progressive stream (its per-tile `current`/`sign`/
  // bit-state), never on the surface content, so a mismatch is unambiguous proof
  // of an engine-side decode divergence - and it names the message and tile.
  // No-ops on the other routes.
  void ProgAbCreateSurface(uint16_t surfaceId, int width, int height);
  void ProgAbDeleteSurface(uint16_t surfaceId);
  void ProgAbMessage(uint16_t surfaceId, const uint8_t* payload, size_t size, int left, int top);
  std::string ProgAbSummary() const;

  // ClearCodec half of the A/B: each band is decoded a second time by a stock
  // FreeRDP CLEAR_CONTEXT *on the engine's own surface content* (the band rect is
  // copied over first), so the only possible difference is the ClearCodec decoder
  // itself (context state / arguments) - not the input pixels.
  void ClearAbBand(uint16_t surfaceId, const uint8_t* payload, size_t size, int left, int top,
                   int width, int height);
  // Mirrors gdi's freerdp_client_codecs_reset() -> clear_context_reset().
  void ClearAbResetGraphics();
  std::string ClearAbSummary() const;

 private:
  GfxReplay() = default;
  ~GfxReplay();
  GfxReplay(const GfxReplay&) = delete;
  GfxReplay& operator=(const GfxReplay&) = delete;

  void Run();
  // Desktop-engine routes (GLES or Vulkan). `vulkan` picks the engine and
  // `compare` additionally feeds the capture into an offline gdi desktop and
  // compares the two screens per frame.
  void RunDesktopReplay(const std::string& gfxPath, bool vulkan, bool compare);
  void RunCpuReplay(const std::string& gfxPath);
  void CompareFrames();
  // Throttles playback to ~60 Hz, but only for frames that actually produced a
  // picture (see the implementation for why empty frame markers must not pace).
  void PaceFrame(int64_t frameStartUs, bool presented);

  std::mutex mutex_;
  // GLES presenter for the pure CPU (gdi) route; the desktop-engine routes own
  // their own presenter inside `desktop_`.
  std::unique_ptr<Renderer> renderer_;
  // Engine adapter for the GLES/Vulkan routes (null on the CPU route).
  std::unique_ptr<ReplayDesktop> desktop_;
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
  // Dev diagnosis: the one-shot per-pixel dump has already been written.
  bool cmpDumpDone_ = false;

  // Dev: Progressive per-message A/B state (replay thread only). `progAb` is a
  // PROGRESSIVE_CONTEXT* kept as void* so this header stays FreeRDP-free.
  void* progAb_ = nullptr;
  std::vector<uint8_t> progAbSurface_;
  uint16_t progAbSurfaceId_ = 0xFFFFu;
  int progAbW_ = 0;
  int progAbH_ = 0;
  int progAbStride_ = 0;
  // Atomic: StatsLines() (UI thread) reports the summary while the replay runs.
  std::atomic<uint64_t> progAbMessages_{0};
  std::atomic<uint64_t> progAbBadMessages_{0};
  std::atomic<uint64_t> progAbBadTiles_{0};
  std::atomic<uint64_t> progAbBadPx_{0};
  bool progAbFirstLogged_ = false;

  // Dev: ClearCodec A/B state (replay thread only; `clearAb` is a CLEAR_CONTEXT*).
  void* clearAb_ = nullptr;
  std::vector<uint8_t> clearAbSurface_;
  uint16_t clearAbSurfaceId_ = 0xFFFFu;
  int clearAbW_ = 0;
  int clearAbH_ = 0;
  int clearAbStride_ = 0;
  std::atomic<uint64_t> clearAbBands_{0};
  std::atomic<uint64_t> clearAbBadBands_{0};
  std::atomic<uint64_t> clearAbBadPx_{0};
  bool clearAbFirstLogged_ = false;

  // Replay-thread only (no locking needed).
  GfxCpuDesktop* cpuDesktop_ = nullptr;
  std::mutex errorMutex_;
  std::string lastError_;
  // Latest engine summary (GLES ClearCodec traffic / Vulkan stats), snapshotted
  // periodically on the replay thread and read by StatsLines on the UI thread.
  std::string traffic_;
};

}  // namespace hmrdp

#endif  // HMRDP_REPLAY_H
