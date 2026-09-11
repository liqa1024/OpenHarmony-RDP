/*
 * HmRdp - RemoteFX / Progressive container parser (see hmrdp_rfx.h).
 */
#include "hmrdp_rfx.h"

#include <cstdio>
#include <cstdlib>

namespace hmrdp {
namespace {

// Progressive block types (libfreerdp/codec/rfx_constants.h).
constexpr uint16_t kWbtSync = 0xCCC0;
constexpr uint16_t kWbtFrameBegin = 0xCCC1;
constexpr uint16_t kWbtFrameEnd = 0xCCC2;
constexpr uint16_t kWbtContext = 0xCCC3;
constexpr uint16_t kWbtRegion = 0xCCC4;
constexpr uint16_t kWbtTileSimple = 0xCCC5;
constexpr uint16_t kWbtTileFirst = 0xCCC6;
constexpr uint16_t kWbtTileUpgrade = 0xCCC7;

inline uint16_t ReadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ParseTile(const uint8_t* data, size_t size, RfxTileRef* tile, RfxParseStats* stats) {
  if (size < 6) {
    return false;
  }
  const uint16_t blockType = ReadU16(data);
  const uint32_t blockLen = ReadU32(data + 2);
  if (blockLen < 6 || static_cast<size_t>(blockLen) > size) {
    return false;
  }
  const uint8_t* p = data + 6;
  const size_t len = static_cast<size_t>(blockLen) - 6;

  if (blockType == kWbtTileFirst || blockType == kWbtTileSimple) {
    const bool simple = (blockType == kWbtTileSimple);
    const size_t headerLen = simple ? 16 : 17;
    if (len < headerLen) {
      return false;
    }
    tile->type = simple ? RfxTileType::kSimple : RfxTileType::kFirst;
    tile->quantIdxY = p[0];
    tile->quantIdxCb = p[1];
    tile->quantIdxCr = p[2];
    tile->xIdx = ReadU16(p + 3);
    tile->yIdx = ReadU16(p + 5);
    tile->flags = p[7];
    size_t h = 8;
    if (!simple) {
      tile->quality = p[h];
      h += 1;
    } else {
      tile->quality = 0xFF;
    }
    tile->yLen = ReadU16(p + h);
    tile->cbLen = ReadU16(p + h + 2);
    tile->crLen = ReadU16(p + h + 4);
    tile->tailLen = ReadU16(p + h + 6);
    h += 8;
    const size_t need = h + static_cast<size_t>(tile->yLen) + tile->cbLen + tile->crLen +
                        tile->tailLen;
    if (need > len) {
      return false;
    }
    tile->yData = p + h;
    h += tile->yLen;
    tile->cbData = p + h;
    h += tile->cbLen;
    tile->crData = p + h;
    h += tile->crLen;
    tile->tailData = p + h;
  } else if (blockType == kWbtTileUpgrade) {
    if (len < 20) {
      return false;
    }
    tile->type = RfxTileType::kUpgrade;
    tile->quantIdxY = p[0];
    tile->quantIdxCb = p[1];
    tile->quantIdxCr = p[2];
    tile->xIdx = ReadU16(p + 3);
    tile->yIdx = ReadU16(p + 5);
    tile->quality = p[7];
    tile->ySrlLen = ReadU16(p + 8);
    tile->yRawLen = ReadU16(p + 10);
    tile->cbSrlLen = ReadU16(p + 12);
    tile->cbRawLen = ReadU16(p + 14);
    tile->crSrlLen = ReadU16(p + 16);
    tile->crRawLen = ReadU16(p + 18);
    size_t h = 20;
    const size_t need = h + static_cast<size_t>(tile->ySrlLen) + tile->yRawLen + tile->cbSrlLen +
                        tile->cbRawLen + tile->crSrlLen + tile->crRawLen;
    if (need > len) {
      return false;
    }
    tile->ySrlData = p + h;
    h += tile->ySrlLen;
    tile->yRawData = p + h;
    h += tile->yRawLen;
    tile->cbSrlData = p + h;
    h += tile->cbSrlLen;
    tile->cbRawData = p + h;
    h += tile->cbRawLen;
    tile->crSrlData = p + h;
    h += tile->crSrlLen;
    tile->crRawData = p + h;
  } else {
    return false;
  }

  if (stats != nullptr) {
    stats->tiles++;
    if (tile->type == RfxTileType::kFirst) {
      stats->firstTiles++;
    } else if (tile->type == RfxTileType::kSimple) {
      stats->simpleTiles++;
    } else {
      stats->upgradeTiles++;
    }
    if (tile->xIdx > stats->maxTileX) {
      stats->maxTileX = tile->xIdx;
    }
    if (tile->yIdx > stats->maxTileY) {
      stats->maxTileY = tile->yIdx;
    }
    if ((tile->flags & 0x01u) != 0) {
      stats->diffTiles++;
    }
  }
  return true;
}

void ReadQuantNibbles(const uint8_t* b, RfxQuant* q) {
  q->LL3 = b[0] & 0x0F;
  q->HL3 = b[0] >> 4;
  q->LH3 = b[1] & 0x0F;
  q->HH3 = b[1] >> 4;
  q->HL2 = b[2] & 0x0F;
  q->LH2 = b[2] >> 4;
  q->HH2 = b[3] & 0x0F;
  q->HL1 = b[3] >> 4;
  q->LH1 = b[4] & 0x0F;
  q->HH1 = b[4] >> 4;
}

bool ParseRegion(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                 RfxParseStats* stats) {
  if (size < 12) {
    return false;
  }
  const uint8_t tileSize = data[0];
  const uint16_t numRects = ReadU16(data + 1);
  const uint8_t numQuant = data[3];
  const uint8_t numProgQuant = data[4];
  const uint8_t regionFlags = data[5];
  const uint16_t numTiles = ReadU16(data + 6);
  const uint32_t tileDataSize = ReadU32(data + 8);
  if (tileSize != 64 || numQuant > 7) {
    return false;
  }
  size_t p = 12;
  // rects (8 bytes each).
  if (p + static_cast<size_t>(numRects) * 8 > size) {
    return false;
  }
  p += static_cast<size_t>(numRects) * 8;
  // Component quant tables: 5 bytes -> 10 x 4-bit shifts each.
  RfxQuant quants[7];
  if (p + static_cast<size_t>(numQuant) * 5 > size) {
    return false;
  }
  for (uint8_t q = 0; q < numQuant; ++q) {
    ReadQuantNibbles(data + p, &quants[q]);
    p += 5;
  }
  // Progressive quant tables: 16 bytes each (quality + Y/Cb/Cr quant).
  RfxProgQuant progQuants[16];
  if (numProgQuant > 16 || p + static_cast<size_t>(numProgQuant) * 16 > size) {
    return false;
  }
  for (uint8_t q = 0; q < numProgQuant; ++q) {
    const uint8_t* b = data + p;
    progQuants[q].quality = b[0];
    ReadQuantNibbles(b + 1, &progQuants[q].y);
    ReadQuantNibbles(b + 6, &progQuants[q].cb);
    ReadQuantNibbles(b + 11, &progQuants[q].cr);
    p += 16;
  }
  if (p + tileDataSize > size) {
    return false;
  }
  const size_t tileEnd = p + tileDataSize;

  if (stats != nullptr) {
    stats->regions++;
    stats->rects += numRects;
    if ((regionFlags & 0x01u) != 0) {
      stats->extrapolateRegions++;
    }
  }

  uint32_t count = 0;
  while (p + 6 <= tileEnd && count < numTiles) {
    const uint32_t blockLen = ReadU32(data + p + 2);
    if (blockLen < 6 || p + blockLen > tileEnd) {
      return false;
    }
    RfxTileRef tile;
    if (!ParseTile(data + p, blockLen, &tile, stats)) {
      return false;
    }
    tile.quants = quants;
    tile.numQuant = numQuant;
    tile.progQuants = progQuants;
    tile.numProgQuant = numProgQuant;
    tile.regionFlags = regionFlags;
    if (onTile) {
      onTile(tile);
    }
    p += blockLen;
    count++;
  }
  return count == numTiles;
}

}  // namespace

// ---------------------------------------------------------------------------
// RLGR decoder (ported from libfreerdp/codec/rfx_rlgr.c, Apache-2.0).
// ---------------------------------------------------------------------------

constexpr int kKpMax = 80;  // max value for kp / krp
constexpr int kLsgr = 3;    // convert kp -> k; k = kp >> kLsgr
constexpr int kUpGr = 4;    // += after a zero run in RL mode
constexpr int kDnGr = 6;    // -= after a nonzero symbol in RL mode
constexpr int kUqGr = 3;    // += after nonzero symbol in GR mode
constexpr int kDqGr = 3;    // -= after zero symbol in GR mode

inline int Lzcnt32(uint32_t x) {
  if (x == 0) {
    return 32;
  }
  int n = 0;
  while ((x & 0x80000000u) == 0) {
    x <<= 1;
    ++n;
  }
  return n;
}

// MSB-first bit reader mirroring FreeRDP's wBitStream 32-bit accumulator. A
// 64-bit buffer is used so refilling works on non-byte-aligned positions.
class MsbBitReader {
 public:
  MsbBitReader(const uint8_t* data, size_t size) : data_(data), size_(size) { Fetch(); }

