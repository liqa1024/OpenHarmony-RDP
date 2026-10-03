/*
 * HmRdp - raw RDPGFX channel capture (doc_agent/gfx-engine.md §6).
 *
 * Records what the server sent on the GFX channel before any decompression, one
 * record per channel chunk; the replay (hmrdp_replay.cpp) feeds those bytes back
 * through FreeRDP's ZGX + RDPGFX parsing. Layout (version 1):
 *
 *   header  'H','M','R','G','P','G','X','1'   (8-byte magic)
 *   record  u32 length | u64 arrivalUs | payload[length]
 *
 * `arrivalUs` (monotonic µs) lets a replay reproduce the server's real cadence;
 * without it a replay can only measure throughput. Version 0 (no magic, plain
 * `u32 length | payload`) is still read - `hasTimestamps()` says which layout.
 *
 * Driven by the FreeRDP hook registered from hmrdp_session.cpp; no FreeRDP
 * dependency here.
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
