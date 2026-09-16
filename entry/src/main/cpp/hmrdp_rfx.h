/*
 * HmRdp - RemoteFX/Progressive container parser + shared GFX command model.
 *
 * §1 - Container parser: walks the compressed "RFX Progressive" bitmap stream
 *      (blocks, region, tiles, quantization tables) and exposes the per-tile
 *      payloads. Portable C++ with no OHOS/FreeRDP dependency.
 * §2 - Shared GFX surface/command model (GpuSurface / GpuCmd / GpuCodec and the
 *      packed pixel formats) used by the Vulkan engine (hmrdp_vk_desktop.*) and
 *      by the replay harness, plus the ClearCodec hook that hands a payload to
 *      FreeRDP's own clear_decompress.
 *
 * The Vulkan engine is the only backend: whether a device can run it is answered
 * by GetVulkanCapabilities() (hmrdp_vk_context.*).
 */
#ifndef HMRDP_RFX_H
#define HMRDP_RFX_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hmrdp {

// ===========================================================================
// §1 Container parser
// ===========================================================================

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

// One clipping rectangle of a region (device pixels). FreeRDP composites a
// decoded tile only inside the union of these rects; the rest of the tile keeps
// the previous surface content.
struct RfxRect {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
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
  // Region clipping rects (valid for the duration of the callback).
  const RfxRect* rects = nullptr;
  uint16_t numRects = 0;

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
  // Dev (doc_agent/gfx-engine.md §6): where the last malformed message failed.
  // FreeRDP's rejection is *not* all-or-nothing - the region header is validated
  // before anything is applied, but a failure during the tile walk happens after
  // the tiles read so far were already registered in the frame's tile list - so
  // the stage decides whether "both reject" really means "both do nothing".
  const char* errorStage = nullptr;
};

using RfxTileCallback = std::function<void(const RfxTileRef&)>;

// One REGION block's header data. FreeRDP's update_tiles composites its tile list
// with the *region's* clipping rects, and a Progressive message can carry a region
// with zero tiles (a pure re-composite pass) - so the rects must be visible even
// when no tile callback fires.
struct RfxRegionRef {
  const RfxRect* rects = nullptr;
  uint16_t numRects = 0;
  uint16_t numTiles = 0;
  uint8_t flags = 0;
};
using RfxRegionCallback = std::function<void(const RfxRegionRef&)>;

// FreeRDP's WBT block state machine: a REGION is silently ignored (not an error!)
// when it arrives before FRAME_BEGIN or after FRAME_END *of the same message*
// (progressive_decompress resets it per message), and the ignored region's tile
// state update goes with it. Pass the owning engine's state so the decoders match
// gdi; nullptr disables the guard.
struct RfxProgressiveState {
  bool frameBegin = false;
  bool frameEnd = false;
  uint32_t skippedRegions = 0;
};

// Parses one Progressive message. Invokes `onTile` for every tile in order.
// Returns false if the container is malformed (stats->errors is bumped).
bool ParseRfxProgressive(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                         RfxParseStats* stats, RfxProgressiveState* state = nullptr,
                         const RfxRegionCallback& onRegion = RfxRegionCallback());

// ===========================================================================
// ClearCodec hook (FreeRDP clear_decompress; implemented in hmrdp_rfx.cpp)
// ===========================================================================

// ClearCodec decode hook, wired to FreeRDP's clear_decompress.
class GfxClearDecoder {
 public:
  virtual ~GfxClearDecoder() = default;
  // Decodes one ClearCodec payload into `dst` (`dstFormat` packed pixel format,
  // `stride` bytes/row) at (xDst,yDst), clipped to dstW/dstH. Returns false on
  // failure.
  virtual bool Decode(const uint8_t* src, size_t size, int width, int height, uint32_t dstFormat,
                      uint8_t* dst, int stride, int xDst, int yDst, int dstW, int dstH) = 0;
  // ResetGraphics parity: FreeRDP's gdi_ResetGraphics calls
  // freerdp_client_codecs_reset(), which for ClearCodec runs
  // clear_context_reset() - the band sequence number restarts (the glyph / VBar
  // caches deliberately survive).
  virtual void Reset() = 0;
};

