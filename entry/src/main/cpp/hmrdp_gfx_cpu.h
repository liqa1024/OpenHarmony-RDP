/*
 * HmRdp - offline FreeRDP CPU (gdi) desktop (doc_agent/gfx-engine.md §6).
 *
 * The A/B reference route for the GPU desktop engine: instead of re-implementing
 * the image decoders, it builds a session-less FreeRDP context with gdi + the
 * stock rdpgfx gdi pipeline (gdi_graphics_pipeline_init), so the replayed
 * capture is decoded through the very same CPU code a live session uses
 * (clear/progressive/planar/solid fill/cache -> gdi primary buffer). The replay
 * only has to feed it the raw chunks and present the primary buffer.
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

// The free-running CPU desktop. One instance per replay run; Init() builds the
// context, Shutdown() tears it down.
class GfxCpuDesktop {
 public:
  // Invoked from gdi's EndPaint hook once a frame has been composed (i.e. the
  // invalid rectangle in the primary buffer is final), mirroring a live session.
  using FrameFn = std::function<void()>;

  GfxCpuDesktop() = default;
  ~GfxCpuDesktop();
  GfxCpuDesktop(const GfxCpuDesktop&) = delete;
  GfxCpuDesktop& operator=(const GfxCpuDesktop&) = delete;

  // Builds the FreeRDP context + gdi + RDPGFX gdi pipeline. The capture's
  // ResetGraphics PDU may resize the desktop later (update->DesktopResize).
  bool Init(int width, int height, std::string* error);
  void Shutdown();

  void SetFrameFn(FrameFn fn) { frameFn_ = std::move(fn); }
  // Optional presenter that gdi can compose into (see
  // AttachPresenterDesktopBuffer). Set before Init().
  void SetPresenter(FramePresenter* presenter) { presenter_ = presenter; }

  // RDPGFX context the raw capture chunks are fed into (GfxReplayPump).
  RdpgfxClientContext* gfx() const { return gfx_; }
  rdpGdi* gdi() const;

  int width() const { return width_; }
  int height() const { return height_; }

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

  // Microseconds spent in BeginPaint waiting for the GPU to release the desktop
  // buffer, accumulated since the last call. The caller reports it as the meter's
  // `sync` phase (see hmrdp_gfx_work.h); it is blocked time, not composition.
  uint64_t TakePresentSyncUs();

 private:
  bool Resize(int width, int height);

  freerdp* instance_ = nullptr;
  RdpgfxClientContext* gfx_ = nullptr;
  FramePresenter* presenter_ = nullptr;
  bool desktopAttached_ = false;
  uint64_t presentSyncUs_ = 0;
  FrameFn frameFn_;
  int width_ = 0;
  int height_ = 0;
  bool resizing_ = false;
};

// Brings gdi up on `instance` composing straight into `presenter`'s own desktop
// buffer when the backend offers one (FramePresenter::AcquireDesktopBuffer,
// doc_agent/cpu-accel-plan.md §4). Falls back to a gdi-owned buffer when it does
// not, which is also what happens before the presenter's device exists - the
// per-frame AttachPresenterDesktopBuffer() then moves gdi over later.
bool InitGdiWithPresenter(freerdp* instance, FramePresenter* presenter, int width, int height);

// Moves gdi's primary buffer onto the presenter's desktop buffer once the backend
// can provide one, so the pixels gdi composes are the ones the presenter uploads
// and the present needs no copy at all. Call it with the frame already composed
// (gdi's EndPaint) and before it is presented: the desktop content is carried
// over, because gdi's primitives read the destination buffer. A cheap no-op while
// the presenter has no buffer yet (retry next frame) and after the first success
// (the caller normally remembers, see GfxCpuDesktop::desktopAttached_).
bool AttachPresenterDesktopBuffer(rdpGdi* gdi, FramePresenter* presenter);

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

// Reads gdi's invalid region out of the primary buffer and hands it to the frame
// presenter (Vulkan by default, GLES fallback) as its individual dirty rects -
// see the implementation. Shared by the live gdi session and the offline CPU
// replay route so the two present exactly the same way. Returns true when a frame
// was actually drawn (a null/empty invalid region is a no-op).
bool PresentGdiFrame(rdpGdi* gdi, FramePresenter* presenter,
                     PresentUploadInfo* info = nullptr);

}  // namespace hmrdp

#endif  // HMRDP_GFX_CPU_H
