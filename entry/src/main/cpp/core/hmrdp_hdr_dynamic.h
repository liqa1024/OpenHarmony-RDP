#ifndef HMRDP_HDR_DYNAMIC_H
#define HMRDP_HDR_DYNAMIC_H

#include <cstddef>
#include <cstdint>

namespace hmrdp {

// HDR Vivid's registered T.35 envelope: itu_t_t35_country_code, terminal_provider_code and
// terminal_provider_oriented_code. What follows it is the CUVA `dynamic_metadata()` body, i.e.
// `system_start_code` first (UWA 005.1 §7.3) - the form the platform's Vivid consumers take, so
// whoever hands the payload over skips exactly these bytes.
//
// The host writes this into a *standard* HEVC stream whenever its HDR luminance analysis runs, so
// nothing is negotiated for it; Vivid has no AV1 carriage (the standard defines none).
constexpr size_t kVividEnvelopeSize = 5;
constexpr uint8_t kVividEnvelope[kVividEnvelopeSize] = {0x26, 0x00, 0x04, 0x00, 0x05};

// The envelope plus the four CUVA statistics fields and their flags. A larger payload is treated as
// not-Vivid rather than truncated, so a malformed frame can never hand the compositor half a payload.
constexpr size_t kMaxVividPayload = 64;

struct VividMetadata {
  // The de-escaped registered payload, starting at the envelope; `size` 0 means none was carried.
  uint8_t payload[kMaxVividPayload] = {};
  size_t size = 0;
};

// Scans one access unit and returns the HDR Vivid payload it carries, if any. `mime` is the codec
// MIME ("video/hevc" or "video/av1"); anything else yields an empty result. Unit: for HEVC the
// Annex-B byte stream (the decode bridge's flat access unit); for AV1 the low-overhead OBU stream.
VividMetadata ScanVividMetadata(const uint8_t* data, size_t size, const char* mime);

}

#endif
