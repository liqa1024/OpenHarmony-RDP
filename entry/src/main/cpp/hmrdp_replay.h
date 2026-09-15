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
#include <array>
#include <cstdint>
#include <map>
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

  // --- Dev: independent CPU reference surface (VULKAN-TODO §8) -------------
  // The compare routes keep a *complete* second implementation of the GFX
  // surface in the harness: FreeRDP's own progressive/clear decoders plus
  // mirrored gdi cache/fill/copy semantics are applied to `refSurface_`, which
  // never reads the engine's surface. CompareFrames then diffs the engine's
  // whole surface against it, so any difference is an absolute end-to-end proof
  // of an engine divergence - the first mismatching pixel and the last command
  // that wrote it name the culprit. No-ops on the other routes.
  void RefCreateSurface(uint16_t surfaceId, int width, int height);
  void RefDeleteSurface(uint16_t surfaceId);
  void RefResetGraphics();
  void RefProgressive(uint16_t surfaceId, const uint8_t* payload, size_t size, int left, int top);
  void RefClearCodec(uint16_t surfaceId, const uint8_t* payload, size_t size, int left, int top,
                     int width, int height);
  void RefCacheStore(uint16_t surfaceId, uint16_t slot, int x, int y, int width, int height);
  void RefCacheRestore(uint16_t surfaceId, uint16_t slot, const uint8_t* pts, uint32_t count);
  void RefCacheEvict(uint16_t slot);
  void RefFill(uint16_t surfaceId, uint32_t pixel, const uint8_t* rects, uint32_t count);
  // Uncompressed bitmap upload (gdi_SurfaceCommand_Uncompressed): 24bpp is
  // expanded to BGRA with alpha 0xFF, 32bpp is copied, both clipped to the
  // surface.
  void RefUpload(uint16_t surfaceId, uint32_t format, int left, int top, int width, int height,
                 const uint8_t* payload, uint32_t payloadLen);
  void RefCopy(uint16_t srcSurfaceId, uint16_t dstSurfaceId, const uint8_t* params, uint32_t count);
  // Diff of the engine's surface against the reference (called per compare).
  void RefCompareSurfaces();
  // Absolute per-command check: right after a command was applied to both the
  // engine and the reference, compare only the rect it wrote. The first failure
  // therefore names the exact culprit command (this is what the per-frame compare
  // cannot do - `update_tiles` re-composites earlier tiles and masks the writer).
  bool RefVerifyRect(uint16_t surfaceId, int x, int y, int width, int height, const char* op);
  // Dev A/B against FreeRDP's own gdi surface for the same surfaceId - the
  // authoritative reference (the mirrored surface above is a hand-written second
  // implementation and has produced false positives). `GdiAbCheck` only records
  // the rect (gdi is fed one chunk behind the sink); `GdiAbFlush` performs the
  // comparison at the chunk boundary, when both sides are at the same position.
  void GdiAbCheck(uint16_t surfaceId, int x, int y, int width, int height, const char* op,
                  const std::string& detail = std::string(), int payloadSlot = -1);
  void GdiAbFlush();
  // Dev: coefficient-level A/B of one Progressive tile's decoder state against
  // FreeRDP's own (patch-freerdp.ps1 step 8).
  void GdiAbCompareState(uint16_t surfaceId, int xIdx, int yIdx);
  // Dev: the RDPGFX frame id gdi is using (forwarded through the StartFrame chain
  // command), so the mirrored reference decodes with the same frame boundary rule.
  void SetMirrorFrameId(uint32_t frameId) { mirrorFrameId_.store(frameId); }
  // Dev: copy one command payload into the ring and return its slot (the chunk
  // buffer is reused by the next chunk, so a raw pointer would be stale by the
  // time the deferred check runs).
  int GdiAbStashPayload(const uint8_t* payload, uint32_t payloadLen);
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

  // Dev: CPU reference surface state (replay thread only). The two contexts are
  // kept as void* so this header stays FreeRDP-free.
  struct RefCacheEntry {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
  };
  void* refProg_ = nullptr;   // PROGRESSIVE_CONTEXT*
  void* refClear_ = nullptr;  // CLEAR_CONTEXT*
  std::vector<uint8_t> refSurface_;
  uint16_t refSurfaceId_ = 0xFFFFu;
  int refW_ = 0;
  int refH_ = 0;
  int refStride_ = 0;
  std::map<uint16_t, RefCacheEntry> refCache_;
  // Per-pixel "which command class last modified this pixel" (1 = progressive,
  // 2 = clearcodec, 3 = cache restore, 4 = fill, 5 = copy). Dev only: it names
  // the writer of the first diverging pixel.
  std::vector<uint8_t> refProv_;
  // Atomic: StatsLines() (UI thread) reports the summary while the replay runs.
  std::atomic<uint64_t> refCommands_{0};
  std::atomic<uint64_t> refChecks_{0};
  std::atomic<uint64_t> refBad_{0};
  std::atomic<uint64_t> refBadPx_{0};
  bool refFirstLogged_ = false;
  // The command class that first failed its own per-command verification.
  std::string refBadOp_;
  // Dev: how many cache restores found no entry (diagnostic for the harness).
  uint32_t refCacheMisses_ = 0;
  // First mismatching pixel of the per-command verification (so the Progressive
  // path can report which tile - and which decode sub-path - is responsible).
  int refBadX_ = -1;
  int refBadY_ = -1;
  bool refBadThisMessage_ = false;
  // Dev: pre-decode verification count (bounded so the log stays readable).
  uint64_t refPreChecks_ = 0;
  // Dev: log messages touching tile (0,0) with their tile-kind breakdown.
  bool refWatchTile_ = false;
  // Dev: watch pixel for the per-command write trace.
  int refWatchX_ = 1408;
  int refWatchY_ = 1260;
  uint32_t refWatchLogged_ = 0;
  uint32_t refTileLogged_ = 0;
  // Dev: how many reference-decode failures have been logged.
  uint32_t refRcLogged_ = 0;
  uint32_t refClearRcLogged_ = 0;

  // Dev: authoritative per-command A/B against gdi's own surface. Rects are
  // recorded per command and verified at the next chunk boundary (gdi is fed one
  // chunk behind the engine).
  struct GdiAbRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    uint16_t surfaceId = 0;
    std::string op;
    // Dev: Progressive tile descriptor (empty for the other command classes).
    std::string detail;
    // Index into `gdiAbPayloads_` (the command payload is only valid during the
    // call - the chunk buffer is reused by the next chunk - so it is copied).
    int payloadSlot = -1;
  };
  std::vector<GdiAbRect> gdiAbPending_;
  // Dev: one-shot dump of a divergent tile's decoder inputs/result.
  bool tileDumpDone_ = false;
  // Dev: a small ring of recent command payloads. The harness's chunk buffer is
  // reused as soon as the next chunk is fed, and the deferred check runs at that
  // point, so a raw pointer would already be stale.
  std::vector<std::vector<uint8_t>> gdiAbPayloads_;
  size_t gdiAbPayloadNext_ = 0;
  std::atomic<uint32_t> mirrorFrameId_{0};
  std::atomic<uint64_t> gdiChecks_{0};
  std::atomic<uint64_t> gdiBad_{0};
  std::atomic<uint64_t> gdiBadPx_{0};
  bool gdiFirstLogged_ = false;
  std::string gdiBadOp_;
  // Dev: reconstructed per-(surface,tileStream) bit positions, so a Progressive
  // tile's `numBits` (old - new) can be reported next to the message's own
  // quantisation data.
  std::map<uint64_t, std::array<uint8_t, 10>> tileBitPos_;
  // Dev: watch pixel for the gdi write trace (defaults to the first divergence
  // the previous runs reported).
  int gdiWatchX_ = 1408;
  int gdiWatchY_ = 1260;
  uint32_t gdiWatchLogged_ = 0;

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
