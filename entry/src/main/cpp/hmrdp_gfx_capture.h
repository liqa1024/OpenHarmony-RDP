/*
 * HmRdp - captured RDPGFX data: recording, reading and offline comparison
 * (PERF-TODO §4).
 *
 * The capture format is produced by the dev "抓取 RFX 码流（测试）" switch during a
 * live session (hmrdp_gfx.bin command stream + hmrdp_gfx_surface.bin baselines)
 * and consumed by two replayers:
 *   - the on-device replay harness (hmrdp_replay.cpp) renders the command
 *     stream to the screen for a fixed, reproducible test;
 *   - the offline self-test (hmrdp_rfx.cpp) replays the same stream and diffs
 *     each frame against the captured baselines.
 *
 * This module is the single place the capture layouts are defined and parsed, so
 * the writer and the readers can never drift apart. The read/compare side has no
 * FreeRDP or GLES dependency and can be compiled on the host.
 *
 * Layout (little-endian), see PERF-TODO appendix A:
 *   hmrdp_gfx.bin         'GFX1' command records (10 x u32 header + params + payload)
 *   hmrdp_gfx_surface.bin 'GFH1' per-frame FNV-1a hash + 'GFS1' full BGRA baseline
 */
#ifndef HMRDP_GFX_CAPTURE_H
#define HMRDP_GFX_CAPTURE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hmrdp {

// ---- Recording (write side, driven by hmrdp_session.cpp) -------------------

// Enables/disables the dump and points it at `dir` (the app filesDir). Passing
// an empty dir or `enabled=false` closes the files and logs the final command
// histogram. Safe to call from the UI thread while the RDP thread is recording.
void GfxDumpConfigure(bool enabled, const std::string& dir);

// Flushes, closes and logs the histogram. Called when the capture is turned off.
void GfxDumpShutdown();

// Flushes the pending records to disk without closing (called at session
// disconnect so a crash or abrupt exit cannot lose the tail of the capture).
void GfxDumpFlush();

// Records one GFX command. `scalars` are four command-specific u32 values.
// `params`/`payload` may be null when their length is 0. Returns the 1-based
// record index (0 when the dump is disabled or the size cap was reached).
uint32_t GfxDumpCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                        const void* params, uint32_t paramsLen, const void* payload,
                        uint32_t payloadLen);

// Records the BGRA surface `surfaceId` as of `recordIndex`. `data` must be
// `stride * height` bytes; a null/empty surface is ignored.
void GfxDumpSurface(uint32_t recordIndex, uint32_t surfaceId, uint32_t width, uint32_t height,
                    uint32_t stride, uint32_t format, const void* data);

// Human-readable "cmd=count" histogram of the current capture.
std::string GfxDumpHistogramSummary();

// ---- Playback (read side) --------------------------------------------------

// One record of an hmrdp_gfx.bin command stream. `params`/`payload` point into
// the buffer owned by GfxCapture and stay valid until it is reopened.
struct GfxCaptureRecord {
  uint32_t index = 0;  // 1-based record index (matches the surface baseline)
  uint16_t cmdId = 0;  // RDPGFX_CMDID_*
  uint32_t surfaceId = 0;
  uint32_t scalars[4] = {0, 0, 0, 0};
  uint32_t paramsLen = 0;
  uint32_t payloadLen = 0;
  const uint8_t* params = nullptr;
  const uint8_t* payload = nullptr;
};

// Forward iterator over the records of an hmrdp_gfx.bin capture. Both the screen
// replay and the offline comparison drive their loop through Next(), so the
// record layout is read in exactly one place.
class GfxCapture {
 public:
  bool Open(const std::string& path);
  bool ok() const { return !data_.empty(); }
  const std::string& path() const { return path_; }
  // Fills `out` with the next record. Returns false at the end of the stream or
  // on a malformed record (the capture is then exhausted).
  bool Next(GfxCaptureRecord* out);
  uint32_t RecordsRead() const { return recordsRead_; }

 private:
  std::vector<uint8_t> data_;
  size_t pos_ = 0;
  uint32_t recordsRead_ = 0;
  std::string path_;
};

// One full BGRA surface baseline captured with the command stream ('GFS1').
struct GfxSurfaceBaseline {
  uint32_t id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t format = 0;
  std::vector<uint8_t> data;
};

// Per-frame baselines from hmrdp_gfx_surface.bin: an FNV-1a hash per
// (record, surface) for every frame, plus the full BGRA images for the first
// frames. A missing file leaves both maps empty so a replay can still run.
struct GfxSurfaceBaselines {
  std::map<uint32_t, std::map<uint32_t, uint64_t>> hashes;   // record -> sid -> hash
  std::map<uint32_t, std::vector<GfxSurfaceBaseline>> full;  // record -> surfaces

  bool Load(const std::string& path);
  bool empty() const { return hashes.empty() && full.empty(); }
  const GfxSurfaceBaseline* FindFull(uint32_t recordIndex, uint32_t surfaceId) const;
};

// FNV-1a 64, identical to the hash written by the recorder above.
uint64_t GfxCaptureHash(const uint8_t* data, size_t size);

// A read-only view of one engine surface's pixels. `stride` is the surface
// scanline in bytes (aligned like the captured baselines).
struct GfxSurfacePixels {
  const uint8_t* data = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
};

// Reads one surface's current pixels for comparison; returns false when the
// surface does not exist. `out->data` must stay valid until the call returns.
using GfxSurfaceReader = std::function<bool(uint32_t surfaceId, GfxSurfacePixels* out)>;

// Outcome of comparing every surface a single EndFrame observed. `mismatch`
// aggregates bad bytes, missing/dimension-mismatched surfaces and hash
// mismatches, matching the historical self-test counters.
struct GfxFrameComparison {
  bool hadBaseline = false;
  size_t surfaceCount = 0;
  uint64_t surfacesCompared = 0;  // finished full-pixel comparisons
  uint64_t surfacesHashed = 0;
  uint64_t comparedBytes = 0;
  uint64_t mismatch = 0;
  uint64_t hashMismatch = 0;
  int64_t sumAbs = 0;
};

// Compares the surfaces recorded for `recordIndex` against `baselines` using
// `read`.
GfxFrameComparison GfxCompareFrame(uint32_t recordIndex, const GfxSurfaceBaselines& baselines,
                                   const GfxSurfaceReader& read);

}  // namespace hmrdp

#endif  // HMRDP_GFX_CAPTURE_H
