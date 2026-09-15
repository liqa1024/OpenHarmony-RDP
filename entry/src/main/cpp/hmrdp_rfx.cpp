/*
 * HmRdp - RemoteFX/Progressive container parser + ClearCodec glue
 * (see hmrdp_rfx.h).
 *
 * The Progressive container (blocks / region / tiles / quantization tables) is
 * parsed here and handed to the engine as per-tile payloads; ClearCodec payloads
 * are decoded through FreeRDP's own clear_decompress.
 */
#include "hmrdp_rfx.h"

#include <cstdint>
#include <memory>

#include <freerdp/codec/clear.h>

#include "hmrdp_log.h"

namespace hmrdp {

// ---------------------------------------------------------------------------
// Progressive container parser
// ---------------------------------------------------------------------------

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
                 const RfxRegionCallback& onRegion, RfxParseStats* stats) {
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
  // FreeRDP's progressive_wb_read_region_header rejects the whole region (and
  // therefore the whole message: nothing is decoded, not even the tile state)
  // when numRects < 1 (-1013) or > 1024. The engine used to accept it, which made
  // its composite (empty clipping rects used to mean "whole tile") diverge from
  // gdi for every tile of that message.
  constexpr uint16_t kMaxRects = 1024;
  if (numRects < 1 || numRects > kMaxRects) {
    return false;
  }
  size_t p = 12;
  // rects (8 bytes each): x,y,width,height all little-endian u16.
  RfxRect rects[kMaxRects];
  if (p + static_cast<size_t>(numRects) * 8 > size) {
    return false;
  }
  for (uint16_t i = 0; i < numRects; ++i) {
    rects[i].x = ReadU16(data + p);
    rects[i].y = ReadU16(data + p + 2);
    rects[i].width = ReadU16(data + p + 4);
    rects[i].height = ReadU16(data + p + 6);
    p += 8;
  }
  // Component quant tables: 5 bytes -> 10 x 4-bit shifts each.
  RfxQuant quants[7];
  if (p + static_cast<size_t>(numQuant) * 5 > size) {
    return false;
  }
  for (uint8_t q = 0; q < numQuant; ++q) {
    ReadQuantNibbles(data + p, &quants[q]);
    // FreeRDP validates the region's component quant table and rejects the whole
    // region (nothing is decoded at all) when any nibble is outside [6,15]
    // (progressive.c `progressive_wb_region`: quant < 6 or > 15 -> return -1).
    // Decoding such a region anyway makes the engine diverge from gdi for every
    // tile of that message.
    const uint8_t nibbles[10] = {quants[q].LL3, quants[q].HL3, quants[q].LH3, quants[q].HH3,
                                 quants[q].HL2, quants[q].LH2, quants[q].HH2, quants[q].HL1,
                                 quants[q].LH1, quants[q].HH1};
    for (size_t i = 0; i < 10; ++i) {
      if (nibbles[i] < 6 || nibbles[i] > 15) {
        return false;
      }
    }
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

  // The region's clipping rects are what update_tiles composites every tile of
  // the frame's list with, and a region can carry zero tiles (a pure re-composite
  // pass), so hand them out before the tile walk.
  if (onRegion) {
    RfxRegionRef ref;
    ref.rects = rects;
    ref.numRects = numRects;
    ref.numTiles = numTiles;
    ref.flags = regionFlags;
    onRegion(ref);
  }

  // FreeRDP walks the tile blocks until `tileDataSize` bytes are consumed (not
  // until `numTiles` blocks) and then requires the count to match: a stream with
  // *more* tile blocks than the header claims fails the whole region
  // (progressive.c: usedTiles >= numTiles -> FALSE, count != numTiles -> -1044).
  uint32_t count = 0;
  while (p + 6 <= tileEnd) {
    if (count >= numTiles) {
      return false;
    }
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
    tile.rects = rects;
    tile.numRects = numRects;
    if (onTile) {
      onTile(tile);
    }
    p += blockLen;
    count++;
  }
  // The byte budget must be consumed exactly and every declared tile must be
  // present (FreeRDP: (end - start) != tileDataSize -> -1041).
  return count == numTiles && p == tileEnd;
}

}  // namespace

bool ParseRfxProgressive(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                         RfxParseStats* stats, RfxProgressiveState* state,
                         const RfxRegionCallback& onRegion) {
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
        // FreeRDP ignores (skips, no error) a REGION that arrives before
        // FRAME_BEGIN or after FRAME_END - the whole region, including its tile
        // state update. Decoding it anyway diverges from gdi.
        if (state != nullptr && (!state->frameBegin || state->frameEnd)) {
          state->skippedRegions++;
          break;
        }
        if (!ParseRegion(body, bodyLen, onTile, onRegion, stats)) {
          ok = false;
        }
        break;
      case kWbtFrameBegin:
        if (state != nullptr) {
          state->frameBegin = true;
          state->frameEnd = false;
        }
        break;
      case kWbtFrameEnd:
        if (state != nullptr) {
          state->frameEnd = true;
        }
        break;
      case kWbtSync:
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

// ---------------------------------------------------------------------------
// ClearCodec glue: FreeRDP clear_decompress (the one codec kept on the CPU)
// ---------------------------------------------------------------------------

namespace {

class FreeRdpClearDecoder : public GfxClearDecoder {
 public:
  FreeRdpClearDecoder() : clear_(clear_context_new(0 /* compressor = FALSE */)) {}
  ~FreeRdpClearDecoder() override {
    if (clear_ != nullptr) {
      clear_context_free(clear_);
      clear_ = nullptr;
    }
  }

  FreeRdpClearDecoder(const FreeRdpClearDecoder&) = delete;
  FreeRdpClearDecoder& operator=(const FreeRdpClearDecoder&) = delete;

  bool Decode(const uint8_t* src, size_t size, int width, int height, uint32_t dstFormat,
              uint8_t* dst, int stride, int xDst, int yDst, int dstW, int dstH) override {
    if (clear_ == nullptr || src == nullptr || dst == nullptr) {
      return false;
    }
    const INT32 rc =
        clear_decompress(clear_, src, static_cast<UINT32>(size), static_cast<UINT32>(width),
                         static_cast<UINT32>(height), dst, dstFormat, static_cast<UINT32>(stride),
                         static_cast<UINT32>(xDst), static_cast<UINT32>(yDst),
                         static_cast<UINT32>(dstW), static_cast<UINT32>(dstH), nullptr);
    return rc >= 0;
  }

  void Reset() override {
    if (clear_ != nullptr) {
      clear_context_reset(clear_);
    }
  }

 private:
  CLEAR_CONTEXT* clear_ = nullptr;
};

}  // namespace

std::unique_ptr<GfxClearDecoder> CreateFreeRdpClearDecoder() {
  return std::unique_ptr<GfxClearDecoder>(new FreeRdpClearDecoder());
}

}  // namespace hmrdp