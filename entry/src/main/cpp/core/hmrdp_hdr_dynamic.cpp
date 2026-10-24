#include "hmrdp_hdr_dynamic.h"

#include <cstring>

namespace hmrdp {
namespace {

// The envelope's bytes are never touched by emulation prevention, so they identify the payload
// before any de-escaping.
bool StartsWithVivid(const uint8_t* p, size_t n) {
  return n >= kVividEnvelopeSize && std::memcmp(p, kVividEnvelope, kVividEnvelopeSize) == 0;
}

// Removes the RBSP emulation-prevention bytes (a 0x03 following 00 00) so the compositor receives
// the clean registered payload. False when the result would not fit `capacity`.
bool Unescape(const uint8_t* in, size_t n, uint8_t* out, size_t capacity, size_t* outSize) {
  size_t zeros = 0;
  size_t written = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = in[i];
    if (zeros >= 2 && b == 0x03) {
      zeros = 0;
      continue;  // drop the emulation-prevention byte itself
    }
    if (written >= capacity) {
      return false;
    }
    out[written++] = b;
    zeros = b == 0x00 ? zeros + 1 : 0;
  }
  *outSize = written;
  return true;
}

// Keeps the payload when it is Vivid and none was kept yet. `escaped` is whether the span still
// holds the codec's emulation-prevention bytes: an HEVC SEI carries them, an AV1 metadata OBU is
// already raw.
void StoreVivid(const uint8_t* payload, size_t size, bool escaped, VividMetadata* into) {
  if (into->size != 0 || payload == nullptr || size == 0 || !StartsWithVivid(payload, size)) {
    return;
  }
  VividMetadata out;
  size_t clean = 0;
  if (escaped) {
    if (!Unescape(payload, size, out.payload, sizeof(out.payload), &clean) || clean == 0) {
      return;
    }
  } else {
    if (size > sizeof(out.payload)) {
      return;
    }
    std::memcpy(out.payload, payload, size);
    clean = size;
  }
  out.size = clean;
  *into = out;
}

// ---- HEVC (Annex-B) ----

// The next Annex-B start code at/after `from`; `codeLength` gets 3 or 4; `size` when none.
size_t FindStartCode(const uint8_t* data, size_t size, size_t from, size_t* codeLength) {
  size_t i = from;
  while (i + 2 < size) {
    // This runs once per submitted frame, so it seeks zero bytes with memchr instead of comparing
    // every byte; a start code always begins with a zero.
    const uint8_t* zero = static_cast<const uint8_t*>(std::memchr(data + i, 0x00, size - i - 2));
    if (zero == nullptr) {
      break;
    }
    i = static_cast<size_t>(zero - data);
    if (data[i + 1] == 0x00 && data[i + 2] == 0x01) {
      *codeLength = 3;
      return i;
    }
    if (i + 3 < size && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
      *codeLength = 4;
      return i;
    }
    ++i;
  }
  return size;
}

// Walks the NALs of one Annex-B access unit, calling `visit(unitOffset, nal, size)`; stops when it
// returns false. `unitOffset` is where that NAL starts in the unit - its start code, so a caller can
// drop the whole thing by byte range.
template <typename Visit>
void WalkNals(const uint8_t* data, size_t size, Visit visit) {
  size_t codeLength = 0;
  size_t at = FindStartCode(data, size, 0, &codeLength);
  while (at < size) {
    const size_t nalStart = at + codeLength;
    size_t nextCode = 0;
    const size_t next = FindStartCode(data, size, nalStart, &nextCode);
    // The NAL ends at the next start code; its leading zero bytes belong to that code, not here.
    size_t nalEnd = next;
    while (nalEnd > nalStart && data[nalEnd - 1] == 0x00) {
      --nalEnd;
    }
    if (nalEnd > nalStart && !visit(at, data + nalStart, nalEnd - nalStart)) {
      return;
    }
    at = next;
    codeLength = nextCode;
  }
}

bool IsSeiNal(const uint8_t* nal, size_t size) {
  if (size < 3) {
    return false;
  }
  const int nalType = (nal[0] >> 1) & 0x3F;
  return nalType == 39 || nalType == 40;  // PREFIX_SEI / SUFFIX_SEI
}

// Walks the SEI messages of one SEI NAL, calling `visit(payloadType, payloadStart, payloadEnd)`;
// stops when it returns false. `payloadStart`..`payloadEnd` is the payload as it sits in the unit.
template <typename Visit>
void WalkSeiMessages(const uint8_t* nal, size_t size, Visit visit) {
  const uint8_t* p = nal + 2;  // skip the 2-byte NAL header
  const uint8_t* end = nal + size;
  while (p < end) {
    uint32_t payloadType = 0;
    while (p < end && *p == 0xFF) {
      payloadType += 255;
      ++p;
    }
    if (p >= end) {
      break;
    }
    payloadType += *p++;
    uint32_t payloadSize = 0;
    while (p < end && *p == 0xFF) {
      payloadSize += 255;
      ++p;
    }
    if (p >= end) {
      break;
    }
    payloadSize += *p++;
    // The size field counts RBSP bytes, i.e. *before* the emulation-prevention bytes the encoder
    // inserts, and these payloads are mostly zero fields (00 00 -> 00 00 03 is common at every value
    // boundary). Advancing by the raw size would land mid-payload and lose whatever follows it -
    // including the payload itself when the unit carries more than one SEI message.
    const uint8_t* const payloadStart = p;
    size_t remaining = payloadSize;
    size_t zeros = 0;
    while (remaining > 0 && p < end) {
      const uint8_t b = *p++;
      if (zeros >= 2 && b == 0x03) {
        zeros = 0;
        continue;  // an escape byte, not one of the payload's own bytes
      }
      zeros = b == 0x00 ? zeros + 1 : 0;
      --remaining;
    }
    if (remaining != 0) {
      break;  // truncated SEI: nothing after it can be trusted
    }
    if (!visit(payloadType, payloadStart, p)) {
      return;
    }
    // The encoder inserts the escape byte before any byte <= 0x03 that follows two zeros, including
    // the byte *after* this payload - the next message's payloadType. It sits on the boundary, so
    // walking the payload's own bytes leaves it behind and the next header would be read from it
    // (picture_timing is payloadType 1), derailing the rest of the scan.
    if (zeros >= 2 && p < end && *p == 0x03) {
      ++p;
    }
  }
}

// The inverse of Unescape, for the one caller that rewrites a payload (PlanVividCurveStrip): the
// shortened payload can need an escape byte the original did not have. 0 when it would not fit.
size_t Escape(const uint8_t* in, size_t n, uint8_t* out, size_t capacity) {
  size_t zeros = 0;
  size_t written = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = in[i];
    if (zeros >= 2 && b <= 0x03) {
      if (written >= capacity) {
        return 0;
      }
      out[written++] = 0x03;
      zeros = 0;
    }
    if (written >= capacity) {
      return 0;
    }
    out[written++] = b;
    zeros = b == 0x00 ? zeros + 1 : 0;
  }
  return written;
}

