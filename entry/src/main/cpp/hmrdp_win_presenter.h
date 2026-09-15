/*
 * HmRdp - CPU frame presenter on the XComponent's native window buffer queue.
 *
 * This is the base present path: it hands the frame to the display stack through
 * `native_window` (OH_NativeWindow_NativeWindowRequestBuffer / FlushBuffer), i.e.
 * **without Vulkan, EGL or GLES**. A device whose Vulkan driver is missing or
 * unusable still shows the gdi-decoded desktop, which is what keeps the CPU
 * (soft-decode) chain maximally compatible; the GPU engine is the optional part.
 *
 * The engine screen cannot go through this path (it lives in a VkImage and would
 * need a GPU->CPU readback), so the two backends split by frame source:
 *   - CPU / gdi frames  -> WinPresenter (this file);
 *   - Vulkan engine frames -> VkRenderer (hmrdp_vk_renderer.*).
 *
 * Layout: the window buffer is the XComponent surface size and the remote desktop
 * is copied 1:1, centred, with black letterbox bars (centre-cropped when the
 * desktop is larger than the surface). Buffer rows are written with the stride
 * the buffer handle reports, and R/B are swapped when the buffer is RGBA-ordered
 * (FreeRDP hands us BGRA) - the order is read from the first buffer's handle, not
 * guessed.
 *
 * The window buffers are recycled by the compositor, so their content cannot be
 * relied on between frames: the remote desktop is accumulated in a CPU-side
 * buffer (dirty rectangle only) and the whole picture is rewritten on every
 * present. That is one memcpy of the desktop per frame - acceptable for the soft
 * path, and the reason the engine path exists at all.
 */
#ifndef HMRDP_WIN_PRESENTER_H
#define HMRDP_WIN_PRESENTER_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct NativeWindowBuffer;

namespace hmrdp {

class WinPresenter {
 public:
  WinPresenter() = default;
  ~WinPresenter();

  WinPresenter(const WinPresenter&) = delete;
  WinPresenter& operator=(const WinPresenter&) = delete;

  // Called from the ArkUI (main) thread when the XComponent surface changes.
  void SetSurface(void* nativeWindow, int width, int height);
  void ResizeSurface(int width, int height);
  void DestroySurface();
  // Paints the surface black so it shows something before the first frame.
  void Prepare();

  // Called from the FreeRDP worker thread. `data` is the whole desktop buffer
  // (top-down BGRA, `srcStride` bytes/row) and `x,y,width,height` the region that
  // changed. Returns true when a frame reached the surface.
  bool PresentBgra(const uint8_t* data, int srcStride, int desktopWidth, int desktopHeight,
                   int x, int y, int width, int height);

  void Reset();

  bool ready() const;

 private:
  // Presents one solid black frame (Prepare's body).
  bool PresentSolidBlackLocked();
  // Request -> map -> (callback) -> unmap -> flush. The callback returns false to
  // abort without flushing.
  bool WithMappedBufferLocked(const std::function<bool(uint8_t* pixels, int stride, int width,
                                                       int height)>& write);
  bool AccumulateLocked(const uint8_t* data, int srcStride, int desktopWidth, int desktopHeight,
                        int x, int y, int width, int height);
  void CopyDesktopLocked(uint8_t* dst, int dstStride, int dstWidth, int dstHeight);
  void ResetLocked();

  mutable std::mutex mutex_;
  // The XComponent OHNativeWindow (opaque here: native_window/external_window.h
  // stays out of this header).
  void* window_ = nullptr;
  int surfaceWidth_ = 0;
  int surfaceHeight_ = 0;
  // Set when the buffer geometry has to be (re)applied before the next request.
  bool geometryDirty_ = true;
  // Remote desktop accumulation buffer (top-down BGRA, `desktopWidth_ * 4` rows).
  std::vector<uint8_t> desktop_;
  int desktopWidth_ = 0;
  int desktopHeight_ = 0;
  // The next present must take the whole desktop from the caller's frame (the
  // accumulation buffer was just (re)created, or the desktop changed size).
  bool fullUpload_ = true;
  // True when the window buffer stores RGBA while FreeRDP hands us BGRA; learnt
  // from the first buffer handle (never guessed).
  bool swapRb_ = true;
  bool formatLogged_ = false;
  // Throttled error logging (a broken surface must not flood hilog per frame).
  int errorLogs_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_WIN_PRESENTER_H
