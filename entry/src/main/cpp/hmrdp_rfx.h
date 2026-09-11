/*
 * HmRdp - RemoteFX / Progressive (MS-RDPRFX) container parser.
 *
 * Parses the compressed "RFX Progressive" bitmap stream that the server sends
 * in CAPROGRESSIVE GFX surface commands. It only walks the container (blocks,
 * region, tiles) and exposes the per-tile RLGR payloads; the pixel decoding is
 * done elsewhere (CPU reference or the GPU path, see PERF-TODO §2).
 *
 * Portable C++17 with no OHOS dependencies so it can also be compiled on the
 * host for offline validation against a captured stream.
 */
#ifndef HMRDP_RFX_H
#define HMRDP_RFX_H

#include <cstddef>
#include <cstdint>
#include <functional>

namespace hmrdp {

enum class RfxTileType {
  kFirst = 0,
  kSimple = 1,
  kUpgrade = 2,
};

// Per-component quantization shifts (10 sub-bands, 4 bits each), read from the
// region's component quant table.
struct RfxQuant {
  uint8_t LL3 = 0;
  uint8_t HL3 = 0;
  uint8_t LH3 = 0;
  uint8_t HH3 = 0;
  uint8_t HL2 = 0;
  uint8_t LH2 = 0;
  uint8_t HH2 = 0;
  uint8_t HL1 = 0;
  uint8_t LH1 = 0;
  uint8_t HH1 = 0;
};

// One entry of the region's progressive quant table (16 bytes): a quality
// index plus the per-component adjustment added to the component quant table.
struct RfxProgQuant {
  uint8_t quality = 0;
  RfxQuant y;
  RfxQuant cb;
  RfxQuant cr;
};

// One 64x64 tile update. For kFirst/kSimple the three component payloads are
// RLGR-encoded whole coefficients; for kUpgrade they are SRL/raw refinement
// bands (progressive passes).
struct RfxTileRef {
  RfxTileType type = RfxTileType::kFirst;
  uint8_t quantIdxY = 0;
  uint8_t quantIdxCb = 0;
  uint8_t quantIdxCr = 0;
  uint16_t xIdx = 0;
  uint16_t yIdx = 0;
  uint8_t flags = 0;
  uint8_t quality = 0;
  // Region-level state, valid for the duration of the callback.
  const RfxQuant* quants = nullptr;
  uint8_t numQuant = 0;
  const RfxProgQuant* progQuants = nullptr;
  uint8_t numProgQuant = 0;
  uint8_t regionFlags = 0;

  // kFirst / kSimple component payloads.
  const uint8_t* yData = nullptr;
  uint16_t yLen = 0;
  const uint8_t* cbData = nullptr;
  uint16_t cbLen = 0;
  const uint8_t* crData = nullptr;
  uint16_t crLen = 0;
  const uint8_t* tailData = nullptr;
  uint16_t tailLen = 0;

  // kUpgrade SRL (low-frequency) + raw (high-frequency) payloads.
  const uint8_t* ySrlData = nullptr;
  uint16_t ySrlLen = 0;
  const uint8_t* yRawData = nullptr;
  uint16_t yRawLen = 0;
  const uint8_t* cbSrlData = nullptr;
  uint16_t cbSrlLen = 0;
  const uint8_t* cbRawData = nullptr;
  uint16_t cbRawLen = 0;
  const uint8_t* crSrlData = nullptr;
  uint16_t crSrlLen = 0;
  const uint8_t* crRawData = nullptr;
  uint16_t crRawLen = 0;
};

struct RfxParseStats {
  uint32_t messages = 0;   // ParseRfxProgressive calls
  uint32_t regions = 0;    // WBT_REGION blocks
  uint32_t rects = 0;      // clipping rects summed over regions
  uint32_t tiles = 0;      // tile blocks summed over regions
  uint32_t firstTiles = 0;
  uint32_t simpleTiles = 0;
  uint32_t upgradeTiles = 0;
  uint16_t maxTileX = 0;   // largest tile x index seen (grid - 1)
  uint16_t maxTileY = 0;
  uint32_t extrapolateRegions = 0;  // region flags & RFX_DWT_REDUCE_EXTRAPOLATE
  uint32_t diffTiles = 0;           // tile flags & RFX_TILE_DIFFERENCE
  uint32_t errors = 0;     // malformed blocks
};

using RfxTileCallback = std::function<void(const RfxTileRef&)>;

// Parses one Progressive message. Invokes `onTile` for every tile in order.
// Returns false if the container is malformed (stats->errors is bumped).
bool ParseRfxProgressive(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                         RfxParseStats* stats);

enum class RlgrMode {
  kRlgr1 = 0,
  kRlgr3 = 1,
};

// Decodes one RLGR (Run-Length Golomb-Rice) encoded component stream into
// `dst` (dstCount int16 coefficients, zero-padded at the end). This is the
// per-component entropy decode of a RemoteFX tile; a straight port of
// FreeRDP's rfx_rlgr_decode so the same math can later run on the GPU.
bool RlgrDecode(RlgrMode mode, const uint8_t* src, size_t size, int16_t* dst, size_t dstCount,
                size_t* remainingBits = nullptr);

// Decodes one kFirst (whole-tile) update into a 64x64 BGRA image (`dstStride`
// in bytes). This is the CPU reference for the RemoteFX tile pipeline
// (RLGR -> differential -> dequant -> inverse DWT -> YCbCr->RGB) that the GPU
// path will mirror. `current` is the per-tile pre-DWT coefficient state used by
// RFX_TILE_DIFFERENCE tiles: pass nullptr for non-differential tiles only.
bool DecodeTileFirst(const RfxTileRef& tile, uint8_t* dst, int dstStride,
                     int16_t* current[3] = nullptr);

}  // namespace hmrdp

#endif  // HMRDP_RFX_H