// ---- HDR10+ (the base version's own extra dynamic layer) ----

// The registration that identifies the Samsung ST 2094-40 (HDR10+) provider: itu_t_t35_country_code
// and itu_t_t35_terminal_provider_code. The payload that follows is a full HDR10+ parameter set.
constexpr size_t kHdr10PlusRegistrationSize = 3;
constexpr uint8_t kHdr10PlusRegistration[kHdr10PlusRegistrationSize] = {0xB5, 0x00, 0x3C};

bool IsHdr10PlusPayload(const uint8_t* payload, size_t size) {
  return size >= kHdr10PlusRegistrationSize &&
         std::memcmp(payload, kHdr10PlusRegistration, kHdr10PlusRegistrationSize) == 0;
}

// Whether the NAL carries HDR10+ and nothing else this side has to keep. A message with no payload
// cannot be a payload we keep, so it does not save the NAL either.
void ClassifySeiNal(const uint8_t* nal, size_t size, bool* hasHdr10Plus, bool* hasOther) {
  *hasHdr10Plus = false;
  *hasOther = false;
  WalkSeiMessages(nal, size, [&](uint32_t payloadType, const uint8_t* start, const uint8_t* end) {
    const size_t payloadSize = static_cast<size_t>(end - start);
    if (payloadSize != 0) {
      if (payloadType == 4 && IsHdr10PlusPayload(start, payloadSize)) {
        *hasHdr10Plus = true;
      } else {
        *hasOther = true;
      }
    }
    return true;  // the verdict needs the whole NAL
  });
}

