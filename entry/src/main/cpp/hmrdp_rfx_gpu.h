/*
 * HmRdp - GPU (GLES compute) RemoteFX/Progressive decoder + GPU surface engine.
 *
 * The CPU reference in hmrdp_rfx.{h,cpp} is validated pixel-exact against
 * FreeRDP. This module mirrors that exact integer pipeline in GLES 3.1 compute
 * shaders so the compressed tile streams can be decoded on the GPU, leaving the
 * CPU only the container parsing and a small metadata upload.
 *
 * GfxGpuDesktop extends that to the full GFX surface model (PERF-TODO §2.3-§2.5):
 * a `surfaceId -> GPU surface` registry with per-surface progressive state and
 * the four pixel operations (decode write / solid fill / surface copy / cache),
 * mirroring the CPU oracle hmrdp_gfx_desktop.{h,cpp} command for command.
 *
 * Devices without GLES 3.1 compute keep using the CPU reference (see
 * GetGpuComputeInfo()).
 */
#ifndef HMRDP_RFX_GPU_H
#define HMRDP_RFX_GPU_H

#include <cstdint>
#include <string>
#include <vector>

#include "hmrdp_rfx.h"

namespace hmrdp {

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

class GfxClearDecoder;

// Public metadata of one GPU-resident GFX surface (the GL buffers stay private
// to the engine). Dimensions/stride are aligned to 16 exactly like FreeRDP's
// gdiGfxSurface; `format` is the packed FreeRDP format (0x20 -> BGRX32,
// 0x21 -> BGRA32), mapped to the kPixelFormat* constants used by the CPU
// reference in hmrdp_gfx_desktop.h.
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
  // Bounding box of the region touched since the last Compose (mirrors the CPU
  // model; a conservative superset of FreeRDP's per-command invalid rects).
  bool dirtyValid = false;
  int dirtyLeft = 0;
  int dirtyTop = 0;
  int dirtyRight = 0;
  int dirtyBottom = 0;
};

// GPU desktop / surface engine: the GLES mirror of the CPU reference
// GfxDesktop in hmrdp_gfx_desktop.{h,cpp}. It owns an offscreen GLES 3.1
// context, one GPU surface buffer (+ progressive tile state) per `surfaceId`
// and a global bitmap cache, and reproduces the FreeRDP command semantics
// (PERF-TODO §2.3-§2.5) so the CPU model stays a pixel-exact oracle.
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
  // (mirrors the CPU GfxDesktop::Compose) and clears their dirty regions.
  // Returns true when the screen dirty region is non-empty.
  bool Compose();
  void ClearScreenDirty();
  bool screenDirty() const;
  int screenWidth() const { return screenW_; }
  int screenHeight() const { return screenH_; }
  // Full screen (top-down, `screenW*4` stride) as BGRA. Dev/verification only.
  bool ReadScreen(std::vector<uint8_t>* out);

  // --- Pixel commands (mirror GfxDesktop) ---------------------------------
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
};

// Offline self-test: replays `rfxPath` (hmrdp_rfx.bin) against `surfacePath`
// (hmrdp_rfx_surface.bin) entirely on the GPU and reports the pixel mismatch on
// the records that have a captured reference surface.
struct RfxGpuSelfTestResult {
  bool ran = false;       // GPU path initialised and replay started
  bool ok = false;        // all compared records matched (threshold > 2)
  uint64_t tiles = 0;
  uint64_t firstTiles = 0;
  uint64_t upgradeTiles = 0;
  uint64_t decodedOk = 0;
  uint64_t decodedFail = 0;
  uint64_t compared = 0;
  uint64_t mismatch = 0;
  double meanAbs = 0.0;
  int comparedRecords = 0;
  int badRecords = 0;
  std::string log;
};
RfxGpuSelfTestResult RunRfxGpuSelfTest(const std::string& rfxPath, const std::string& surfacePath);

// B2 offline self-test: replays the full GFX command stream (hmrdp_gfx.bin)
// through the GPU desktop surface model (progressive on the GPU, ClearCodec
// decoded on the CPU and uploaded) and compares it with the captured baselines
// (hmrdp_gfx_surface.bin: 'GFS1' full / 'GFH1' per-frame hash).
struct RfxGpuDesktopSelfTestResult {
  bool ran = false;
  bool ok = false;
  uint32_t records = 0;
  uint32_t comparedRecords = 0;
  uint32_t badRecords = 0;
  uint64_t compared = 0;
  uint64_t mismatch = 0;
  uint64_t surfacesHashed = 0;
  uint64_t hashMismatch = 0;
  double meanAbs = 0.0;
  std::string log;
};
RfxGpuDesktopSelfTestResult RunGfxGpuDesktopSelfTest(const std::string& gfxPath,
                                                     const std::string& surfacePath);

}  // namespace hmrdp

#endif  // HMRDP_RFX_GPU_H
