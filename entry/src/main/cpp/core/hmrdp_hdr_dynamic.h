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

// HDR10+ (Samsung ST 2094-40, registered T.35 `B5 00 3C`) comes beside HDR Vivid in every PQ access
// unit the base version encodes, and no client can ask it not to (see moonlight-video.md §2). The
// platform has no HDR10+ at all, so this side drops it and keeps the plain HDR10 base - the static
// mastering-display and content-light SEI is a different message, left alone - plus HDR Vivid. That
// SEI sits in an access unit of its own, so the removal is that byte range, start code included.
struct Hdr10PlusRemoval {
  size_t offset = 0;
  size_t length = 0;
};

// Plans that removal for one access unit. False when there is no such SEI (nothing to drop); a NAL
// that also carries a payload this side keeps (Vivid) is left untouched.
bool PlanHdr10PlusRemoval(const uint8_t* data, size_t size, const char* mime,
                          Hdr10PlusRemoval* removal);

// The base version's PQ Vivid payload also carries a per-frame tone-mapping curve beside the
// statistics (`tone_mapping_mode_flag` set, then the base parameters). Measured offline: its
// middle-tone parameter takes exactly two values (1.2 / 4.4) and flips between them every few
// frames, unrelated to the statistics, so a display that obeys the curve remaps the whole picture in
// steps - and stripping it is what stopped the flicker on the device. The payload is therefore
// rewritten to the statistics-only form, the one HLG already carries and the one the base version's
// own serializer writes today: the four statistics stay, everything after them goes.
//
// The rewrite changes the payload's length, so `offset` (the payload's size field) .. +`length` is
// replaced by `payload` (`payloadSize` bytes: the new size field, then the escaped payload).
struct VividCurveStrip {
  size_t offset = 0;
  size_t length = 0;
  uint8_t payload[kMaxVividPayload] = {};
  size_t payloadSize = 0;
};

// Plans that rewrite for one access unit. False when there is nothing to do: no Vivid payload, one
// this side does not rewrite (it is not the NAL's only payload), or one that is already
// statistics-only - the form a host that stops writing the curve sends.
bool PlanVividCurveStrip(const uint8_t* data, size_t size, const char* mime,
                         VividCurveStrip* strip);

}

#endif
