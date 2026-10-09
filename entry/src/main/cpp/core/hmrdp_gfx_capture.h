#ifndef HMRDP_GFX_CAPTURE_H
#define HMRDP_GFX_CAPTURE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hmrdp {

// Enables/disables the dump and points it at `dir` (the app filesDir). An empty
// dir or `enabled=false` closes the file. Idempotent, so re-applying the same
// value does not truncate a running capture.
void GfxDumpConfigure(bool enabled, const std::string& dir);

void GfxDumpShutdown();

void GfxDumpFlush();

void GfxDumpRaw(const void* data, uint32_t size);

// While the replay harness is reading a capture, new raw chunks are ignored so a
// running capture is not overwritten by the replayed stream.
void GfxDumpSetReplaying(bool replaying);

// Whether `path` carries per-record arrival times (version 1 layout). Reads only
// the header.
bool GfxCaptureHasTimestamps(const std::string& path);

class GfxRawCapture {
 public:
  bool Open(const std::string& path);
  // Points `data`/`size` at the next chunk (valid until the next call). Returns
  // false at the end of the stream or on a truncated record.
  bool Next(const uint8_t** data, uint32_t* size);
  uint64_t timestampUs() const { return timestampUs_; }
  bool hasTimestamps() const { return version_ >= 1; }

 private:
  std::vector<uint8_t> data_;
  size_t pos_ = 0;
  uint32_t version_ = 0;
  uint64_t timestampUs_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_GFX_CAPTURE_H