// Creates a ClearCodec decoder backed by FreeRDP's clear_decompress.
std::unique_ptr<GfxClearDecoder> CreateFreeRdpClearDecoder();

// ===========================================================================
// §2 Shared GFX surface/command model
// ===========================================================================

// FreeRDP packed surface pixel formats (values copied from freerdp/codec/color.h
// so this header stays FreeRDP-free). The wire format maps 0x20 -> BGRX32 and
// 0x21 -> BGRA32, and that is also the engine's storage order everywhere: the
// presenter converts at present time, so there is no RGBA-storage variant
// (doc_agent/gfx-engine.md §2.3).
constexpr uint32_t kPixelFormatBgra32 = 0x20048888u;
constexpr uint32_t kPixelFormatBgrx32 = 0x20040888u;

// Public metadata of one GFX surface (the backend buffers stay private to the
// engine). Dimensions/stride are aligned to 16 exactly like FreeRDP's
// gdiGfxSurface; `format` is the packed FreeRDP format above.
struct GpuSurface {
  uint16_t id = 0;
  int width = 0;   // aligned to 16
  int height = 0;  // aligned to 16
  int stride = 0;  // aligned width*4, bytes
  uint32_t format = 0;
  int gridW = 0;
  int gridH = 0;
  bool mapped = false;
  uint32_t outputX = 0;
  uint32_t outputY = 0;
  int mappedWidth = 0;   // raw CreateSurface width
  int mappedHeight = 0;  // raw CreateSurface height
  // Union of everything marked since the last Compose. The rect *list* lives in
  // the engine's Surface (Impl): commands mark the individual rects they touched
  // (a Progressive message marks one rect per decoded tile, a cache restore or an
  // uncompressed upload its own rect), the engine merges them into exact
  // rectangles and Compose copies exactly those - never their merged bounding box.
  // That is the same policy the CPU (gdi) present path uses for its upload, so a
  // scene's cost profile is comparable across the two routes
  // (doc_agent/gfx-engine.md §2.3). This box is the bounded fallback used when
  // even the merged list is too long.
  bool dirtyValid = false;
  int dirtyLeft = 0;
  int dirtyTop = 0;
  int dirtyRight = 0;
  int dirtyBottom = 0;
};

// --- Shared GFX command model (Vulkan engine + replay harness) --------------

// Command ids as delivered to an engine (mirrors the capture stream). A capture
// carries raw ids; this enum documents the ones the engines implement.
enum GpuCmd : uint16_t {
  kGpuCmdWireToSurface = 0x0001,
  kGpuCmdSolidFill = 0x0004,
  kGpuCmdSurfaceToSurface = 0x0005,
  kGpuCmdSurfaceToCache = 0x0006,
  kGpuCmdCacheToSurface = 0x0007,
  kGpuCmdEvictCacheEntry = 0x0008,
  kGpuCmdCreateSurface = 0x0009,
  kGpuCmdDeleteSurface = 0x000A,
  kGpuCmdStartFrame = 0x000B,
  kGpuCmdEndFrame = 0x000C,
  kGpuCmdResetGraphics = 0x000E,
  kGpuCmdMapSurfaceToOutput = 0x000F,
  kGpuCmdMapSurfaceToScaledOutput = 0x0017,
};

// GFX surface command codec ids (RDPGFX_CODECID_*), carried in the surface
// command's first scalar. Unknown ids are not drawn by the engine.
enum GpuCodec : uint32_t {
  kGpuCodecUncompressed = 0x0000,
  kGpuCodecClearCodec = 0x0008,
  kGpuCodecCaprogressive = 0x0009,
  kGpuCodecCaprogressiveV2 = 0x000D,
};

}  // namespace hmrdp

#endif  // HMRDP_RFX_H
