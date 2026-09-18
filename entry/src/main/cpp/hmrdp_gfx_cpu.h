/*
 * HmRdp - offline FreeRDP CPU (gdi) desktop (doc_agent/gfx-engine.md §6).
 *
 * The replay's decoder: instead of re-implementing the image decoders, it builds
 * a session-less FreeRDP context with gdi + the stock rdpgfx gdi pipeline
 * (gdi_graphics_pipeline_init), so the replayed capture is decoded through the
 * very same CPU code a live session uses (clear/progressive/planar/solid
 * fill/cache -> gdi primary buffer). The replay only has to feed it the raw
 * chunks and present the primary buffer.
 */
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

// What the CPU present path handed over for one frame: either the individual
// dirty rects (the staging presenter memcpy's them, so bytes are what a frame
// pays for) or their merged bounding box (zero-copy presenters pay per rect, so
// the box's single region wins - see PresentGdiFrame). `boxBytes`/`usedRects`
// report which shape was chosen and what the other one would have cost.
struct PresentUploadInfo {
  int64_t uploadedBytes = 0;       // bytes handed to the presenter
  int64_t boxBytes = 0;            // bytes the merged box would have uploaded
  int rectCount = 0;               // presenter rects (1 = the box)
  int totalRects = 0;              // rects gdi accumulated for this frame
  bool usedRects = false;          // the rect list was used
  bool rectListTruncated = false;  // more rects than the cap: box used instead
};

// The CPU (gdi) frame host: the *one* implementation of "compose into the
// presenter's buffer, wait for the previous frame's GPU read, present the dirty
// rects". The live session and the offline replay both reach it through their
// gdi update hooks and run the exact same call chain - there is no second,
// "live-compatible" branch kept behind the abstraction. The two owners differ
// only in the observers they attach to that chain:
//
//   EndPaint -> [PrePresent] -> attach -> PresentGdiFrame -> [Presented]
//
// `PrePresent` (optional) may veto one frame - the replay uses it for its run
// cap, its surface resize and the energy sample. `Presented` is where the owner
// accounts for the frame (the live session's telemetry, the replay's stats and
// golden reference). Attaching gdi to the presenter's desktop buffer is retried
// on every frame until it succeeds, so a presenter whose device appears after
// the session started is picked up automatically.
class GdiFrameHost {
 public:
  // Returns false to skip this frame entirely (nothing attached, nothing
  // presented, no accounting).
  using PrePresentFn = std::function<bool(rdpGdi*)>;
  // Runs once per frame that made it past `PrePresent`. `presented` is false when
  // the presenter had nothing to draw / could not draw; the owner still gets to
  // count it (the replay distinguishes skips from failures).
  using PresentedFn = std::function<void(bool presented, uint64_t presentUs,
                                         const PresentUploadInfo& info)>;

  void SetPresenter(FramePresenter* presenter) { presenter_ = presenter; }
  void SetPrePresentFn(PrePresentFn fn) { prePresentFn_ = std::move(fn); }
  void SetPresentedFn(PresentedFn fn) { presentedFn_ = std::move(fn); }
  // The meter the blocked-time (`sync`) figures are reported to. Explicit, not
  // the process-global active meter: a replay running next to a live session
  // measures with its own meter and must not feed the session's (hmrdp_gfx_work.h).
  void SetMeter(GfxWorkMeter* meter) { meter_ = meter; }

  // Runs at every point that is still *before* this frame may write the desktop
  // buffer (RDPGFX START_FRAME and the surface/setup commands). Idempotent per
  // frame: the presenter drains its pending wait on the first call. The blocked
  // time is reported to the work meter as the `sync` phase and handed out of the
  // pending `zgx+parse` window, so `本机` never counts it twice.
  void OnFrameBegin();
  // Same wait at gdi's BeginPaint (the compose of a non-mirrored surface happens
  // there); a no-op once OnFrameBegin drained it.
  void OnBeginPaint();

  // The composed frame is presented: runs PrePresent, attaches the desktop buffer
  // when it can, presents the dirty region, runs Presented. `*presentUs` receives
  // the present's own duration. Returns true when a frame reached the surface.
  bool OnEndPaint(rdpGdi* gdi, uint64_t* presentUs, PresentUploadInfo* info = nullptr);

