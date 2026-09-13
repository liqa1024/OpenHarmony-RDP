/*
 * HmRdp - GPU RemoteFX/Progressive decoder + GFX surface engine.
 *
 * §1 - Container parser: walks the compressed "RFX Progressive" bitmap stream
 *      (blocks, region, tiles, quantization tables) and exposes the per-tile
 *      payloads. Portable C++ with no OHOS/FreeRDP dependency.
 * §2 - GPU decode + surface engine: the integer RemoteFX/Progressive pipeline
 *      runs in GLES 3.1 compute shaders, and GfxGpuDesktop holds the full GFX
 *      surface model (PERF-TODO §2.3-§2.5) - a `surfaceId -> GPU surface`
 *      registry with per-surface progressive state and the four pixel
 *      operations (decode write / solid fill / surface copy / cache),
 *      reproducing the FreeRDP command semantics.
 *
 * ClearCodec is the one codec not implemented in GLES: it is decoded on the CPU
 * through FreeRDP's clear_decompress (read-modify-write of the target surface).
 *
 * Devices without GLES 3.1 compute cannot use the engine (see
 * GetGpuComputeInfo()); the session then keeps FreeRDP's own gdi path.
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
};

using RfxTileCallback = std::function<void(const RfxTileRef&)>;

// Parses one Progressive message. Invokes `onTile` for every tile in order.
// Returns false if the container is malformed (stats->errors is bumped).
bool ParseRfxProgressive(const uint8_t* data, size_t size, const RfxTileCallback& onTile,
                         RfxParseStats* stats);

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
};

// Creates a ClearCodec decoder backed by FreeRDP's clear_decompress.
std::unique_ptr<GfxClearDecoder> CreateFreeRdpClearDecoder();

// ===========================================================================
// §2 GPU decode + surface engine
// ===========================================================================

// Runtime GLES compute capability. RFX decoding only uses the GPU path when
// `compute` is true.
struct GpuComputeInfo {
  bool egl = false;      // an offscreen EGL context could be created
  bool compute = false;  // GLES >= 3.1 (compute shaders) available
  int glMajor = 0;
  int glMinor = 0;
  int maxWorkGroupInvocations = 0;
  int maxSharedMemory = 0;
  int maxSsboSize = 0;
  int maxTextureSize = 0;
  char renderer[128] = {0};
  char version[64] = {0};

  std::string Describe() const;
};

// Probes the device once (result cached). Safe to call from any thread.
const GpuComputeInfo& GetGpuComputeInfo();

// FreeRDP packed surface pixel formats (values copied from freerdp/codec/color.h
// so this header stays FreeRDP-free). The wire format maps 0x20 -> BGRX32 and
// 0x21 -> BGRA32.
constexpr uint32_t kPixelFormatBgra32 = 0x20048888u;
constexpr uint32_t kPixelFormatBgrx32 = 0x20040888u;

// Public metadata of one GPU-resident GFX surface (the GL buffers stay private
// to the engine). Dimensions/stride are aligned to 16 exactly like FreeRDP's
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
  // Bounding box of the region touched since the last Compose (a conservative
  // superset of FreeRDP's per-command invalid rects).
  bool dirtyValid = false;
  int dirtyLeft = 0;
  int dirtyTop = 0;
  int dirtyRight = 0;
  int dirtyBottom = 0;
};

// GPU desktop / surface engine: it owns an offscreen GLES 3.1 context, one GPU
// surface buffer (+ progressive tile state) per `surfaceId` and a global bitmap
// cache, and reproduces the FreeRDP command semantics (PERF-TODO §2.3-§2.5).
//
// ClearCodec is decoded on the CPU through the injected GfxClearDecoder hook
// (FreeRDP's clear_decompress) with a read-modify-write of the target surface;
// every other command runs on the GPU (compute).
class GfxGpuDesktop {
 public:
  explicit GfxGpuDesktop(GfxClearDecoder* clearDecoder = nullptr);
  ~GfxGpuDesktop();

  GfxGpuDesktop(const GfxGpuDesktop&) = delete;
  GfxGpuDesktop& operator=(const GfxGpuDesktop&) = delete;

  // Creates the offscreen context, programs and shared scratch buffers. No
  // surface exists until CreateSurface is called.
  bool Init();
  void Reset();
  bool ready() const { return ready_; }

  // --- Surface lifecycle ---------------------------------------------------
  // Aligns width/height to 16, stride to 16 bytes, fills with 0xFF and resets
  // the per-surface progressive state. `format` is the wire pixel format
  // (0x20 -> BGRX32, 0x21 -> BGRA32).
  bool CreateSurface(uint16_t surfaceId, int width, int height, uint32_t format);
  void DeleteSurface(uint16_t surfaceId);
  const GpuSurface* FindSurface(uint16_t surfaceId) const;

  // Output mapping metadata (PERF-TODO §2.5); consumed by Compose. 1:1 only:
  // server-side scaled mappings are unsupported (see ApplyCommand).
  void MapSurfaceToOutput(uint16_t surfaceId, uint32_t outputOriginX, uint32_t outputOriginY);

  // --- Screen (front buffer) ------------------------------------------------
  // Resets the screen buffer (FreeRDP ResetGraphics); zero releases it.
  bool ResetGraphics(int width, int height);
  // Composites every output-mapped surface's dirty region into the screen
  // (FreeRDP's gdi_OutputUpdate) and clears their dirty regions.
  // Returns true when the screen dirty region is non-empty.
  bool Compose();
  void ClearScreenDirty();
  bool screenDirty() const;
  int screenWidth() const { return screenW_; }
  int screenHeight() const { return screenH_; }
  // Screen texture (BGRA bytes as RGBA8) in the process-wide EGL share group,
  // so the Renderer can sample it directly; 0 when no screen is allocated.
  // Returned as uint32_t to keep GLES out of this header.
  uint32_t screenTexture() const;
  // Full screen (top-down, `screenW*4` stride) as BGRA. Dev/verification only.
  bool ReadScreen(std::vector<uint8_t>* out);

  // --- Pixel commands (FreeRDP GFX command semantics) ----------------------
  // Applies one captured/received GFX command. `params`/`payload` may be null
  // when their length is 0. Unknown command ids and unknown target surfaces are
  // ignored.
  void ApplyCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                    const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                    uint32_t payloadLen);

  // Decodes one Progressive message payload ("WBT" container), updating the
  // surface's persistent tile state. Returns false on GL failure.
  bool DecodeMessage(uint16_t surfaceId, const uint8_t* payload, size_t size);

  bool SolidFill(uint16_t surfaceId, uint32_t bgraPixel, const uint16_t* rects,
                 uint32_t rectCount);
  // Uploads a BGRA rect into the surface (uncompressed 24/32bpp).
  bool UploadBgra(uint16_t surfaceId, int left, int top, int width, int height,
                  const uint8_t* bgra, int srcStride);
  // Full-width row transfer, used to emulate ClearCodec (which must read the
  // existing surface content for the pixels its bands do not overwrite).
  bool DownloadRows(uint16_t surfaceId, int top, int height, uint8_t* dst, int dstStride);
  bool UploadRows(uint16_t surfaceId, int top, int height, const uint8_t* src, int srcStride);
  bool SurfaceToCache(uint16_t surfaceId, uint16_t slot, int x, int y, int width, int height);
  bool CacheToSurface(uint16_t surfaceId, uint16_t slot, int dstX, int dstY);
  void EvictCache(uint16_t slot);
  bool SurfaceToSurface(uint16_t srcSurfaceId, int srcX, int srcY, int width, int height,
                        uint16_t dstSurfaceId, int dstX, int dstY);

  // Full surface (top-down, `stride` bytes) as BGRA. Dev/verification only: it
  // maps the GPU buffer back to the CPU.
  bool ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool ready_ = false;
  int screenW_ = 0;
  int screenH_ = 0;

  // ClearCodec is decoded on the CPU and is not self-contained, so each command
  // needs a GPU->CPU->GPU read-modify-write of its band. The stream sends runs of
  // consecutive ClearCodec commands for the same surface (measured ~17 on
  // average, up to ~200), so they are queued and served by a single shared
  // mapping instead of one map/unmap stall per command. Order is preserved: the
  // queue is flushed, in arrival order, before any other command (and before
  // every compose), and a run never spans two surfaces.
  void QueueClear(uint16_t surfaceId, const uint8_t* payload, size_t payloadLen, int left,
                  int top, int width, int height);
  void FlushPendingClears();
};

// --- Shared command/present pipeline (session + replay harness) -------------

// Command ids as delivered to the engine (mirrors the capture stream). A
// capture carries raw ids; this enum documents the ones the engine implements.
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

class Renderer;

// Composes the engine screen and presents it, then clears the screen dirty gate.
// The engine screen texture lives in the process-wide EGL share group, so the
// Renderer samples it directly (no CPU readback/upload). Returns true when a
// frame reached the screen.
bool GpuPresentComposed(GfxGpuDesktop* engine, Renderer* renderer);

}  // namespace hmrdp

#endif  // HMRDP_RFX_H
