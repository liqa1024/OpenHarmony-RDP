/*
 * HmRdp - CPU reference GFX desktop / surface model (PERF-TODO §2.5 "B1").
 *
 * The GPU RemoteFX path only owns the 64x64 progressive tiles. A real RDPGFX
 * session also carries surface management, cached bitmap blits, solid fills and
 * (on the tested server) ClearCodec payloads, so the desktop can only be owned
 * once all of those are reproduced with the exact FreeRDP semantics.
 *
 * This module is the CPU-runnable mirror of the GPU desktop model (PERF-TODO §2):
 * the same flat 32-bit-word surface/cache buffers and the same per-pixel
 * fill/copy/compose operations as the compute kernels in hmrdp_rfx_gpu.cpp, just
 * executed by CPU loops. It replays the captured command stream
 * (hmrdp_gfx.bin, see hmrdp_gfx_dump.h) and can be aligned pixel-by-pixel
 * against the captured FreeRDP surface baselines (hmrdp_gfx_surface.bin), so the
 * algorithm can be verified (ideally on the host) before any GPU adaptation.
 *
 * The only non-symmetric piece is ClearCodec: per PERF-TODO §2.8 it is not
 * GPU-ized and is decoded by the injected GfxClearDecoder hook (FreeRDP's
 * clear_decompress on device); the model otherwise has no OHOS dependency.
 *
 * The CPU reference for the RemoteFX tiles themselves is hmrdp_rfx.cpp.
 *
 * Alignment highlights (mirroring libfreerdp/gdi/gfx.c):
 *  - CreateSurface aligns width/height/scanline to 16 and fills with 0xFF.
 *  - Surface ids are UINT16; the GFX pixel format maps 0x20 -> BGRX32,
 *    0x21 -> BGRA32.
 *  - CAPROGRESSIVE commands carry no destination rect (left/top/width/height
 *    are zero): tiles are placed at absolute surface coordinates xIdx*64,
 *    yIdx*64 and only composited inside the region's clip rects.
 *  - ClearCodec is decoded by the injected hook (FreeRDP clear_decompress on the
 *    device); the model never reimplements it (see PERF-TODO §2.8).
 */
#ifndef HMRDP_GFX_DESKTOP_H
#define HMRDP_GFX_DESKTOP_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hmrdp_rfx.h"

namespace hmrdp {

// FreeRDP packed pixel formats (values copied from freerdp/codec/color.h so this
// header stays FreeRDP-free). The captured stream only uses BGRX32.
constexpr uint32_t kPixelFormatBgra32 = 0x20048888u;
constexpr uint32_t kPixelFormatBgrx32 = 0x20040888u;
constexpr uint32_t kPixelFormatBgr24 = 0x18020888u;

// ClearCodec decode hook. On the device this is wired to FreeRDP's
// clear_decompress (hmrdp_gfx_clear.cpp); keeping it an interface lets the
// portable model be compiled and unit-replayed on the host.
class GfxClearDecoder {
 public:
  virtual ~GfxClearDecoder() = default;
  // Decodes one ClearCodec payload into `dst` (`dstFormat` packed pixel format,
  // `stride` bytes/row) at (xDst,yDst), clipped to dstW/dstH. Returns false on
  // failure.
  virtual bool Decode(const uint8_t* src, size_t size, int width, int height, uint32_t dstFormat,
                      uint8_t* dst, int stride, int xDst, int yDst, int dstW, int dstH) = 0;
};

struct GfxDesktopStats {
  uint32_t records = 0;
  uint32_t errors = 0;
  uint32_t created = 0;
  uint32_t deleted = 0;
  uint32_t clearCodec = 0;
  uint32_t progressive = 0;
  uint32_t progressiveTiles = 0;
  uint32_t progressiveSimple = 0;
  uint32_t uncompressed = 0;
  uint32_t solidFill = 0;
  uint32_t surfaceToSurface = 0;
  uint32_t surfaceToCache = 0;
  uint32_t cacheToSurface = 0;
  uint32_t evicted = 0;
  uint32_t unsupportedCodecs = 0;
};

// One GFX surface. Dimensions/scanline are aligned to 16 exactly like FreeRDP's
// gdiGfxSurface, and the initial clear is 0xFF.
struct GfxSurface {
  uint32_t id = 0;
  int width = 0;        // aligned to 16
  int height = 0;       // aligned to 16
  int stride = 0;       // aligned width*4, bytes
  uint32_t format = kPixelFormatBgrx32;
  std::vector<uint8_t> data;
  int gridW = 0;
  int gridH = 0;
  // Lazily created per-tile progressive state (RfxTileState is ~48 KB).
  std::vector<std::unique_ptr<RfxTileState>> tiles;
  bool mapped = false;
  uint32_t outputX = 0;
  uint32_t outputY = 0;
};

class GfxDesktop {
 public:
  explicit GfxDesktop(GfxClearDecoder* clearDecoder);
  ~GfxDesktop();

