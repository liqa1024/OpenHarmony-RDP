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

  // --- zero-copy desktop buffer (doc_agent/cpu-path.md §4) --------------
  // A backend that can hand gdi a host-visible frame buffer it owns returns it
  // here, so gdi composes the desktop straight into the memory the presenter
  // uploads from and no per-frame copy of the frame happens. `*stride` receives
  // the buffer's row pitch in bytes: the caller must pass exactly that to gdi
  // (a caller-provided buffer only works when gdi's stride matches).
  // Returns nullptr while the backend cannot provide one - the caller then keeps
  // gdi's own buffer and PresentBgra copies the dirty rects, as before. It is
  // called once per frame until it succeeds, then never again.
  virtual uint8_t* AcquireDesktopBuffer(int width, int height, int* stride) {
    (void)width;
    (void)height;
    (void)stride;
    return nullptr;
  }
  // Called before a frame's pixels are written into the buffer AcquireDesktopBuffer
  // returned: waits (when needed) for the GPU to be done reading the previous frame
  // out of it. A no-op for backends without a desktop buffer.
  virtual void BeginDesktopBufferWrite() {}
  // Releases the buffer (resize/teardown). Safe to call when none was acquired.
  virtual void ReleaseDesktopBuffer() {}
  // True while PresentBgra reads the pixels straight out of the backend's own
  // buffer, i.e. per-rect work costs the presenter a copy region + a cache flush
  // instead of a CPU copy. The caller picks the cheap dirty shape accordingly:
  // with a CPU copy in the way the byte count dominates and the rect list wins,
  // without one the rect *count* dominates and the merged box wins
  // (hmrdp_gfx_cpu.cpp PresentGdiFrame, doc_agent/cpu-path.md §4).
  virtual bool usesDesktopBuffer() const { return false; }

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

// The "硬件加速" setting: whether frames may be presented through Vulkan at all.
// Process-wide, applied when a presenter is created, so turning it off keeps the
// session path free of Vulkan (GLES is then the only backend) - that is the mode
// a device with a partial or untrusted Vulkan implementation needs, and it is
// what the emulator runs on. Default on.
void SetHardwareAccelEnabled(bool enabled);
bool HardwareAccelEnabled();

// Picks the presenter backend from the (cached) Vulkan presenter capability and
// the hardware-acceleration setting, and falls back to GLES. Never returns
// nullptr.
std::unique_ptr<FramePresenter> CreateFramePresenter();

}  // namespace hmrdp

#endif  // HMRDP_PRESENTER_H
