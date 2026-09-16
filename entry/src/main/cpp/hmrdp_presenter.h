/*
 * HmRdp - frame presenter interface for CPU (gdi) frames.
 *
 * Only one thing lives here: getting an already-decoded desktop frame onto the
 * XComponent surface. Decoding and composition are not its business.
 *
 * Two backends, picked by CreateFramePresenter():
 *
 *  - Vulkan (VkRenderer): the default. The dirty rectangle is copied once into a
 *    host-visible staging buffer, uploaded into a persistent desktop image and
 *    the GPU does the letterboxed blit to the swapchain - i.e. the CPU only
 *    hands the changed pixels over and never writes display memory itself. The
 *    same class also presents the engine's screen image (PresentImage), so the
 *    CPU and engine frame sources share one implementation.
 *  - GLES (GlesPresenter): the compatibility fallback, mainly for devices whose
 *    Vulkan cannot present (the emulator). Same shape - dirty-rect texture
 *    upload (ES3 + GL_UNPACK_ROW_LENGTH) and a letterboxed quad - so the only
 *    copy of the frame is a driver-side texture upload.
 *
 * Which backend is used is decided by the *presenter* capability
 * (VulkanCapabilities::presenterSupported), which is deliberately looser than the
 * engine/hardware-decode one: presenting needs no compute, only a device that can
 * blit to the XComponent surface (see doc_agent/gfx-engine.md §2.3).
 */
#ifndef HMRDP_PRESENTER_H
#define HMRDP_PRESENTER_H

#include <cstdint>
#include <memory>

namespace hmrdp {

// One changed region of the desktop, in desktop pixels. A present receives the
// whole list of regions FreeRDP invalidated for this frame, not their merged
// bounding box: the box of a handful of scattered updates can be several times
// the pixels that actually changed (the gdi pipeline keeps the individual rects
// in `hwnd->cinvalid`, see libfreerdp/gdi/region.c gdi_InvalidateRegion).
struct PresentRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

class FramePresenter {
 public:
  virtual ~FramePresenter() = default;

  // Called from the ArkUI (main) thread when the XComponent surface changes.
  virtual void SetSurface(void* nativeWindow, int width, int height) = 0;
  virtual void ResizeSurface(int width, int height) = 0;
  virtual void DestroySurface() = 0;
  // Paints the surface black so it shows something before the first frame.
  // False when the backend could not bring its context/surface up (the caller
  // keeps going: the next present retries).
  virtual bool Prepare() = 0;

  // Called from the FreeRDP worker thread. `data` is the whole desktop frame
  // (top-down BGRA, `srcStride` bytes/row), `desktopWidth/Height` the desktop
  // dimensions used for the letterbox, and `rects`/`rectCount` the regions that
  // changed (at least one; the caller clips them to the desktop). Overlapping
  // rects are allowed - the pixels copied are identical. Returns true when a
  // frame reached the surface.
  virtual bool PresentBgra(const uint8_t* data, int srcStride, int desktopWidth,
                           int desktopHeight, const PresentRect* rects, int rectCount) = 0;

  virtual void Reset() = 0;
};

// Picks the presenter backend from the (cached) Vulkan presenter capability and
// falls back to GLES. Never returns nullptr.
std::unique_ptr<FramePresenter> CreateFramePresenter();

}  // namespace hmrdp

#endif  // HMRDP_PRESENTER_H