  uint32_t Peek() const { return static_cast<uint32_t>(acc_ >> 32); }
  size_t Remaining() const { return static_cast<size_t>(accBits_) + (size_ - pos_) * 8; }

  void Shift(int n) {
    if (n <= 0) {
      return;
    }
    if (n >= 64) {
      acc_ = 0;
      accBits_ = 0;
    } else {
      acc_ <<= n;
      accBits_ -= n;
      if (accBits_ < 0) {
        accBits_ = 0;
      }
    }
    Fetch();
  }

 private:
  void Fetch() {
    while (accBits_ <= 32 && pos_ < size_) {
      acc_ |= static_cast<uint64_t>(data_[pos_++]) << (56 - accBits_);
      accBits_ += 8;
    }
  }

  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  uint64_t acc_ = 0;
  int accBits_ = 0;
};

bool RlgrDecode(RlgrMode mode, const uint8_t* src, size_t size, int16_t* dst, size_t dstCount,
                size_t* remainingBits) {
  if (src == nullptr || size == 0 || dst == nullptr || dstCount == 0) {
    return false;
  }
  MsbBitReader br(src, size);
  int k = 1;
  int kp = k << kLsgr;
  int kr = 1;
  int krp = kr << kLsgr;
  size_t out = 0;

  while (br.Remaining() > 0 && out < dstCount) {
    if (k != 0) {
      // Run-Length (RL) mode.
      size_t run = 0;
      int cnt = Lzcnt32(br.Peek());
      size_t nbits = br.Remaining();
      if (static_cast<size_t>(cnt) > nbits) {
        cnt = static_cast<int>(nbits);
      }
      int vk = cnt;
      while (cnt == 32 && br.Remaining() > 0) {
        br.Shift(32);
        cnt = Lzcnt32(br.Peek());
        nbits = br.Remaining();
        if (static_cast<size_t>(cnt) > nbits) {
          cnt = static_cast<int>(nbits);
        }
        vk += cnt;
      }
      br.Shift(vk % 32);
      if (br.Remaining() < 1) {
        break;
      }
      br.Shift(1);
      while (vk-- > 0) {
        run += static_cast<size_t>(1) << k;
        kp += kUpGr;
        if (kp > kKpMax) {
          kp = kKpMax;
        }
        k = kp >> kLsgr;
      }
      if (br.Remaining() < static_cast<size_t>(k)) {
        break;
      }
      const uint32_t maskK = (k > 0) ? ((1u << k) - 1u) : 0u;
      run += (k > 0) ? ((br.Peek() >> (32 - k)) & maskK) : 0u;
      br.Shift(k);
      if (br.Remaining() < 1) {
        break;
      }
      const uint32_t sign = (br.Peek() & 0x80000000u) ? 1u : 0u;
      br.Shift(1);

      cnt = Lzcnt32(~br.Peek());
      nbits = br.Remaining();
      if (static_cast<size_t>(cnt) > nbits) {
        cnt = static_cast<int>(nbits);
      }
      vk = cnt;
      while (cnt == 32 && br.Remaining() > 0) {
        br.Shift(32);
        cnt = Lzcnt32(~br.Peek());
        nbits = br.Remaining();
        if (static_cast<size_t>(cnt) > nbits) {
          cnt = static_cast<int>(nbits);
        }
        vk += cnt;
      }
      br.Shift(vk % 32);
      if (br.Remaining() < 1) {
        break;
      }
      br.Shift(1);
      if (br.Remaining() < static_cast<size_t>(kr)) {
        break;
      }
      const uint32_t maskR = (kr > 0) ? ((1u << kr) - 1u) : 0u;
      uint16_t code = (kr > 0) ? static_cast<uint16_t>((br.Peek() >> (32 - kr)) & maskR) : 0;
      br.Shift(kr);
      code = static_cast<uint16_t>(code | static_cast<uint16_t>(vk << kr));

      if (vk == 0) {
        krp -= 2;
        if (krp < 0) {
          krp = 0;
        }
        kr = krp >> kLsgr;
      } else if (vk != 1) {
        krp += vk;
        if (krp > kKpMax) {
          krp = kKpMax;
        }
        kr = krp >> kLsgr;
      }
      kp -= kDnGr;
      if (kp < 0) {
        kp = 0;
      }
      k = kp >> kLsgr;

      const int16_t mag =
          sign ? static_cast<int16_t>(-static_cast<int32_t>(code + 1))
               : static_cast<int16_t>(code + 1);
      size_t sz = run;
      if (out + sz > dstCount) {
        sz = dstCount - out;
      }
      for (size_t i = 0; i < sz; ++i) {
        dst[out++] = 0;
      }
      if (out < dstCount) {
        dst[out++] = mag;
      }
    } else {
      // Golomb-Rice (GR) mode.
      int cnt = Lzcnt32(~br.Peek());
      size_t nbits = br.Remaining();
      if (static_cast<size_t>(cnt) > nbits) {
        cnt = static_cast<int>(nbits);
      }
      int vk = cnt;
      while (cnt == 32 && br.Remaining() > 0) {
        br.Shift(32);
        cnt = Lzcnt32(~br.Peek());
        nbits = br.Remaining();
        if (static_cast<size_t>(cnt) > nbits) {
          cnt = static_cast<int>(nbits);
        }
        vk += cnt;
      }
      br.Shift(vk % 32);
      if (br.Remaining() < 1) {
        break;
      }
      br.Shift(1);
      if (br.Remaining() < static_cast<size_t>(kr)) {
        break;
      }
      const uint32_t maskR = (kr > 0) ? ((1u << kr) - 1u) : 0u;
      uint16_t code = (kr > 0) ? static_cast<uint16_t>((br.Peek() >> (32 - kr)) & maskR) : 0;
      br.Shift(kr);
      code = static_cast<uint16_t>(code | static_cast<uint16_t>(vk << kr));

      if (vk == 0) {
        krp -= 2;
        if (krp < 0) {
          krp = 0;
        }
        kr = krp >> kLsgr;
      } else if (vk != 1) {
        krp += vk;
        if (krp > kKpMax) {
          krp = kKpMax;
        }
        kr = krp >> kLsgr;
      }

      if (mode == RlgrMode::kRlgr1) {
        int16_t mag = 0;
        if (code == 0) {
          kp += kUqGr;
          if (kp > kKpMax) {
            kp = kKpMax;
          }
          k = kp >> kLsgr;
          mag = 0;
        } else {
          kp -= kDqGr;
          if (kp < 0) {
            kp = 0;
          }
          k = kp >> kLsgr;
          mag = (code & 1) ? static_cast<int16_t>(-static_cast<int32_t>((code + 1) >> 1))
                           : static_cast<int16_t>(code >> 1);
        }
        if (out < dstCount) {
          dst[out++] = mag;
        }
      } else {
        uint32_t nIdx = 0;
        if (code != 0) {
          nIdx = static_cast<uint32_t>(32 - Lzcnt32(code));
        }
        if (br.Remaining() < nIdx) {
          break;
        }
        const uint32_t maskN = (nIdx > 0) ? ((1u << nIdx) - 1u) : 0u;
        const uint32_t val1 = (nIdx > 0) ? ((br.Peek() >> (32 - nIdx)) & maskN) : 0u;
        br.Shift(static_cast<int>(nIdx));
        const uint32_t val2 = static_cast<uint32_t>(code) - val1;
        if (val1 != 0 && val2 != 0) {
          kp -= 2 * kDqGr;
          if (kp < 0) {
            kp = 0;
          }
          k = kp >> kLsgr;
        } else if (val1 == 0 && val2 == 0) {
          kp += 2 * kUqGr;
          if (kp > kKpMax) {
            kp = kKpMax;
          }
          k = kp >> kLsgr;
        }
        const int16_t mag1 = (val1 & 1)
                                 ? static_cast<int16_t>(-static_cast<int32_t>((val1 + 1) >> 1))
                                 : static_cast<int16_t>(val1 >> 1);
        if (out < dstCount) {
          dst[out++] = mag1;
        }
        const int16_t mag2 = (val2 & 1)
                                 ? static_cast<int16_t>(-static_cast<int32_t>((val2 + 1) >> 1))
                                 : static_cast<int16_t>(val2 >> 1);
        if (out < dstCount) {
          dst[out++] = mag2;
        }
      }
    }
  }

  while (out < dstCount) {
    dst[out++] = 0;
  }
  if (remainingBits != nullptr) {
    *remainingBits = br.Remaining();
  }
  return true;
}

// ---------------------------------------------------------------------------
// RemoteFX tile pixel decode (ported from libfreerdp/codec/rfx_*.c,
// primitives/prim_colors.c; Apache-2.0). CPU reference for the GPU path.
// ---------------------------------------------------------------------------

namespace {

inline int Clip255(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// rfx_differential_decode: cumulative sum over the last `size` coefficients.
void DifferentialDecode(int16_t* buffer, size_t size) {
  for (size_t i = 0; i + 1 < size; ++i) {
    buffer[i + 1] = static_cast<int16_t>(buffer[i + 1] + buffer[i]);
  }
}

void DequantSubband(int16_t* buffer, size_t offset, size_t length, uint8_t shift) {
  if (shift == 0) {
    return;
  }
  for (size_t i = 0; i < length; ++i) {
    buffer[offset + i] = static_cast<int16_t>(buffer[offset + i] << shift);
  }
}

// rfx_dwt_2d_decode_block: one level of the reversible 5/3 inverse DWT.
void DwtDecodeBlock(int16_t* buffer, int16_t* idwt, size_t subbandWidth) {
  const size_t totalWidth = subbandWidth << 1;
  const int16_t* ll = buffer + subbandWidth * subbandWidth * 3;
  const int16_t* hl = buffer;
  int16_t* lDst = idwt;
  const int16_t* lh = buffer + subbandWidth * subbandWidth;
  const int16_t* hh = buffer + subbandWidth * subbandWidth * 2;
  int16_t* hDst = idwt + subbandWidth * subbandWidth * 2;

  for (size_t y = 0; y < subbandWidth; ++y) {
    lDst[0] = static_cast<int16_t>(ll[0] - ((hl[0] + hl[0] + 1) >> 1));
    hDst[0] = static_cast<int16_t>(lh[0] - ((hh[0] + hh[0] + 1) >> 1));
    for (size_t n = 1; n < subbandWidth; ++n) {
      const size_t x = n << 1;
      lDst[x] = static_cast<int16_t>(ll[n] - ((hl[n - 1] + hl[n] + 1) >> 1));
      hDst[x] = static_cast<int16_t>(lh[n] - ((hh[n - 1] + hh[n] + 1) >> 1));
    }
    size_t n = 0;
    for (; n < subbandWidth - 1; ++n) {
      const size_t x = n << 1;
      lDst[x + 1] = static_cast<int16_t>((hl[n] << 1) + ((lDst[x] + lDst[x + 2]) >> 1));
      hDst[x + 1] = static_cast<int16_t>((hh[n] << 1) + ((hDst[x] + hDst[x + 2]) >> 1));
    }
    const size_t x = n << 1;
    lDst[x + 1] = static_cast<int16_t>((hl[n] << 1) + lDst[x]);
    hDst[x + 1] = static_cast<int16_t>((hh[n] << 1) + hDst[x]);

    ll += subbandWidth;
    hl += subbandWidth;
    lDst += totalWidth;
    lh += subbandWidth;
    hh += subbandWidth;
    hDst += totalWidth;
  }

  for (size_t x = 0; x < totalWidth; ++x) {
    const int16_t* l = idwt + x;
    const int16_t* h = idwt + x + subbandWidth * totalWidth;
    int16_t* dst = buffer + x;
    *dst = static_cast<int16_t>(*l - ((*h * 2 + 1) >> 1));
    for (size_t n = 1; n < subbandWidth; ++n) {
      l += totalWidth;
      h += totalWidth;
      dst[2 * totalWidth] = static_cast<int16_t>(*l - ((*(h - totalWidth) + *h + 1) >> 1));
      dst[totalWidth] = static_cast<int16_t>((*(h - totalWidth) << 1) +
                                             ((*dst + dst[2 * totalWidth]) >> 1));
      dst += 2 * totalWidth;
    }
    dst[totalWidth] = static_cast<int16_t>((*h << 1) + ((*dst * 2) >> 1));
  }
}

void InverseDwt2d(int16_t* buffer, int16_t* temp) {
  DwtDecodeBlock(&buffer[3840], temp, 8);
  DwtDecodeBlock(&buffer[3072], temp, 16);
  DwtDecodeBlock(&buffer[0], temp, 32);
}

// YCbCr(16s) -> BGRA (matches primitives yCbCrToRGB_16s8u_P3AC4R_BGRX).
void YCbCrToBgra(const int16_t* y, const int16_t* cb, const int16_t* cr, uint8_t* dst,
                 int dstStride) {
  for (int row = 0; row < 64; ++row) {
    for (int col = 0; col < 64; ++col) {
      const int i = row * 64 + col;
      const int32_t yv = static_cast<int32_t>(static_cast<uint32_t>(y[i] + 4096) << 16);
      const int32_t cbv = cb[i];
      const int32_t crv = cr[i];
      const int64_t crr = static_cast<int64_t>(crv) * static_cast<int64_t>(1.402525f * 65536.0f);
      const int64_t crg = static_cast<int64_t>(crv) * static_cast<int64_t>(0.714401f * 65536.0f);
      const int64_t cbg = static_cast<int64_t>(cbv) * static_cast<int64_t>(0.343730f * 65536.0f);
      const int64_t cbb = static_cast<int64_t>(cbv) * static_cast<int64_t>(1.769905f * 65536.0f);
      const int r = static_cast<int16_t>((crr + yv) >> 16) >> 5;
      const int g = static_cast<int16_t>((yv - cbg - crg) >> 16) >> 5;
      const int b = static_cast<int16_t>((cbb + yv) >> 16) >> 5;
      uint8_t* p = dst + static_cast<size_t>(row) * dstStride + col * 4;
      p[0] = static_cast<uint8_t>(Clip255(b));
      p[1] = static_cast<uint8_t>(Clip255(g));
      p[2] = static_cast<uint8_t>(Clip255(r));
      p[3] = 0xFF;
    }
  }
}

}  // namespace

// --- Reduced-band (extrapolate) inverse DWT, from progressive.c ---------------

static int ProgBandL(size_t level) {
  return static_cast<int>((64 >> level) + 1);
}

static int ProgBandH(size_t level) {
  if (level == 1) {
    return (64 >> 1) - 1;
  }
  return static_cast<int>((64 + (1 << (level - 1))) >> level);
}

static void ProgIdwtX(const int16_t* pLowBand, size_t nLowStep, const int16_t* pHighBand,
                      size_t nHighStep, int16_t* pDstBand, size_t nDstStep, size_t nLowCount,
                      size_t nHighCount, size_t nDstCount) {
  for (size_t i = 0; i < nDstCount; ++i) {
    const int16_t* pL = pLowBand;
    const int16_t* pH = pHighBand;
    int16_t* pX = pDstBand;
    int16_t H0 = *pH++;
    int16_t L0 = *pL++;
    int16_t X0 = static_cast<int16_t>(L0 - H0);
    int16_t X2 = static_cast<int16_t>(L0 - H0);
    for (size_t j = 0; j < (nHighCount - 1); ++j) {
      const int16_t H1 = *pH++;
      L0 = *pL++;
      X2 = static_cast<int16_t>(L0 - ((H0 + H1) / 2));
      const int16_t X1 = static_cast<int16_t>(((X0 + X2) / 2) + (2 * H0));
      pX[0] = X0;
      pX[1] = X1;
      pX += 2;
      X0 = X2;
      H0 = H1;
    }
    if (nLowCount <= (nHighCount + 1)) {
      if (nLowCount <= nHighCount) {
        pX[0] = X2;
        pX[1] = static_cast<int16_t>(X2 + (2 * H0));
      } else {
        L0 = *pL++;
        X0 = static_cast<int16_t>(L0 - H0);
        pX[0] = X2;
        pX[1] = static_cast<int16_t>(((X0 + X2) / 2) + (2 * H0));
        pX[2] = X0;
      }
    } else {
      L0 = *pL++;
      X0 = static_cast<int16_t>(L0 - (H0 / 2));
      pX[0] = X2;
      pX[1] = static_cast<int16_t>(((X0 + X2) / 2) + (2 * H0));
      pX[2] = X0;
      L0 = *pL++;
      pX[3] = static_cast<int16_t>((X0 + L0) / 2);
    }
    pLowBand += nLowStep;
    pHighBand += nHighStep;
    pDstBand += nDstStep;
  }
}

static void ProgIdwtY(const int16_t* pLowBand, size_t nLowStep, const int16_t* pHighBand,
                      size_t nHighStep, int16_t* pDstBand, size_t nDstStep, size_t nLowCount,
                      size_t nHighCount, size_t nDstCount) {
  for (size_t i = 0; i < nDstCount; ++i) {
    const int16_t* pL = pLowBand;
    const int16_t* pH = pHighBand;
    int16_t* pX = pDstBand;
    int16_t H0 = *pH;
    pH += nHighStep;
    int16_t L0 = *pL;
    pL += nLowStep;
    int16_t X0 = static_cast<int16_t>(L0 - H0);
    int16_t X2 = static_cast<int16_t>(L0 - H0);
    for (size_t j = 0; j < (nHighCount - 1); ++j) {
      const int16_t H1 = *pH;
      pH += nHighStep;
      L0 = *pL;
      pL += nLowStep;
      X2 = static_cast<int16_t>(L0 - ((H0 + H1) / 2));
      const int16_t X1 = static_cast<int16_t>(((X0 + X2) / 2) + (2 * H0));
      *pX = X0;
      pX += nDstStep;
      *pX = X1;
      pX += nDstStep;
      X0 = X2;
      H0 = H1;
    }
    if (nLowCount <= (nHighCount + 1)) {
      if (nLowCount <= nHighCount) {
        *pX = X2;
        pX += nDstStep;
        *pX = static_cast<int16_t>(X2 + (2 * H0));
      } else {
        L0 = *pL;
        X0 = static_cast<int16_t>(L0 - H0);
        *pX = X2;
        pX += nDstStep;
        *pX = static_cast<int16_t>(((X0 + X2) / 2) + (2 * H0));
        pX += nDstStep;
        *pX = X0;
      }
    } else {
      L0 = *pL;
      pL += nLowStep;
      X0 = static_cast<int16_t>(L0 - (H0 / 2));
      *pX = X2;
      pX += nDstStep;
      *pX = static_cast<int16_t>(((X0 + X2) / 2) + (2 * H0));
      pX += nDstStep;
      *pX = X0;
      pX += nDstStep;
      L0 = *pL;
      *pX = static_cast<int16_t>((X0 + L0) / 2);
    }
    ++pLowBand;
    ++pHighBand;
    ++pDstBand;
  }
}

static void ProgDwtBlock(int16_t* buffer, int16_t* temp, size_t level) {
  const size_t nBandL = static_cast<size_t>(ProgBandL(level));
  const size_t nBandH = static_cast<size_t>(ProgBandH(level));
  size_t offset = 0;
  const int16_t* HL = &buffer[offset];
  offset += nBandH * nBandL;
  const int16_t* LH = &buffer[offset];
  offset += nBandL * nBandH;
  const int16_t* HH = &buffer[offset];
  offset += nBandH * nBandH;
  int16_t* LL = &buffer[offset];
  const size_t nDstStepX = nBandL + nBandH;
  const size_t nDstStepY = nBandL + nBandH;
  offset = 0;
  int16_t* L = &temp[offset];
  offset += nBandL * nDstStepX;
  int16_t* H = &temp[offset];
  int16_t* LLx = &buffer[0];

  ProgIdwtX(LL, nBandL, HL, nBandH, L, nDstStepX, nBandL, nBandH, nBandL);
  ProgIdwtX(LH, nBandL, HH, nBandH, H, nDstStepX, nBandL, nBandH, nBandH);
  ProgIdwtY(L, nDstStepX, H, nDstStepX, LLx, nDstStepY, nBandL, nBandH, nBandL + nBandH);
}

static void RfxDwtExtrapolateDecode(int16_t* buffer, int16_t* temp) {
  ProgDwtBlock(&buffer[3807], temp, 3);
  ProgDwtBlock(&buffer[3007], temp, 2);
  ProgDwtBlock(&buffer[0], temp, 1);
}

bool DecodeTileFirst(const RfxTileRef& tile, uint8_t* dst, int dstStride, int16_t* current[3]) {
  if (tile.type != RfxTileType::kFirst || dst == nullptr || dstStride < 64 * 4) {
    return false;
  }
  if (tile.quants == nullptr || tile.quantIdxY >= tile.numQuant ||
      tile.quantIdxCb >= tile.numQuant || tile.quantIdxCr >= tile.numQuant) {
    return false;
  }
  // TEMP debug switches (offline reference tuning; see PERF-TODO §1.G).
  static const int kShiftMode = getenv("HMRDP_RFX_SHIFT") ? atoi(getenv("HMRDP_RFX_SHIFT")) : 0;
  static const int kOffsetsMode = getenv("HMRDP_RFX_OFFSETS") ? atoi(getenv("HMRDP_RFX_OFFSETS")) : 0;
  static const int kDwtMode = getenv("HMRDP_RFX_DWT") ? atoi(getenv("HMRDP_RFX_DWT")) : 0;
  const bool regionExtra = (tile.regionFlags & 0x01u) != 0;
  const bool extrapolate = (kOffsetsMode == 0) ? regionExtra : (kOffsetsMode == 2);
  const bool extrapolateDwt = (kDwtMode == 0) ? regionExtra : (kDwtMode == 2);
  const bool diff = (tile.flags & 0x01u) != 0;

  // Per-tile progressive quant (0xFF -> the all-zero "full" table).
  RfxQuant progY;
  RfxQuant progCb;
  RfxQuant progCr;
  if (tile.quality != 0xFF && tile.progQuants != nullptr && tile.quality < tile.numProgQuant) {
    progY = tile.progQuants[tile.quality].y;
    progCb = tile.progQuants[tile.quality].cb;
    progCr = tile.progQuants[tile.quality].cr;
  }
  const RfxQuant* qv[3] = {&tile.quants[tile.quantIdxY], &tile.quants[tile.quantIdxCb],
                           &tile.quants[tile.quantIdxCr]};
  const RfxQuant* pv[3] = {&progY, &progCb, &progCr};
  const auto fieldShift = [](int q, int p) -> uint8_t {
    int v = 0;
    switch (kShiftMode) {
      case 1:
        v = q;
        break;
      case 2:
        v = q + p;
        break;
      case 3:
        v = 0;
        break;
      default:
        v = q + p - 1;
        break;
    }
    return static_cast<uint8_t>(v < 0 ? 0 : v);
  };
  RfxQuant shift[3];
  for (int c = 0; c < 3; ++c) {
    const RfxQuant& q = *qv[c];
    const RfxQuant& p = *pv[c];
    RfxQuant& s = shift[c];
    s.LL3 = fieldShift(q.LL3, p.LL3);
    s.HL3 = fieldShift(q.HL3, p.HL3);
    s.LH3 = fieldShift(q.LH3, p.LH3);
    s.HH3 = fieldShift(q.HH3, p.HH3);
    s.HL2 = fieldShift(q.HL2, p.HL2);
    s.LH2 = fieldShift(q.LH2, p.LH2);
    s.HH2 = fieldShift(q.HH2, p.HH2);
    s.HL1 = fieldShift(q.HL1, p.HL1);
    s.LH1 = fieldShift(q.LH1, p.LH1);
    s.HH1 = fieldShift(q.HH1, p.HH1);
  }

  const uint8_t* data[3] = {tile.yData, tile.cbData, tile.crData};
  const uint16_t len[3] = {tile.yLen, tile.cbLen, tile.crLen};

  int16_t comp[3][4096];
  int16_t temp[4096];
  for (int c = 0; c < 3; ++c) {
    if (data[c] == nullptr || len[c] == 0) {
      return false;
    }
    if (!RlgrDecode(RlgrMode::kRlgr1, data[c], len[c], comp[c], 4096)) {
      return false;
    }
    if (getenv("HMRDP_RFX_DEBUG") != nullptr && c == 0) {
      static int rdbg = 0;
      if (rdbg < 2) {
        int nz = 0, mn = 32767, mx = -32768;
        for (int i = 0; i < 4096; ++i) {
          if (comp[c][i] != 0) nz++;
          if (comp[c][i] < mn) mn = comp[c][i];
          if (comp[c][i] > mx) mx = comp[c][i];
        }
        printf("  Y rlgr: len=%u nz=%d range[%d..%d] tail:", tile.yLen, nz, mn, mx);
        for (int i = 4088; i < 4096; ++i) printf(" %d", comp[c][i]);
        printf("\n");
        rdbg++;
      }
    }
    if (extrapolate) {
      DequantSubband(comp[c], 0, 1023, shift[c].HL1);
      DequantSubband(comp[c], 1023, 1023, shift[c].LH1);
      DequantSubband(comp[c], 2046, 961, shift[c].HH1);
      DequantSubband(comp[c], 3007, 272, shift[c].HL2);
      DequantSubband(comp[c], 3279, 272, shift[c].LH2);
      DequantSubband(comp[c], 3551, 256, shift[c].HH2);
      DequantSubband(comp[c], 3807, 72, shift[c].HL3);
      DequantSubband(comp[c], 3879, 72, shift[c].LH3);
      DequantSubband(comp[c], 3951, 64, shift[c].HH3);
      DifferentialDecode(&comp[c][4015], 81);
      DequantSubband(comp[c], 4015, 81, shift[c].LL3);
    } else {
      DifferentialDecode(&comp[c][4032], 64);
      DequantSubband(comp[c], 0, 1024, shift[c].HL1);
      DequantSubband(comp[c], 1024, 1024, shift[c].LH1);
      DequantSubband(comp[c], 2048, 1024, shift[c].HH1);
      DequantSubband(comp[c], 3072, 256, shift[c].HL2);
      DequantSubband(comp[c], 3328, 256, shift[c].LH2);
      DequantSubband(comp[c], 3584, 256, shift[c].HH2);
      DequantSubband(comp[c], 3840, 64, shift[c].HL3);
      DequantSubband(comp[c], 3904, 64, shift[c].LH3);
      DequantSubband(comp[c], 3968, 64, shift[c].HH3);
      DequantSubband(comp[c], 4032, 64, shift[c].LL3);
    }
    if (current != nullptr && current[c] != nullptr) {
      if (diff) {
        for (int i = 0; i < 4096; ++i) {
          comp[c][i] = static_cast<int16_t>(comp[c][i] + current[c][i]);
        }
      } else {
        for (int i = 0; i < 4096; ++i) {
          current[c][i] = comp[c][i];
        }
      }
    }
    if (extrapolateDwt) {
      RfxDwtExtrapolateDecode(comp[c], temp);
    } else {
      InverseDwt2d(comp[c], temp);
    }
  }
  if (getenv("HMRDP_RFX_DEBUG") != nullptr) {
    static int dbg = 0;
    if (dbg < 3) {
      printf("tile(%u,%u) Ydc=%d Cbdc=%d Crdc=%d\n", tile.xIdx, tile.yIdx, comp[0][0],
             comp[1][0], comp[2][0]);
      dbg++;
    }
  }
  YCbCrToBgra(comp[0], comp[1], comp[2], dst, dstStride);
  return true;
}

bool ParseRfxProgressive(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                         RfxParseStats* stats) {
  if (data == nullptr || size < 6) {
    if (stats != nullptr) {
      stats->errors++;
    }
    return false;
  }
  if (stats != nullptr) {
    stats->messages++;
  }

  bool ok = true;
  size_t pos = 0;
  while (pos + 6 <= size) {
    const uint16_t blockType = ReadU16(data + pos);
    const uint32_t blockLen = ReadU32(data + pos + 2);
    if (blockLen < 6 || pos + blockLen > size) {
      ok = false;
      break;
    }
    const uint8_t* body = data + pos + 6;
    const size_t bodyLen = static_cast<size_t>(blockLen) - 6;
    switch (blockType) {
      case kWbtRegion:
        if (!ParseRegion(body, bodyLen, onTile, stats)) {
          ok = false;
        }
        break;
      case kWbtSync:
      case kWbtFrameBegin:
      case kWbtFrameEnd:
      case kWbtContext:
        break;
      default:
        ok = false;
        break;
    }
    if (!ok) {
      break;
    }
    pos += blockLen;
  }
  if (!ok && stats != nullptr) {
    stats->errors++;
  }
  return ok;
}

}  // namespace hmrdp
