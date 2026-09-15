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

  // RDPGFX context the raw capture chunks are fed into (GfxReplayPump).
  RdpgfxClientContext* gfx() const { return gfx_; }
  rdpGdi* gdi() const;

  int width() const { return width_; }
  int height() const { return height_; }

  // Internal: called by the gdi update hooks.
  void OnEndPaint();
  void OnDesktopResize();

 private:
  bool Resize(int width, int height);

  freerdp* instance_ = nullptr;
  RdpgfxClientContext* gfx_ = nullptr;
  FrameFn frameFn_;
  int width_ = 0;
  int height_ = 0;
  bool resizing_ = false;
};

// Reads gdi's invalid rectangle out of the primary buffer and hands it to the
// frame presenter (Vulkan by default, GLES fallback). Shared by the live gdi
// session and the offline CPU replay route so the two present exactly the same
// way. Returns true when a frame was actually drawn (a null/empty invalid region
// is a no-op).
bool PresentGdiFrame(rdpGdi* gdi, FramePresenter* presenter);

}  // namespace hmrdp

#endif  // HMRDP_GFX_CPU_H
