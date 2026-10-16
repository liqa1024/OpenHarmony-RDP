#ifndef HMRDP_PRESENTER_H
#define HMRDP_PRESENTER_H

#include <cstdint>
#include <memory>

namespace hmrdp {

// XEngine = platform spatial upscale (needs XEG_spatial_upscale); FSR = AMD FidelityFX SR 1.0
// (EASU + RCAS) run by the presenter.
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

  // A backend that owns a host-visible buffer returns it (with row pitch) so gdi composes directly
  // into the upload source; called once per frame until it succeeds, then never again; nullptr while unavailable.
  virtual uint8_t* AcquireDesktopBuffer(int width, int height, int* stride) {
    (void)width;
    (void)height;
    (void)stride;
    return nullptr;
  }
  virtual void BeginDesktopBufferWrite() {}
  virtual void ReleaseDesktopBuffer() {}
  // True while PresentBgra reads straight from the backend buffer: per-rect cost is a copy + flush, so
  // rect count dominates and the merged box wins; with a CPU copy the bytes dominate and the rect list wins.
  virtual bool usesDesktopBuffer() const { return false; }

  // Only Vulkan implements it (GLES is compatibility-only); remembered until the device and desktop size are known.
  virtual void SetSuperResolution(bool enabled, SuperResolutionBackend backend, int outputWidth,
                                  int outputHeight) {
    (void)enabled;
    (void)backend;
    (void)outputWidth;
    (void)outputHeight;
  }

  // Microseconds of the presents since the last call spent blocked on the display (vkAcquireNextImageKHR /
  // eglSwapBuffers); reported as the `presentWait` sub-item, not client work.
  virtual uint64_t TakePresentWaitUs() { return 0; }

  // GPU time of the presents since the last call, split into upload (`copyUs`), 超分 (`srUs`) and
  // clear+letterbox (`blitUs`); this is our GPU work, not the GPU's total load (no API exposes that).
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

  // Moonlight 超分: present one decoded frame straight from its ohos graphics-allocator buffer (an
  // offscreen consumer surface buffer, NV12), importing it into the presenter's own device,
  // converting to the picture format and running the shared 超分 / letterbox tail. `inputWidth`/
  // `inputHeight` are the stream (session) resolution. Blocking per frame: the caller returns the
  // buffer the moment this returns, so the presenter must be done with it. False when the presenter
  // has no such path (GLES, or a device without the import / YCbCr-sampler support).
  virtual bool PresentVideo(const void* nativeBuffer, int inputWidth, int inputHeight) {
    (void)nativeBuffer;
    (void)inputWidth;
    (void)inputHeight;
    return false;
  }

  virtual void Reset() = 0;
};

// The "硬件加速" setting: whether frames may go through Vulkan at all. Process-wide, applied when a
// presenter is created; off is for a device with a partial or untrusted Vulkan implementation.
void SetHardwareAccelEnabled(bool enabled);
bool HardwareAccelEnabled();

std::unique_ptr<FramePresenter> CreateFramePresenter();

}

#endif