// ---- the tone-mapping curve the base version writes into PQ Vivid ----

// The payload up to and including the four 12-bit statistics: envelope, system_start_code, 4 x 12
// bits. Byte 12 is the one that carries tone_mapping_mode_flag (its top bit) and
// color_saturation_mapping_flag; the statistics-only form ends right there.
constexpr size_t kStatisticOnlyVividPayloadSize = kVividEnvelopeSize + 1 + 6 + 1;

bool IsVividPayload(const uint8_t* payload, size_t size) {
  return size >= kVividEnvelopeSize && std::memcmp(payload, kVividEnvelope, kVividEnvelopeSize) == 0;
}

// The wire span of a NAL's only SEI payload when that payload is HDR Vivid's. False when the NAL
// carries another payload too - the rewrite would then have to re-escape around it, and no host
// build writes that shape (the encoder puts each injected payload in an access unit of its own).
bool SoleVividSeiPayload(const uint8_t* nal, size_t size, const uint8_t** spanStart,
                         const uint8_t** spanEnd) {
  const uint8_t* start = nullptr;
  const uint8_t* end = nullptr;
  int payloads = 0;
  WalkSeiMessages(nal, size, [&](uint32_t payloadType, const uint8_t* p, const uint8_t* q) {
    if (static_cast<size_t>(q - p) == 0) {
      return true;  // padding, not a payload
    }
    if (++payloads == 1 && payloadType == 4 && IsVividPayload(p, static_cast<size_t>(q - p))) {
      start = p;
      end = q;
    }
    return true;
  });
  if (payloads != 1 || start == nullptr) {
    return false;
  }
  *spanStart = start;
  *spanEnd = end;
  return true;
}

void ScanHevcNal(const uint8_t* nal, size_t size, VividMetadata* vivid) {
  if (IsSeiNal(nal, size)) {
    WalkSeiMessages(nal, size, [vivid](uint32_t payloadType, const uint8_t* start, const uint8_t* end) {
      if (payloadType == 4) {  // user_data_registered_itu_t_t35
        StoreVivid(start, static_cast<size_t>(end - start), true, vivid);
      }
      return true;
    });
  }
}

VividMetadata ScanHevc(const uint8_t* data, size_t size) {
  VividMetadata vivid;
  WalkNals(data, size, [&vivid](size_t, const uint8_t* nal, size_t nalSize) {
    ScanHevcNal(nal, nalSize, &vivid);
    return true;
  });
  return vivid;
}

// ---- AV1 (low-overhead OBU) ----

