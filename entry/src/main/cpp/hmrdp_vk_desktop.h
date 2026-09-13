/*
 * HmRdp - Vulkan GFX surface engine (V1 scope, VULKAN-TODO.md §5 V1).
 *
 * Reproduces the FreeRDP RDPGFX command semantics that GfxGpuDesktop
 * (hmrdp_rfx.{h,cpp}) implements on GLES 3.1 compute, so the replay harness, the
 * live shadow compare and the dev panel keep working unchanged (VULKAN-TODO
 * §4.3): solid fill, surface-to-surface copy, bitmap cache, uncompressed upload,
 * output mapping, screen compose and dirty tracking.
 *
 * Design decisions specific to this backend:
 *
 *  - Every pixel resource (surface / cache entry / screen / temp) is a
 *    `VkImage` in `VK_FORMAT_B8G8R8A8_UNORM`, which is exactly FreeRDP's packed
 *    BGRA/BGRX byte order - so no channel swizzling is needed anywhere on the
 *    CPU boundary.
 *  - All images live in `VK_IMAGE_LAYOUT_GENERAL` for their whole lifetime.
 *    GENERAL is legal for transfer, colour-attachment and storage use, so there
 *    is no layout state machine to get wrong; VULKAN-TODO §7.2 names
 *    layout/barrier bugs the number-one Vulkan trap, so V1 trades a little
 *    potential bandwidth for provable correctness. Revisit once correctness is
 *    pinned by the compare harness.
 *  - Hazards between operations are covered by one coarse `VkMemoryBarrier`
 *    (all writes -> all reads), emitted lazily only when something was written
 *    since the last one. No `vkDeviceWaitIdle` per command (VULKAN-TODO §7.3).
 *  - Commands are recorded into one primary command buffer and submitted at
 *    natural boundaries (`Flush()`: readback, present, teardown).
 *
 * Not implemented in V1: Progressive and ClearCodec. Those commands leave the
 * surface untouched, and `unsupportedSeen()` tells a correctness run to exclude
 * the affected frames from its claim (V1 acceptance: fill/copy/uncompressed
 * `bad=0`).
 */
#ifndef HMRDP_VK_DESKTOP_H
#define HMRDP_VK_DESKTOP_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "hmrdp_rfx.h"  // GpuSurface / GpuCmd / GpuCodec / kPixelFormat*
#include "hmrdp_vk_context.h"

namespace hmrdp {

class VkRenderer;

class GfxVkDesktop {
 public:
  GfxVkDesktop();
  ~GfxVkDesktop();

  GfxVkDesktop(const GfxVkDesktop&) = delete;
  GfxVkDesktop& operator=(const GfxVkDesktop&) = delete;

  // Brings up the device (no surface needed: the offline harness has none) and
  // the shared command buffer.
  //
  // `format` is the pixel format of every engine image. The default is FreeRDP's
  // own byte order, so the offline compare harness needs no channel swizzling at
  // all (its result must not be able to "cancel out" a swizzle bug). The
  // presentation path passes the swapchain format instead, which on the current
  // devices is RGBA8; the engine then swaps R/B on its CPU boundaries and the
  // renderer blits image-to-image with no CPU round trip.
  bool Init(VkFormat format = VK_FORMAT_B8G8R8A8_UNORM);
  void Reset();
  bool ready() const;
  // True when the engine images are RGBA8 and the CPU boundary swaps R/B (so
  // ReadScreen/ReadSurface still return FreeRDP's BGRA bytes).
  bool swapRb() const;

  // --- Surface lifecycle (same rules as GfxGpuDesktop) ----------------------
  // Aligns width/height to 16, fills with 0xFF. `format` is the wire format
  // (0x20 -> BGRX32, 0x21 -> BGRA32); either way the pixels stay BGRA bytes.
  bool CreateSurface(uint16_t surfaceId, int width, int height, uint32_t format);
  void DeleteSurface(uint16_t surfaceId);
  const GpuSurface* FindSurface(uint16_t surfaceId) const;
  // 1:1 output mapping only; scaled mappings unmap the surface (unsupported,
  // exactly like the current gdi/GLES behaviour).
  void MapSurfaceToOutput(uint16_t surfaceId, uint32_t outputOriginX, uint32_t outputOriginY);

  // --- Screen (front buffer) -----------------------------------------------
  bool ResetGraphics(int width, int height);
  // Composites every output-mapped surface's dirty region into the screen and
  // clears their dirty regions. Returns true when the screen dirty region is
  // non-empty.
  bool Compose();
  void ClearScreenDirty();
  bool screenDirty() const;
  int screenWidth() const;
  int screenHeight() const;
  // Screen image (VK_IMAGE_LAYOUT_GENERAL) for the renderer to blit.
  VkImage screenImage() const;
  VkFormat format() const;
  // Full screen (top-down, `screenW*4` stride, 0xFF-initialised) as BGRA.
  bool ReadScreen(std::vector<uint8_t>* out);

  // --- Pixel commands ------------------------------------------------------
  void ApplyCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                    const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                    uint32_t payloadLen);

  bool SolidFill(uint16_t surfaceId, uint32_t bgraPixel, const uint16_t* rects,
                 uint32_t rectCount);
  // Uploads a BGRA rect into the surface (uncompressed 24/32bpp).
  bool UploadBgra(uint16_t surfaceId, int left, int top, int width, int height,
                  const uint8_t* bgra, int srcStride);
  bool SurfaceToCache(uint16_t surfaceId, uint16_t slot, int x, int y, int width, int height);
  bool CacheToSurface(uint16_t surfaceId, uint16_t slot, int dstX, int dstY);
  void EvictCache(uint16_t slot);
  bool SurfaceToSurface(uint16_t srcSurfaceId, int srcX, int srcY, int width, int height,
                        uint16_t dstSurfaceId, int dstX, int dstY);

  // Full surface (top-down, `stride` bytes) as BGRA. Dev/verification only.
  bool ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out);

  // --- Diagnostics ---------------------------------------------------------
  // True when a command V1 does not implement (Progressive / ClearCodec) was
  // seen since resetUnsupportedSeen(): the compare harness uses this to exclude
  // frames the engine is not yet expected to reproduce.
  bool unsupportedSeen() const;
  void resetUnsupportedSeen();
  std::string Stats() const;
  std::string lastError() const;

  // Ends recording, submits and waits. Required before any CPU readback, before
  // presenting, and before destroying resources work still references.
  bool Flush();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool ready_ = false;
  int screenW_ = 0;
  int screenH_ = 0;
};

// Composes the Vulkan engine screen and presents it through `renderer`. Same
// contract as GpuPresentComposed: returns true when a frame reached the screen,
// false when nothing was dirty (a static desktop must not present).
bool GpuVkPresentComposed(GfxVkDesktop* engine, VkRenderer* renderer);

}  // namespace hmrdp

#endif  // HMRDP_VK_DESKTOP_H