  // Applies one captured GFX command. `params`/`payload` may be null when their
  // length is 0. Unknown command ids are ignored (counted in stats().records).
  void ApplyCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                    const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                    uint32_t payloadLen);

  const GfxSurface* FindSurface(uint32_t id) const;
  const std::map<uint32_t, GfxSurface>& surfaces() const { return surfaces_; }
  const GfxDesktopStats& stats() const { return stats_; }
  void Reset();

 private:
  GfxSurface* EnsureSurface(uint32_t id);
  RfxTileState* TileState(GfxSurface* surface, int xIdx, int yIdx);
  void ApplyWireToSurface(uint32_t surfaceId, const uint8_t* params, uint32_t codecId,
                          const uint8_t* payload, uint32_t payloadLen);
  void ApplyProgressive(GfxSurface* surface, const uint8_t* payload, uint32_t payloadLen);
  void ApplyUncompressed(GfxSurface* surface, uint32_t srcFormat, int left, int top, int width,
                         int height, const uint8_t* payload, uint32_t payloadLen);
  void Blit(const GfxSurface& src, int srcX, int srcY, GfxSurface& dst, int dstX, int dstY,
            int width, int height);
  void FillRects(GfxSurface& surface, uint32_t pixel, const uint8_t* params, uint32_t rectCount);

  GfxClearDecoder* clear_ = nullptr;
  std::map<uint32_t, GfxSurface> surfaces_;
  struct CacheEntry {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> data;
  };
  std::map<uint32_t, CacheEntry> cache_;
  // FreeRDP's SurfaceToCache internally calls EvictCacheEntry on the same slot;
  // old captures recorded that internal call as a standalone command, so the
  // replay drops an EvictCacheEntry that immediately follows a SurfaceToCache
  // for the same slot.
  int lastCacheStoreSlot_ = -1;
  GfxDesktopStats stats_;
};

// Replays an hmrdp_gfx.bin capture through `desktop` and compares every surface
// against the GFS1 baselines in `surfacePath` at each EndFrame. The comparison
// is byte-exact over the whole baseline surface.
struct GfxDesktopSelfTestResult {
  bool ran = false;
  bool ok = false;
  uint32_t records = 0;
  uint32_t comparedRecords = 0;
  uint32_t badRecords = 0;
  uint64_t surfacesCompared = 0;
  uint64_t compared = 0;
  uint64_t mismatch = 0;
  // Hash-only checks use the per-frame 'GFH1' records (every frame, tiny) so a
  // long capture is validated end to end; byte checks use the 'GFS1' baselines.
  uint64_t surfacesHashed = 0;
  uint64_t hashMismatch = 0;
  double meanAbs = 0.0;
  std::string log;
};
GfxDesktopSelfTestResult ReplayGfxCapture(GfxDesktop* desktop, const std::string& gfxPath,
                                          const std::string& surfacePath);

// Device-side convenience entry point: replays `gfxPath` with a desktop whose
// ClearCodec hook is FreeRDP's clear_decompress. Implemented in
// hmrdp_gfx_clear.cpp (requires libfreerdp3); declared here so callers only
// depend on the portable result type.
GfxDesktopSelfTestResult RunGfxDesktopSelfTest(const std::string& gfxPath,
                                               const std::string& surfacePath);

// Creates a ClearCodec decoder backed by FreeRDP's clear_decompress.
// Implemented in hmrdp_gfx_clear.cpp (requires libfreerdp3).
std::unique_ptr<GfxClearDecoder> CreateFreeRdpClearDecoder();

}  // namespace hmrdp

#endif  // HMRDP_GFX_DESKTOP_H
