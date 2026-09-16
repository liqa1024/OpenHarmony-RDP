/*
 * HmRdp - raw RDPGFX channel capture (doc_agent/gfx-engine.md §6).
 *
 * Records exactly what the server sent on the GFX dynamic virtual channel,
 * before any decompression: one record per channel chunk. The replay harness
 * (hmrdp_replay.cpp) feeds those bytes back through FreeRDP's own ZGX + RDPGFX
 * parsing, so the file stays completely free of any engine / serialization
 * coupling and is smaller (one compression layer less than a parsed command
 * stream).
 *
 * On-disk layout (version 1, the current one):
 *
 *   header  'H','M','R','G','P','G','X','1'      (8 bytes magic)
 *   record  u32 length | u64 arrivalUs | payload[length]
 *
 * `arrivalUs` is the monotonic clock at which the chunk arrived, so a replay can
 * reproduce the server's real cadence instead of a synthetic one. Without it a
 * replay can only measure throughput (see doc_agent/gfx-engine.md §6) and cannot
 * reproduce the load the live session actually ran under.
 *
 * Version 0 (no magic, `u32 length | payload[length]`, no timing) is still read:
 * `GfxRawCapture::hasTimestamps()` reports which layout the file uses, and the
 * replay falls back to its fixed pacing for those files.
 *
 * The recorder is driven by the FreeRDP hook registered from hmrdp_session.cpp
 * (`HmrdpSetGfxRawCapture`); this module has no FreeRDP dependency.
 */
#ifndef HMRDP_GFX_CAPTURE_H
#define HMRDP_GFX_CAPTURE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hmrdp {

// Enables/disables the dump and points it at `dir` (the app filesDir). Passing
// an empty dir or `enabled=false` closes the file. Idempotent, so re-applying
// the same value does not truncate a running capture.
void GfxDumpConfigure(bool enabled, const std::string& dir);

// Flushes and closes the file. Called when the capture is turned off.
void GfxDumpShutdown();

// Flushes pending bytes to disk without closing (called at session disconnect).
void GfxDumpFlush();

// Appends one raw (still ZGX-compressed) GFX channel chunk, together with the
// monotonic time it arrived. No-op unless the capture is enabled.
void GfxDumpRaw(const void* data, uint32_t size);

// While the replay harness is reading a capture, new raw chunks are ignored so a
// running capture is not overwritten by the replayed stream.
void GfxDumpSetReplaying(bool replaying);

// Whether `path` carries per-record arrival times (version 1 layout). Reads only
// the header, so it is cheap even for a large capture - the replay uses it to
// decide whether the realtime mode is available before opening the file.
bool GfxCaptureHasTimestamps(const std::string& path);

// Reads the raw chunk stream back for replay.
class GfxRawCapture {
 public:
  bool Open(const std::string& path);
  // Points `data`/`size` at the next chunk (valid until the next call). Returns
  // false at the end of the stream or on a truncated record.
  bool Next(const uint8_t** data, uint32_t* size);
  // Arrival time of the record last returned by Next(), on the monotonic clock
  // (microseconds). Only meaningful when hasTimestamps() is true.
  uint64_t timestampUs() const { return timestampUs_; }
  // Whether the file carries arrival times (version 1 layout).
  bool hasTimestamps() const { return version_ >= 1; }

 private:
  std::vector<uint8_t> data_;
  size_t pos_ = 0;
  uint32_t version_ = 0;
  uint64_t timestampUs_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_GFX_CAPTURE_H
