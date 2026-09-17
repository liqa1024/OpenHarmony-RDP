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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_gfx_work.h"

namespace hmrdp {

class FramePresenter;
class GfxCpuDesktop;
class ReplayDesktop;

// Which decoder/presenter the replay runs.
//  kCpu           - FreeRDP's own gdi pipeline only (perf reference), presented
//                   through the presenter the "硬件加速" setting selects.
//  kVulkan        - Vulkan desktop engine only. **Not reachable from the UI any
//                   more** (the engine routes were dropped while the GPU work is
//                   being redone, doc_agent/gpu-accel-plan.md); kept so the
//                   reference implementation stays runnable from code.
//  kVulkanCompare - same engine, with an offline gdi desktop fed the same
//                   capture simultaneously and compared per frame. Same status:
//                   kept for reference, no longer a UI route.
enum class GfxReplayRoute {
  kCpu = 0,
  kVulkan = 1,
  kVulkanCompare = 2,
};

// Correctness gate for the CPU (gdi) route: replay a capture and check the
// composed desktop against a *golden reference* recorded from an earlier run of
// the same capture (replaces the engine-vs-gdi shadow comparison, which cannot be
// kept consistent and needs the engine, doc_agent/cpu-accel-plan.md §7).
//
//  kOff     - nothing (the normal perf run).
//  kExport  - record the reference: one 64-bit hash of the composed desktop per
//             frame into `hmrdp_ref.hash`, plus the last frame as `hmrdp_ref.bmp`
//             so a human can look at it.
//  kCompare - check every frame's hash against the stored list (`bad` = frames
//             that differ) and, for the frame the reference image was taken from,
//             compare pixel by pixel (`rgbPx`/`maxDelta`/`bbox`). A failing run
//             also dumps the frame that failed as `hmrdp_ref_fail.bmp`.
//
// The reference is only meaningful while the decode is *bit-exact*: that is the
// contract the CPU-side optimizations must keep (逐位等价), so the same golden
// file stays valid across them.
enum class GfxReplayRefMode {
  kOff = 0,
  kExport = 1,
  kCompare = 2,
};

class GfxReplay {
 public:
  static GfxReplay& Instance();

  // Takes ownership of `nativeWindow` (from the XComponent surface id) and
  // replays `gfxPath` (a raw hmrdp_gfx.bin capture) onto it through `route`.
  // Waits briefly for the first frame or failure.
  //
  // `realtime` selects the playback clock:
  //   false - **not throttled at all** (default): the pump feeds the next record as
  //           soon as the previous frame is done, so the machine stays saturated
  //           and the numbers are a pure throughput measurement ("can the client
  //           chew through this stream"). Deliberately unpaced: a frame budget
  //           (the earlier kFrameMs sleep) left the CPU idle between frames, which
  //           dropped the whole SoC to its lowest clock and made every per-frame
  //           cost ~3x larger - see doc_agent/cpu-accel-plan.md §7.
  //   true  - each record is fed at the arrival time recorded in the capture, so
  //           the frame cadence, the per-frame gaps and therefore the machine
  //           state (CPU placement/frequency, cache locality, threadpool
  //           wake-ups) match the live session. Requires a version 1 capture;
  //           for an untimed capture it falls back to false and says so.
  bool Start(void* nativeWindow, int surfaceW, int surfaceH, const std::string& gfxPath,
             GfxReplayRoute route, bool realtime, GfxReplayRefMode refMode = GfxReplayRefMode::kOff);
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
  // Golden reference (see GfxReplayRefMode). `RefCollectFrame` runs on the replay
  // thread once per composed frame (CPU route): it hashes it, and in export mode
  // keeps the hash / in compare mode checks it against the loaded list.
  void RefCollectFrame(GfxCpuDesktop* cpu);
  // Loads `hmrdp_ref.hash` (+ the `.bmp` it names) next to the capture; false with
  // a one-line reason in `refNote_` when the reference is missing/unusable.
  bool RefLoad(const std::string& capturePath);
  // Writes this run as the reference: the per-frame hashes plus the last composed
  // frame as `.bmp` (the frame the hash list's `imageFrame` points at).
  void RefWrite(const std::string& capturePath, GfxCpuDesktop* cpu);
  // Pixel-level comparison against the stored reference image (only for the frame
  // the image was taken from); fills the cmp* fields.
  void RefCompareImage(const uint8_t* data, int stride, int width, int height);
  // Closes the frame for the cadence bookkeeping: the first presented frame marks
  // the fps origin (so a run's start-up does not dilute the rate) and every frame
  // counts. It deliberately does not sleep - the fast mode runs flat out and the
  // realtime mode's cadence comes from the capture (PaceRecord), so there is
  // nothing left to throttle here.
  void MarkFrameEnd(bool presented);

