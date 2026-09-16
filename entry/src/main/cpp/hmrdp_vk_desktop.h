/*
 * HmRdp - Vulkan GFX surface engine (V2 scope, doc_agent/gfx-engine.md §1).
 *
 * Reproduces the FreeRDP RDPGFX command semantics (doc_agent/gfx-engine.md §0.2-§0.4)
 * so the replay harness and the dev panel keep working: solid fill,
 * surface-to-surface copy, bitmap cache, uncompressed upload, output mapping,
 * screen compose and dirty tracking.
 *
 * V2 storage model (doc_agent/gfx-engine.md §1):
 *
 *  - Surfaces and bitmap-cache entries are **persistent-mapped host-visible
 *    linear `VkBuffer`s**, not images. Their row pitch is the FreeRDP `stride`
 *    (16B-aligned width*4), so the CPU addresses any pixel directly with no
 *    staging, no readback and no layout state machine. This is what lets V4's
 *    ClearCodec read-modify-write surface pixels on the CPU, and what V3's
 *    Progressive compute shader writes tiles into (the buffers already carry
 *    `STORAGE_BUFFER` usage).
 *  - The screen stays a `VkImage` (`VK_IMAGE_LAYOUT_GENERAL`) because it is the
 *    presentation source. Compose copies each mapped surface's dirty region into
 *    it with `vkCmdCopyBufferToImage`, carrying the stride via `bufferRowLength`.
 *    The region is the **list of rects the commands actually touched**, not their
 *    merged box: the GPU route follows the same policy as the CPU (gdi) present
 *    path (doc_agent/gfx-engine.md §2.3), with the box kept only as the bounded
 *    fallback above `GpuSurface::kMaxDirtyRects`.
 *  - Pixel commands (fill / upload / cache / copy) run on the CPU against the
 *    mapping, exactly as FreeRDP's gdi path does. `ReadSurface` is a plain copy
 *    out of the mapping; only `ReadScreen` and `Flush` touch the device.
 *  - Image format is `VK_FORMAT_B8G8R8A8_UNORM` (FreeRDP's byte order) for the
 *    offline harness; when the swapchain forces RGBA8 the CPU boundaries swap
 *    R/B so the renderer can still blit image-to-image.
 *  - Hazards are covered by one coarse `VkMemoryBarrier`, emitted lazily and
 *    split into a HOST_WRITE leg (CPU-written surfaces) and a TRANSFER_WRITE leg
 *    (device-written screen), both targeting TRANSFER reads. No
 *    `vkDeviceWaitIdle` per command (doc_agent/gfx-engine.md §3).
 *  - Commands are recorded into one primary command buffer and submitted at
 *    natural boundaries (`Flush()`: readback, present, teardown).
 *
 * On top of that the engine decodes RemoteFX Progressive on the GPU (V3) and
 * ClearCodec on the CPU against the mapped surface (V4). Codec commands it still
 * cannot serve (Planar / Alpha / RemoteFX non-progressive, CAPROGRESSIVE_V2)
 * leave the surface untouched and are counted in `Stats()` - never silent.
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
  // the shared command buffer. Storage/picture format is fixed at FreeRDP's BGRA
  // byte order - the engine does not follow the swapchain, the presenter converts
  // (doc_agent/gfx-engine.md §2.3) - so there is nothing to configure and no R/B
  // swap anywhere in the engine.
  bool Init();
  void Reset();
  bool ready() const;

  // --- Surface lifecycle -----------------------------------------------------
  // Allocates a persistent-mapped host-visible buffer, aligns width/height to
  // 16 and fills it with 0xFF. `format` is the wire format (0x20 -> BGRX32,
  // 0x21 -> BGRA32); either way the pixels stay BGRA bytes.
  bool CreateSurface(uint16_t surfaceId, int width, int height, uint32_t format);
  void DeleteSurface(uint16_t surfaceId);
  // 1:1 output mapping only; scaled mappings unmap the surface (unsupported,
  // exactly like the gdi behaviour).
  void MapSurfaceToOutput(uint16_t surfaceId, uint32_t outputOriginX, uint32_t outputOriginY);

  // --- Screen (front buffer) -----------------------------------------------
  bool ResetGraphics(int width, int height);
  // Composites every output-mapped surface's dirty rects into the screen and
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

  // --- Diagnostics ---------------------------------------------------------
  // One-line engine summary (surface/cache counts, per-command timings, the
  // number of commands whose codec is not implemented, ...). Unimplemented or
  // failed codec commands are counted and visible here, never silent (V5).
  std::string Stats() const;

  // Triage for a pixel mismatch (doc_agent/gfx-engine.md §6/§7): the screen-level
  // compare can only report "these pixels differ". Reading the same pixel back out
  // of every mapped surface that covers it says which side is wrong:
  //   * the surface already holds the reference's value -> the surface content is
  //     right and the *compose* (the dirty rect list) never copied it;
  //   * the surface holds some other value -> the divergence is in the codec /
  //     command path that produced the pixels, not in the compose.
  // Values are FreeRDP-order BGRA, exactly like ReadScreen. Returns the number of
  // hits written; 0 means no mapped surface covers the point.
  struct ScreenPixelHit {
    uint16_t surfaceId = 0;
    int surfaceX = 0;
    int surfaceY = 0;
    uint32_t bgra = 0;
  };
  int ProbeScreenPixel(int x, int y, ScreenPixelHit* out, int capacity);

  // Perf accounting for the present strategy (doc_agent/gfx-engine.md §2.3): one
  // present's host-time split - record the dirty compose, submit+wait, then blit the
  // whole screen to the swapchain and present. Reported in Stats().
  void NotePresentSplitUs(uint64_t composeUs, uint64_t flushUs, uint64_t blitUs);
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

// Composes the Vulkan engine screen and presents it through `renderer`. Returns
// true when a frame reached the screen, false when nothing was dirty (a static
// desktop must not present).
bool GpuVkPresentComposed(GfxVkDesktop* engine, VkRenderer* renderer);

}  // namespace hmrdp

#endif  // HMRDP_VK_DESKTOP_H
