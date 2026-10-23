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

void ScanHevcNal(const uint8_t* nal, size_t size, VividMetadata* vivid) {
  if (size < 3) {
    return;
  }
  const int nalType = (nal[0] >> 1) & 0x3F;
  if (nalType != 39 && nalType != 40) {  // PREFIX_SEI / SUFFIX_SEI
    return;
  }
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
    // inserts, and a Vivid payload is mostly zero fields (00 00 -> 00 00 03 is common at every
    // value boundary). Advancing by the raw size would land mid-payload and lose whatever follows
    // it - including the payload itself when the unit carries more than one SEI message.
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
    if (payloadType == 4) {  // user_data_registered_itu_t_t35
      StoreVivid(payloadStart, static_cast<size_t>(p - payloadStart), true, vivid);
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

VividMetadata ScanHevc(const uint8_t* data, size_t size) {
  VividMetadata vivid;
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
    if (nalEnd > nalStart) {
      ScanHevcNal(data + nalStart, nalEnd - nalStart, &vivid);
    }
    at = next;
    codeLength = nextCode;
  }
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

}
