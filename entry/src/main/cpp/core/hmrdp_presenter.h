#ifndef HMRDP_PRESENTER_H
#define HMRDP_PRESENTER_H

#include <cstdint>
#include <memory>

namespace hmrdp {

// XEngine is the platform's spatial upscale (needs the device's XEG_spatial_upscale
// feature); FSR is AMD's FidelityFX Super Resolution 1.0 (EASU + RCAS, MIT) run by the
// presenter itself.
enum class SuperResolutionBackend {
  kXengine = 0,
  kFsr = 1,
};

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
  virtual bool Prepare() = 0;

  // A backend that can hand gdi a host-visible buffer it owns returns it here (with its
  // row pitch), so gdi composes straight into the memory the presenter uploads from.
  // Called once per frame until it succeeds, then never again; nullptr while the backend
  // cannot provide one.
  virtual uint8_t* AcquireDesktopBuffer(int width, int height, int* stride) {
    (void)width;
    (void)height;
    (void)stride;
    return nullptr;
  }
  virtual void BeginDesktopBufferWrite() {}
  virtual void ReleaseDesktopBuffer() {}
  // True while PresentBgra reads straight out of the backend's own buffer: the per-rect
  // cost is then a copy region + cache flush, so the rect *count* dominates and the merged
  // box wins; with a CPU copy in the way the bytes dominate and the rect list wins.
  virtual bool usesDesktopBuffer() const { return false; }

  // Only the Vulkan backend implements it (GLES is compatibility-only); the request is
  // remembered until the device and desktop size are known.
  virtual void SetSuperResolution(bool enabled, SuperResolutionBackend backend, int outputWidth,
                                  int outputHeight) {
    (void)enabled;
    (void)backend;
    (void)outputWidth;
    (void)outputHeight;
  }

  // Microseconds of the presents since the last call spent *blocked* on the display
  // (vkAcquireNextImageKHR / eglSwapBuffers) rather than working; reported as the blocked
  // `presentWait` sub-item, not client work.
  virtual uint64_t TakePresentWaitUs() { return 0; }

  // GPU time of the presents since the last call, split into dirty-rect upload (`copyUs`),
  // 超分 upscale (`srUs`) and clear + letterbox (`blitUs`); drains the window, all zero when
  // unmeasurable. This is *our* GPU work, not the GPU's total load - no public API exposes
  // the latter.
  virtual void TakeGpuTimings(uint64_t* copyUs, uint64_t* srUs, uint64_t* blitUs) {
    if (copyUs != nullptr) {
      *copyUs = 0;
    }
    if (srUs != nullptr) {
      *srUs = 0;
    }
    if (blitUs != nullptr) {
      *blitUs = 0;
    }
  }

  virtual bool PresentBgra(const uint8_t* data, int srcStride, int desktopWidth,
                           int desktopHeight, const PresentRect* rects, int rectCount) = 0;

  virtual void Reset() = 0;
};

// The "硬件加速" setting: whether frames may be presented through Vulkan at all.
// Process-wide, applied when a presenter is created; turning it off is the mode a device
// with a partial or untrusted Vulkan implementation needs.
void SetHardwareAccelEnabled(bool enabled);
bool HardwareAccelEnabled();

std::unique_ptr<FramePresenter> CreateFramePresenter();

}  // namespace hmrdp

#endif  // HMRDP_PRESENTER_H