  // Realtime mode: called from GfxReplayPump right before a record is fed, with
  // the record's recorded arrival time (monotonic microseconds). Sleeps until the
  // recorded offset from the first record has elapsed; when the client is already
  // behind, it does not sleep (no catch-up) and only records how far behind the
  // schedule it is. This is the *only* deliberate playback sleep left.
  void PaceRecord(uint64_t timestampUs);

  std::mutex mutex_;
  // Per-frame client work for the CPU (gdi) route: the very same meter the live
  // session uses, fed through the same hooks (chunk stamp + wrapped
  // SurfaceCommand/EndFrame + present), so a replayed frame and a live frame are
  // measured identically and can be compared figure by figure
  // (hmrdp_gfx_work.h). Only the CPU route installs its wrappers - it is the one
  // that mirrors live (gdi); the engine route keeps its own stats.
  GfxWorkMeter meter_;
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
  // CPU present upload (doc_agent/gfx-engine.md §2.3): what this run's presents
  // actually uploaded vs what the merged box would have cost, how often the rect
  // list was used, and how often the rect cap forced the box. Kept as a standing
  // regression read-out - the saving is visible in every run.
  std::atomic<uint64_t> uploadBytes_{0};
  std::atomic<uint64_t> uploadBoxBytes_{0};
  std::atomic<uint64_t> uploadRectPresents_{0};
  std::atomic<uint64_t> uploadTruncated_{0};
  // Largest number of individual dirty rects one frame carried (the raw gdi
  // count, before the presenter cap): says whether the cap was even near and how
  // scattered the content is.
  std::atomic<uint64_t> uploadMaxRects_{0};
  // Process CPU time (all threads) at the start and the end of the run: the
  // energy side of the decode-worker A/B. Wall time alone cannot tell "faster"
  // from "more cores woken for nothing"; a run whose `本机` stops improving while
  // cpu climbs is past the sweet spot (doc_agent/cpu-accel-plan.md §5).
  std::atomic<int64_t> cpuStartUs_{0};
  std::atomic<int64_t> cpuEndUs_{0};
  std::atomic<uint64_t> pumpUs_{0};
  // Time spent deliberately sleeping in PaceRecord(); subtracted from pumpUs_ so
  // the reported feed cost is compute, not playback throttling. The fast mode does
  // not sleep, so this is 0 there.
  std::atomic<uint64_t> paceUs_{0};
  // Realtime playback: requested by the caller, and effective only when the
  // capture carries arrival times (an untimed capture falls back to the unpaced
  // fast mode, and says so in the log).
  std::atomic<int> realtimeRequested_{0};
  std::atomic<int> realtimeActive_{0};
  // Worst lateness behind the recorded schedule (realtime mode): the honest
  // "cannot keep up with the real load" figure, in microseconds.
  std::atomic<uint64_t> realtimeLagUs_{0};
  // Realtime schedule origin: first record's recorded time and the wall clock it
  // was fed at (replay thread only).
  uint64_t recordBaseUs_ = 0;
  int64_t recordWallBaseUs_ = 0;
  // When the first presented frame ended: fps is measured from here, so the run's
  // start-up (engine/presenter init, first-frame wait) does not count as playback
  // time (replay thread writes, UI thread reads via StatsLines).
  std::atomic<int64_t> firstFrameEndUs_{0};
  // When the pump started, so the reported `feed` (compute) can exclude the run's
  // start-up and mean "per-frame compute" (replay thread writes).
  std::atomic<int64_t> pumpStartUs_{0};
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

  // --- golden reference (kExport / kCompare, CPU route only) ---------------
  // One hash per composed frame, in playback order. Written to / read from
  // `hmrdp_ref.hash` next to the capture (replay thread only).
  std::vector<uint64_t> refHashes_;
  std::atomic<int> refMode_{0};
  // Frames recorded/checked so far (the vector itself is replay-thread only), and
  // the total time spent hashing them (reported so a reference run's `compose` is
  // never read as a performance figure - the hash runs inside gdi's EndFrame).
  std::atomic<uint64_t> refFrames_{0};
  std::atomic<uint64_t> refHashUs_{0};
  std::atomic<uint64_t> refChecks_{0};
  std::atomic<uint64_t> refBad_{0};
  std::atomic<int64_t> refFirstBad_{-1};
  // The stored reference image (top-down BGRA, `refImageW_ * 4` bytes per row) and
  // the frame index it was taken from, both from the stored file when comparing.
  std::vector<uint8_t> refImage_;
  int refImageW_ = 0;
  int refImageH_ = 0;
  int64_t refImageFrame_ = -1;
  // Why the stored reference could not be used exactly as recorded (missing
  // image, geometry change, ...): one line, reported in the stats.
  std::string refNote_;
  // Fingerprint of the capture this run replays: the reference files are named
  // after it (`hmrdp_ref_<tag>.hash/.bmp`), because the device's capture file
  // name is always hmrdp_gfx.bin - a reference recorded from another recording
  // must not be usable by accident (doc_agent/gfx-engine.md §6).
  std::string refTag_;

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
