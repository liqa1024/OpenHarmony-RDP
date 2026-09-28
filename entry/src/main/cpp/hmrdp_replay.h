/*
 * HmRdp - dev-only recorded-RDP replay (doc_agent/gfx-engine.md §5/§6).
 *
 * Replays a captured raw GFX channel stream (hmrdp_gfx.bin) through FreeRDP's own
 * ZGX + RDPGFX parsing and gdi pipeline, presenting each frame through the
 * presenter the "硬件加速" setting selects. Debug facility only.
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
struct PresentUploadInfo;  // hmrdp_gfx_cpu.h

// Correctness gate (doc_agent/gfx-engine.md §8.2). Files are named after the
// capture's fingerprint (`hmrdp_ref_<tag>.{hash,bmp}`), so a reference recorded
// from another capture cannot be used by accident.
//   kOff     - plain perf run.
//   kExport  - write the reference: one 64-bit hash per composed frame + the last
//              frame as a BMP.
//   kCompare - check each frame's hash (`bad` = differing frames) and pixel-compare
//              the frame the reference image came from (`rgbPx`/`maxDelta`/`bbox`).
enum class GfxReplayRefMode {
  kOff = 0,
  kExport = 1,
  kCompare = 2,
};

class GfxReplay {
 public:
  static GfxReplay& Instance();

  // Takes ownership of `nativeWindow` and replays `gfxPath` onto it; waits briefly
  // for the first frame or a failure.
  // `realtime` = false feeds the next record as soon as the previous frame is done
  // (pure throughput, not the live cadence); true feeds each record at the arrival
  // time stored in the capture (v1 only - an untimed capture falls back to false
  // and reports mode=fast(untimed)).
  bool Start(void* nativeWindow, int surfaceW, int surfaceH, const std::string& gfxPath,
             bool realtime, GfxReplayRefMode refMode = GfxReplayRefMode::kOff);
  void Resize(int width, int height);
  void Stop();
  // Single-line form for logs, multi-line for the on-device stats panel.
  std::string Stats();
  std::string StatsLines();

  // Frame observers installed on the shared GdiFrameHost (same EndPaint chain a
  // live session runs). OnCpuFramePre may veto a frame (run cap, resize, energy
  // sample); OnCpuFramePresent does the frame's accounting.
  bool OnCpuFramePre();
  void OnCpuFramePresent(GfxCpuDesktop* cpu, bool presented, uint64_t presentUs,
                         const PresentUploadInfo& upload);

  // Accumulated present time (µs), reported per frame by Stats().
  void RecordPresent(uint64_t micros);

 private:
  GfxReplay() = default;
  ~GfxReplay();
  GfxReplay(const GfxReplay&) = delete;
  GfxReplay& operator=(const GfxReplay&) = delete;

  void Run();
  void RunCpuReplay(const std::string& gfxPath);
  // Golden reference (see GfxReplayRefMode): hash the composed frame, keep it in
  // export mode / check it in compare mode. Replay thread, once per frame.
  void RefCollectFrame(GfxCpuDesktop* cpu);
  // Load `hmrdp_ref_<tag>.hash` (+ the `.bmp` it names) next to the capture; false
  // with a reason in `refNote_` when it is missing/unusable.
  bool RefLoad(const std::string& capturePath);
  // Write this run as the reference (per-frame hashes + the last frame as .bmp).
  void RefWrite(const std::string& capturePath, GfxCpuDesktop* cpu);
  // Pixel comparison against the stored reference image (only its frame).
  void RefCompareImage(const uint8_t* data, int stride, int width, int height);
  // Cadence bookkeeping: the first presented frame marks the fps origin; nothing
  // is throttled here (fast mode never sleeps, realtime paces in PaceRecord).
  void MarkFrameEnd(bool presented);
  // Realtime pacing: sleep until the recorded offset from the first record has
  // elapsed; when already behind, do not sleep (no catch-up), only record `lag`.
  // The only deliberate playback sleep.
  void PaceRecord(uint64_t timestampUs);

  std::mutex mutex_;
  GfxWorkMeter meter_;
  std::unique_ptr<FramePresenter> presenter_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  // Run identity and whether it was cut short. A driver uses runId_ to tell one
  // round's stats from the previous round's; an aborted run is not a measurement
  // and must not be written as a reference.
  std::atomic<uint64_t> runId_{0};
  std::atomic<bool> aborted_{false};
  void* window_ = nullptr;
  std::string gfxPath_;
  int surfaceW_ = 0;
  int surfaceH_ = 0;
  std::atomic<int> pendingW_{0};  std::atomic<int> pendingH_{0};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> presents_{0};
  // EndFrame markers whose present did not reach the screen (window/surface not
  // ready, GL error).
  std::atomic<uint64_t> presentFailures_{0};
  std::atomic<uint64_t> presentUs_{0};
  // Present upload accounting (doc_agent/present-pipeline.md §3).
  std::atomic<uint64_t> uploadBytes_{0};
  std::atomic<uint64_t> uploadBoxBytes_{0};
  std::atomic<uint64_t> uploadRectPresents_{0};
  std::atomic<uint64_t> uploadTruncated_{0};
  // Largest raw gdi rect count one frame carried (before the presenter cap).
  std::atomic<uint64_t> uploadMaxRects_{0};
  // Process CPU time at run start/end. Reported next to `cpuKHz=` because two runs
  // are only comparable at the same clock (doc_agent/gfx-engine.md §8.1).
  std::atomic<int64_t> cpuStartUs_{0};
  std::atomic<int64_t> cpuEndUs_{0};
  std::atomic<uint64_t> pumpUs_{0};
  // Deliberate PaceRecord() sleep, subtracted from pumpUs_ so `feed` is compute.
  std::atomic<uint64_t> paceUs_{0};
  // realtimeRequested_ = caller's wish; realtimeActive_ = it actually ran paced
  // (false when the capture has no arrival times).
  std::atomic<int> realtimeRequested_{0};
  std::atomic<int> realtimeActive_{0};
  // Worst lateness behind the recorded schedule, µs (realtime only).
  std::atomic<uint64_t> realtimeLagUs_{0};
  // Realtime schedule origin: first record's recorded time and the wall clock it
  // was fed at (replay thread only).
  uint64_t recordBaseUs_ = 0;
  int64_t recordWallBaseUs_ = 0;
  // First presented frame's end: fps is measured from here so start-up does not
  // count as playback (replay thread writes, UI thread reads).
  std::atomic<int64_t> firstFrameEndUs_{0};
  // Pump start, so `feed` can exclude the run's start-up.
  std::atomic<int64_t> pumpStartUs_{0};
  std::atomic<int64_t> startUs_{0};
  // Set when the loop ends, so Stats() stops decaying fps/feed while idle.
  std::atomic<int64_t> endUs_{0};
  // Golden-reference pixel diff for the single frame the stored image came from
  // (the hashes say *if* a frame differs; these say by how much).
  std::atomic<uint64_t> imgDiffChecks_{0};
  std::atomic<uint64_t> imgDiffBad_{0};
  std::atomic<uint64_t> imgDiffRgbPx_{0};
  std::atomic<uint64_t> imgDiffAlphaPx_{0};
  std::atomic<int> imgDiffBBoxX0_{-1};
  std::atomic<int> imgDiffBBoxY0_{-1};
  std::atomic<int> imgDiffBBoxX1_{-1};
  std::atomic<int> imgDiffBBoxY1_{-1};
  std::atomic<int> imgDiffMaxDelta_{0};
  std::atomic<uint64_t> imgDiffSmallPx_{0};  // delta <= 2 (rounding, not content)

  // Golden reference: one hash per composed frame, in playback order, written to /
  // read from `hmrdp_ref_<tag>.hash` (replay thread only).
  std::vector<uint64_t> refHashes_;
  std::atomic<int> refMode_{0};
  // Frames recorded/checked and the total hash time (a reference run's `compose`
  // must never be read as a performance figure - the hash runs in gdi's EndFrame).
  std::atomic<uint64_t> refFrames_{0};
  std::atomic<uint64_t> refHashUs_{0};
  std::atomic<uint64_t> refChecks_{0};
  std::atomic<uint64_t> refBad_{0};
  std::atomic<int64_t> refFirstBad_{-1};
  // Stored reference image (top-down BGRA, `refImageW_ * 4` bytes/row) and the
  // frame index it came from.
  std::vector<uint8_t> refImage_;
  int refImageW_ = 0;
  int refImageH_ = 0;
  int64_t refImageFrame_ = -1;
  // Why the stored reference could not be used as recorded, one line in the stats.
  std::string refNote_;
  // Capture fingerprint: the device's capture name is always hmrdp_gfx.bin, so the
  // reference is keyed by this tag to keep another recording from reusing it.
  std::string refTag_;

  std::mutex errorMutex_;
  std::string lastError_;
};

}  // namespace hmrdp

#endif  // HMRDP_REPLAY_H
