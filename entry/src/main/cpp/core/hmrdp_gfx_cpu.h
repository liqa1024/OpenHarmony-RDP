#ifndef HMRDP_GFX_CPU_H
#define HMRDP_GFX_CPU_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/client/rdpgfx.h>

namespace hmrdp {

class FramePresenter;
class GfxWorkMeter;

struct PresentUploadInfo {
  int64_t uploadedBytes = 0;
  int64_t boxBytes = 0;
  int rectCount = 0;
  int totalRects = 0;
  bool usedRects = false;
  bool rectListTruncated = false;
};

class GdiFrameHost {
 public:
  using PrePresentFn = std::function<bool(rdpGdi*)>;
  using PresentedFn = std::function<void(bool presented, uint64_t presentUs,
                                         const PresentUploadInfo& info)>;

  void SetPresenter(FramePresenter* presenter) { presenter_ = presenter; }
  void SetPrePresentFn(PrePresentFn fn) { prePresentFn_ = std::move(fn); }
  void SetPresentedFn(PresentedFn fn) { presentedFn_ = std::move(fn); }
  // The meter is per context, never a process-global slot: a replay running next to a
  // live session must not feed the session's meter.
  void SetMeter(GfxWorkMeter* meter) { meter_ = meter; }

  // Idempotent per frame: the presenter drains its pending wait on the first call, and
  // the blocked time is reported as `sync`, out of the pending `zgx+parse` window, so it
  // is never counted twice.
  void OnFrameBegin();
  void OnBeginPaint();

  bool OnEndPaint(rdpGdi* gdi, uint64_t* presentUs, PresentUploadInfo* info = nullptr);

  bool desktopAttached() const { return desktopAttached_; }
  void DetachDesktopBuffer() { desktopAttached_ = false; }

 private:
  FramePresenter* presenter_ = nullptr;
  GfxWorkMeter* meter_ = nullptr;
  PrePresentFn prePresentFn_;
  PresentedFn presentedFn_;
  bool desktopAttached_ = false;
};

class GfxCpuDesktop {
 public:
  GfxCpuDesktop() = default;
  ~GfxCpuDesktop();
  GfxCpuDesktop(const GfxCpuDesktop&) = delete;
  GfxCpuDesktop& operator=(const GfxCpuDesktop&) = delete;

  bool Init(int width, int height, std::string* error);
  void Shutdown();

  void SetPresenter(FramePresenter* presenter);
  void SetMeter(GfxWorkMeter* meter);
  void SetPrePresentFn(GdiFrameHost::PrePresentFn fn) { host_.SetPrePresentFn(std::move(fn)); }
  void SetPresentedFn(GdiFrameHost::PresentedFn fn) { host_.SetPresentedFn(std::move(fn)); }

  RdpgfxClientContext* gfx() const { return gfx_; }
  rdpGdi* gdi() const;

  // Both run *before* this frame can write the desktop, and the wait is idempotent per
  // frame: OnFrameBegin at the RDPGFX START_FRAME (the decode writes), OnBeginPaint at
  // update->BeginPaint, which FreeRDP calls after the frame's decode (the compose
  // writes). Whichever runs first waits.
  void OnFrameBegin();
  void OnBeginPaint();
  void OnEndPaint();
  void OnDesktopResize();

 private:
  bool Resize(int width, int height);

  freerdp* instance_ = nullptr;
  RdpgfxClientContext* gfx_ = nullptr;
  FramePresenter* presenter_ = nullptr;
  GfxWorkMeter* meter_ = nullptr;
  GdiFrameHost host_;
  int width_ = 0;
  int height_ = 0;
  bool resizing_ = false;
};

bool InitGdiWithPresenter(freerdp* instance, FramePresenter* presenter, int width, int height);

bool AttachPresenterDesktopBuffer(rdpGdi* gdi, FramePresenter* presenter);

bool PresentGdiFrame(rdpGdi* gdi, FramePresenter* presenter,
                     PresentUploadInfo* info = nullptr);

}  // namespace hmrdp

#endif  // HMRDP_GFX_CPU_H
