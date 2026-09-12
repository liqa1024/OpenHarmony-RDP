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

}  // namespace hmrdp

#endif  // HMRDP_RFX_GPU_H