bool ReadLeb128(const uint8_t* data, size_t size, size_t* at, uint64_t* value) {
  uint64_t result = 0;
  int shift = 0;
  while (*at < size && shift <= 56) {
    const uint8_t b = data[(*at)++];
    result |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

VividMetadata ScanAv1(const uint8_t* data, size_t size) {
  VividMetadata vivid;
  size_t at = 0;
  while (at < size) {
    const uint8_t header = data[at++];
    if ((header & 0x80) != 0) {  // forbidden bit
      break;
    }
    const int obuType = (header >> 3) & 0x0F;
    const bool hasExtension = (header & 0x04) != 0;
    const bool hasSize = (header & 0x02) != 0;
    if (hasExtension) {
      if (at >= size) {
        break;
      }
      ++at;  // an OBU extension header; metadata OBUs do not use one
    }
    uint64_t payloadSize = size - at;
    if (hasSize && (!ReadLeb128(data, size, &at, &payloadSize) || payloadSize > size - at)) {
      break;
    }
    if (obuType == 5) {  // OBU_METADATA
      size_t inner = at;
      const size_t innerEnd = at + static_cast<size_t>(payloadSize);
      uint64_t metadataType = 0;
      if (ReadLeb128(data, innerEnd, &inner, &metadataType) && metadataType == 4) {  // ITU-T T.35
        StoreVivid(data + inner, innerEnd - inner, false, &vivid);
      }
    }
    at += static_cast<size_t>(payloadSize);
  }
  return vivid;
}

}  // namespace

VividMetadata ScanVividMetadata(const uint8_t* data, size_t size, const char* mime) {
  if (data == nullptr || size == 0 || mime == nullptr) {
    return {};
  }
  if (std::strcmp(mime, "video/hevc") == 0) {
    return ScanHevc(data, size);
  }
  if (std::strcmp(mime, "video/av1") == 0) {
    return ScanAv1(data, size);
  }
  return {};
}

bool PlanHdr10PlusRemoval(const uint8_t* data, size_t size, const char* mime,
                         Hdr10PlusRemoval* removal) {
  if (data == nullptr || size == 0 || mime == nullptr || removal == nullptr) {
    return false;
  }
  // AV1 carries HDR10+ in a metadata OBU the platform's model has no room for either, but this side
  // chooses AV1 for HDR only when it cannot decode HEVC Main10, so the Annex-B carriage is the one
  // that shows up; an OBU reader can be added if that changes.
  if (std::strcmp(mime, "video/hevc") != 0) {
    return false;
  }
  bool planned = false;
  WalkNals(data, size, [&](size_t unitOffset, const uint8_t* nal, size_t nalSize) {
    if (!IsSeiNal(nal, nalSize)) {
      return true;
    }
    bool hasHdr10Plus = false;
    bool hasOther = false;
    ClassifySeiNal(nal, nalSize, &hasHdr10Plus, &hasOther);
    if (hasHdr10Plus && !hasOther) {
      removal->offset = unitOffset;
      removal->length = static_cast<size_t>(nal - data) + nalSize - unitOffset;
      planned = true;
      return false;
    }
    return true;
  });
  return planned;
}

bool PlanVividCurveStrip(const uint8_t* data, size_t size, const char* mime,
                         VividCurveStrip* strip) {
  if (data == nullptr || size == 0 || mime == nullptr || strip == nullptr) {
    return false;
  }
  if (std::strcmp(mime, "video/hevc") != 0) {
    return false;
  }
  bool planned = false;
  WalkNals(data, size, [&](size_t, const uint8_t* nal, size_t nalSize) {
    if (planned || !IsSeiNal(nal, nalSize)) {
      return true;
    }
    const uint8_t* payloadStart = nullptr;
    const uint8_t* payloadEnd = nullptr;
    if (!SoleVividSeiPayload(nal, nalSize, &payloadStart, &payloadEnd)) {
      return true;
    }
    // The size field is rewritten along with the payload, so it has to be the single byte this shape
    // has: the byte before the payload is the size, and the one before that is the payload type.
    if (payloadStart - data < 2 || payloadStart[-1] == 0xFF || payloadStart[-2] == 0xFF) {
      return true;
    }
    uint8_t rbsp[kMaxVividPayload] = {};
    size_t clean = 0;
    if (!Unescape(payloadStart, static_cast<size_t>(payloadEnd - payloadStart), rbsp, sizeof(rbsp),
                  &clean) ||
        clean < kStatisticOnlyVividPayloadSize) {
      return true;
    }
    // Already statistics-only (HLG's form, and a host that stopped writing the curve): nothing to do.
    if ((rbsp[kStatisticOnlyVividPayloadSize - 1] & 0x80) == 0) {
      return true;
    }
    rbsp[kStatisticOnlyVividPayloadSize - 1] = 0x00;  // both flags off: statistics only
    uint8_t escaped[kMaxVividPayload] = {};
    const size_t escapedSize =
        Escape(rbsp, kStatisticOnlyVividPayloadSize, escaped, sizeof(escaped));
    if (escapedSize == 0) {
      return true;
    }
    strip->offset = static_cast<size_t>(payloadStart - data) - 1;  // the size field as well
    strip->length = static_cast<size_t>(payloadEnd - payloadStart) + 1;
    strip->payload[0] = static_cast<uint8_t>(kStatisticOnlyVividPayloadSize);
    std::memcpy(strip->payload + 1, escaped, escapedSize);
    strip->payloadSize = escapedSize + 1;
    planned = true;
    return false;
  });
  return planned;
}

}