  // True once gdi composes into the presenter's own buffer (zero-copy).
  bool desktopAttached() const { return desktopAttached_; }
  // The presenter's buffer is no longer usable (resize/teardown): gdi owns its
  // own buffer again and the next OnEndPaint re-acquires.
  void DetachDesktopBuffer() { desktopAttached_ = false; }

 private:
  FramePresenter* presenter_ = nullptr;
  GfxWorkMeter* meter_ = nullptr;
  PrePresentFn prePresentFn_;
  PresentedFn presentedFn_;
  bool desktopAttached_ = false;
};

// The free-running CPU desktop. One instance per replay run; Init() builds the
// context, Shutdown() tears it down.
class GfxCpuDesktop {
 public:
  GfxCpuDesktop() = default;
  ~GfxCpuDesktop();
  GfxCpuDesktop(const GfxCpuDesktop&) = delete;
  GfxCpuDesktop& operator=(const GfxCpuDesktop&) = delete;

  // Builds the FreeRDP context + gdi + RDPGFX gdi pipeline. The capture's
  // ResetGraphics PDU may resize the desktop later (update->DesktopResize).
  bool Init(int width, int height, std::string* error);
  void Shutdown();

  // Optional presenter that gdi can compose into (see
  // AttachPresenterDesktopBuffer). Set before Init().
  void SetPresenter(FramePresenter* presenter);
  // The meter the frame host reports its `sync` blocked time to (see GdiFrameHost).
  void SetMeter(GfxWorkMeter* meter);
  // The frame host's observers (see GdiFrameHost). The replay installs both; the
  // gdi EndPaint hook then runs the same chain the live session runs.
  void SetPrePresentFn(GdiFrameHost::PrePresentFn fn) { host_.SetPrePresentFn(std::move(fn)); }
  void SetPresentedFn(GdiFrameHost::PresentedFn fn) { host_.SetPresentedFn(std::move(fn)); }

  // RDPGFX context the raw capture chunks are fed into (GfxReplayPump).
  RdpgfxClientContext* gfx() const { return gfx_; }
  rdpGdi* gdi() const;

  // Internal: called by the gdi update hooks.
  //
  // OnFrameBegin runs at the RDPGFX START_FRAME, i.e. before this frame's surface
  // commands write the desktop; OnBeginPaint runs at update->BeginPaint, which
  // FreeRDP calls *after* the frame's decode (inside gdi_OutputUpdate), just
  // before the compose. The wait for the GPU to release the presenter's desktop
  // buffer belongs at *both* points and is idempotent per frame: the frame-begin
  // one protects the in-place decode writes, the BeginPaint one the compose
  // writes, and whichever runs first is the one that waits.
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

// Brings gdi up on `instance` composing straight into `presenter`'s own desktop
// buffer when the backend offers one (FramePresenter::AcquireDesktopBuffer,
// doc_agent/present-pipeline.md §4.5). Falls back to a gdi-owned buffer when it does
// not, which is also what happens before the presenter's device exists - the
// per-frame AttachPresenterDesktopBuffer() then moves gdi over later.
bool InitGdiWithPresenter(freerdp* instance, FramePresenter* presenter, int width, int height);

// Moves gdi's primary buffer onto the presenter's desktop buffer once the backend
// can provide one, so the pixels gdi composes are the ones the presenter uploads
// and the present needs no copy at all. Call it with the frame already composed
// (gdi's EndPaint) and before it is presented: the desktop content is carried
// over, because gdi's primitives read the destination buffer. A cheap no-op while
// the presenter has no buffer yet (retry next frame) and after the first success
// (see GdiFrameHost, which remembers).
bool AttachPresenterDesktopBuffer(rdpGdi* gdi, FramePresenter* presenter);

// Reads gdi's invalid region out of the primary buffer and hands it to the frame
// presenter (Vulkan by default, GLES fallback) as its individual dirty rects -
// see the implementation. Shared by the live gdi session and the offline CPU
// replay route so the two present exactly the same way. Returns true when a frame
// was actually drawn (a null/empty invalid region is a no-op).
bool PresentGdiFrame(rdpGdi* gdi, FramePresenter* presenter,
                     PresentUploadInfo* info = nullptr);

}  // namespace hmrdp

#endif  // HMRDP_GFX_CPU_H
