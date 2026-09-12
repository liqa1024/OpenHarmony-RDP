/*
 * HmRdp - GPU (GLES compute) RemoteFX/Progressive tile decoder.
 *
 * The CPU reference in hmrdp_rfx.{h,cpp} is validated pixel-exact against
 * FreeRDP. This module mirrors that exact integer pipeline in GLES 3.1 compute
 * shaders so the compressed tile streams can be decoded on the GPU, leaving the
 * CPU only the container parsing and a small metadata upload.
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

// One 64x64x3 progressive surface, decoded on the GPU. The decoder owns an
// offscreen GLES 3.1 context and the device buffers for the tile coefficient
// state, so it can be driven message by message.
class RfxGpuDecoder {
 public:
  RfxGpuDecoder();
  ~RfxGpuDecoder();

  RfxGpuDecoder(const RfxGpuDecoder&) = delete;
  RfxGpuDecoder& operator=(const RfxGpuDecoder&) = delete;

  // `gridW`/`gridH` are the tile grid; `surfaceW`/`surfaceH` the desktop size.
  bool Init(int gridW, int gridH, int surfaceW, int surfaceH);
  void Reset();
  bool ready() const { return ready_; }

  // Decodes one Progressive message payload ("WBT" container as captured by the
  // rfx dump), updating the persistent tile state and the internal surface.
  // Returns false on GL failure.
  bool DecodeMessage(const uint8_t* payload, size_t size);

  // BGRA surface (top-down, `surfaceW*4` stride) as of the last DecodeMessage.
  bool ReadSurface(std::vector<uint8_t>* out);

  // --- B2: GPU desktop (surface) commands ---------------------------------
  // The surface created by Init is the single GFX output surface used by the
  // captured streams (PERF-TODO §2.5 "B2"). ClearCodec is decoded on the CPU by
  // the caller and uploaded via UploadBgra; every other command runs on the GPU
  // (compute), mirroring the CPU reference in hmrdp_gfx_desktop.cpp.
  bool SolidFill(uint32_t bgraPixel, const uint16_t* rects, uint32_t rectCount);
  // Uploads a BGRA rect into the surface (used for ClearCodec / uncompressed).
  bool UploadBgra(int left, int top, int width, int height, const uint8_t* bgra, int srcStride);
  // Full-width row transfer, used to emulate ClearCodec (which must read the
  // existing surface content for the pixels its bands do not overwrite).
  bool DownloadRows(int top, int height, uint8_t* dst, int dstStride);
  bool UploadRows(int top, int height, const uint8_t* src, int srcStride);
  bool SurfaceToCache(uint16_t slot, int x, int y, int width, int height);
  bool CacheToSurface(uint16_t slot, int dstX, int dstY);
  void EvictCache(uint16_t slot);
  bool SurfaceToSurface(int srcX, int srcY, int width, int height, int dstX, int dstY);

  int surfaceWidth() const { return surfaceW_; }
  int surfaceHeight() const { return surfaceH_; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool ready_ = false;
  int gridW_ = 0;
  int gridH_ = 0;
  int surfaceW_ = 0;
  int surfaceH_ = 0;
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
