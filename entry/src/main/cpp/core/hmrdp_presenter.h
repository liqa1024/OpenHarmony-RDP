#ifndef HMRDP_PRESENTER_H
#define HMRDP_PRESENTER_H

#include <cstdint>
#include <memory>

#include "hmrdp_video_frame.h"

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

  // Moonlight 超分: present one decoded frame straight from the buffer the codec handed out (NV12),
  // importing it into the presenter's own device, converting to the picture format and running the
  // shared 超分 / letterbox tail. `geometry` is the codec's own description of the frame (see
  // hmrdp_video_frame.h). Returns a token for WaitVideoFrame when the frame was submitted, or -1
  // when nothing was submitted (the caller may return the buffer immediately). The GPU may still be
  // reading the buffer until WaitVideoFrame(token) returns.
  virtual int PresentVideo(const void* nativeBuffer, const VideoFrameGeometry& geometry) {
    (void)nativeBuffer;
    (void)geometry;
    return -1;
  }

  // Waits until the GPU no longer reads the codec buffer presented as `token`. A no-op for an
  // unknown token or when the frame was never submitted.
  virtual void WaitVideoFrame(int token) { (void)token; }

  // Whether this presenter can consume codec buffers at all. The session must ask before it puts the
  // decoder into buffer mode: a presenter that cannot import would leave the stream with no output.
  virtual bool supportsVideo() const { return false; }

  // The 超分 state the session requested and what the presenter actually runs (input smaller than the
  // output, and the upscale object built). A request that is not active means the session fell back
  // to presenting the input picture; the caller reports that instead of claiming 超分 is on.
  virtual bool superResolutionRequested() const { return false; }
  virtual bool superResolutionActive() const { return false; }

  virtual void Reset() = 0;
};

// The "硬件加速" setting: whether frames may go through Vulkan at all. Process-wide, applied when a
// presenter is created; off is for a device with a partial or untrusted Vulkan implementation.
void SetHardwareAccelEnabled(bool enabled);
bool HardwareAccelEnabled();

std::unique_ptr<FramePresenter> CreateFramePresenter();

}

#endif
