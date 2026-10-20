#ifndef HMRDP_GFX_CPU_H
#define HMRDP_GFX_CPU_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/client/rdpgfx.h>

#include "hmrdp_display_rate.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

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
  // Per context, never process-global: a replay next to a live session must not feed the
  // session's meter.
  void SetMeter(GfxWorkMeter* meter) { meter_ = meter; }

  // Idempotent per frame: the first call drains the presenter's pending wait, reported as
  // `sync` outside the pending `zgx+parse` window so it is never counted twice.
  void OnFrameBegin();
  void OnBeginPaint();

  bool OnEndPaint(rdpGdi* gdi, uint64_t* presentUs, PresentUploadInfo* info = nullptr);

  // Live sessions coalesce presents to the display's gear: a frame landing inside the current
  // refresh period leaves its dirty box to the next present instead of going out on its own. The
  // desktop is a state, so only the newest one has to be shown, and the display cannot show more
  // than one per period anyway. Off for the replay, which presents every frame it feeds.
  void SetPresentPacing(bool enabled);

  // Microseconds until a carried present may go (0 = now), -1 when nothing is carried. The caller
  // flushes before it goes idle, so a burst's last frame is never left unpresented.
  int64_t PendingPresentWaitUs() const;
  bool FlushPendingPresent();

  bool desktopAttached() const { return desktopAttached_; }
  // Also drops a carried region: its coordinates belong to the geometry being left behind.
  void DetachDesktopBuffer() {
    desktopAttached_ = false;
    carryValid_ = false;
  }

 private:
  bool PresentNow(rdpGdi* gdi, const PresentRect* carry, uint64_t* presentUs,
                  PresentUploadInfo* info);
  // Unions the frame's dirty box into the carried one.
  void CarryFrameRegion(rdpGdi* gdi);

  FramePresenter* presenter_ = nullptr;
  GfxWorkMeter* meter_ = nullptr;
  PrePresentFn prePresentFn_;
  PresentedFn presentedFn_;
  bool desktopAttached_ = false;

  bool presentPacing_ = false;
  SlotSchedule slots_;
  PresentRect carryBox_;
  bool carryValid_ = false;
  // Kept for the carried present, which happens outside the frame that produced the pixels.
  rdpGdi* lastGdi_ = nullptr;
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

  // Both run before this frame can write the desktop and the wait is idempotent per frame:
  // OnFrameBegin at RDPGFX START_FRAME, OnBeginPaint at update->BeginPaint (after decode); whichever runs first waits.
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

// `carry` (optional) is a region carried from earlier frames; it is presented together with this
// frame's own dirty region as one merged box.
bool PresentGdiFrame(rdpGdi* gdi, FramePresenter* presenter, PresentUploadInfo* info = nullptr,
                     const PresentRect* carry = nullptr);

}

#endif
