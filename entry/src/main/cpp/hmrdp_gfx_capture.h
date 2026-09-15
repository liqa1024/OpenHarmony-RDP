/*
 * HmRdp - raw RDPGFX channel capture (doc_agent/gfx-engine.md §6).
 *
 * Records exactly what the server sent on the GFX dynamic virtual channel,
 * before any decompression: one record per channel chunk (`u32 length` +
 * `length` bytes). The replay harness (hmrdp_replay.cpp) feeds those bytes back
 * through FreeRDP's own ZGX + RDPGFX parsing, so the file stays completely free
 * of any engine / serialization coupling and is smaller (one compression layer
 * less than a parsed command stream).
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

// Appends one raw (still ZGX-compressed) GFX channel chunk. No-op unless the
// capture is enabled.
void GfxDumpRaw(const void* data, uint32_t size);

// While the replay harness is reading a capture, new raw chunks are ignored so a
// running capture is not overwritten by the replayed stream.
void GfxDumpSetReplaying(bool replaying);

// Reads the raw chunk stream back for replay.
class GfxRawCapture {
 public:
  bool Open(const std::string& path);
  // Points `data`/`size` at the next chunk (valid until the next call). Returns
  // false at the end of the stream or on a truncated record.
  bool Next(const uint8_t** data, uint32_t* size);

 private:
  std::vector<uint8_t> data_;
  size_t pos_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_GFX_CAPTURE_H
