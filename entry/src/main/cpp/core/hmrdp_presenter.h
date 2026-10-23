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

// The static metadata (SMPTE 2086 + CTA-861.3) describing the HDR picture this presenter hands to the
// compositor, in the units the platform takes: chromaticities as x/y, luminances in nits.
//
// `valid == false` means the host described nothing usable and the platform gets this build's BT.2020
// 1000-nit default instead. It matters because the compositor converts the picture to the panel from
// what it is told about it: the values a host actually sends win when they are usable, and the default
// is only the fallback (the same default the reference clients inject).
struct HdrStaticMetadata {
  bool valid = false;
  float redX = 0.708f;
  float redY = 0.292f;
  float greenX = 0.170f;
  float greenY = 0.797f;
  float blueX = 0.131f;
  float blueY = 0.046f;
  float whiteX = 0.3127f;
  float whiteY = 0.3290f;
  float maxLuminance = 1000.0f;
  float minLuminance = 0.001f;
  float maxContentLightLevel = 1000.0f;
  float maxFrameAverageLightLevel = 400.0f;
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

  // HDR: the HDR base transfer the host confirmed it switched its output to, so the frames handed to
  // PresentVideo are that encoding: 0 = SDR (Rec.709), 1 = HDR10 (Rec.2020 PQ), 2 = HLG (Rec.2020
  // ARIB STD-B67). Remembered until the swapchain and the output stage are (re)built, exactly like
  // SetSuperResolution - the answer usually arrives after this presenter exists. A host that never
  // reports HDR sends an SDR stream whatever was requested.
  virtual void SetStreamHdr(int base) { (void)base; }
  // What the HDR picture is, told to the window the swapchain presents into (colour space, gamut,
  // metadata type and this metadata). The host answers this while the session runs, right before the
  // HDR state, so it is remembered until the window declaration is (re)written.
  virtual void SetHdrStaticMetadata(const HdrStaticMetadata& metadata) { (void)metadata; }
  // The per-frame HDR Vivid metadata the host carries inside a standard HEVC stream (a registered
  // T.35 SEI), handed to the window with the HDR declaration so the compositor can tone-map it.
  // `payload` is the de-escaped registered payload, nullptr / 0 clears it (see hmrdp_hdr_dynamic.h).
  virtual void SetStreamDynamicHdr(const uint8_t* payload, int size) {
    (void)payload;
    (void)size;
  }
  // Whether the HDR output stage is actually running (the HDR swapchain was built). The toolbar
  // reports this rather than the request, so a fallback to SDR is never claimed as HDR.
  virtual bool hdrActive() const { return false; }
  // Whether the stream really carries HDR Vivid, so the toolbar names the dynamic layer rather than
  // only the base transfer. Never true while the output stage is not running HDR.
  virtual bool vividHdrActive() const { return false; }

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

  // The video path: present one decoded frame straight from the buffer the codec handed out (NV12),
  // importing it into the presenter's own device, converting to the picture format and running the
  // shared tail — 超分 when the session asked for it, then letterbox. `geometry` is the codec's own
  // description of the frame (see hmrdp_video_frame.h). Returns a token for WaitVideoFrame when the
  // frame was submitted, or -1 when nothing was submitted (the caller may return the buffer
  // immediately; the GPU may still be reading it until WaitVideoFrame(token) returns). -1 also
  // answers "this frame cannot be presented at all", and since the mode cannot change mid-session a
  // run of them is a black screen the session has to report rather than work around.
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
  // The answer is only consulted when the session starts — the output mode cannot change afterwards,
  // so a presenter that says no here leaves the decoder on its own output surface for the session.
  virtual bool supportsVideo() const { return false; }

  // The 超分 state the session requested and what the presenter actually runs (input smaller than the
  // output, and the upscale object built). A request that is not active means the session fell back
  // to presenting the input picture; the caller reports that instead of claiming 超分 is on.
  virtual bool superResolutionRequested() const { return false; }
  virtual bool superResolutionActive() const { return false; }

  // Moonlight 超分: the colour space the session negotiated with the host for the frames handed to
  // PresentVideo (OH_NativeBuffer_ColorSpace, 0 = not stated). The importer presents what each buffer
  // itself declares; this is only what that declaration is checked against, and videoColorMismatch()
  // reports the disagreement so the session can say so instead of showing wrong colours silently.
  virtual void SetVideoColorSpace(int colorSpace) { (void)colorSpace; }
  virtual bool videoColorMismatch() const { return false; }

  virtual void Reset() = 0;
};

// The "硬件加速" setting: whether frames may go through Vulkan at all. Process-wide, applied when a
// presenter is created; off is for a device with a partial or untrusted Vulkan implementation.
void SetHardwareAccelEnabled(bool enabled);
bool HardwareAccelEnabled();

std::unique_ptr<FramePresenter> CreateFramePresenter();

}

#endif
