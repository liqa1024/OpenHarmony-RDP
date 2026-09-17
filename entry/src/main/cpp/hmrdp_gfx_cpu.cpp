/*
 * HmRdp - offline FreeRDP CPU (gdi) desktop (see hmrdp_gfx_cpu.h).
 */
#include "hmrdp_gfx_cpu.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include <freerdp/codec/color.h>
#include <freerdp/codecs.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/settings.h>

#include "hmrdp_gfx_driver.h"
#include "hmrdp_gfx_work.h"
#include "hmrdp_log.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The offline context carries a back-pointer to its desktop (same trick as the
// live HmrdpContext), so the gdi update hooks can find the owner. `context` must
// stay first: freerdp_context_new() allocates instance->ContextSize bytes and
// everything else casts the resulting rdpContext* straight back.
struct CpuContext {
  rdpContext context;
  GfxCpuDesktop* owner;
};

CpuContext* CpuOf(rdpContext* context) {
  return reinterpret_cast<CpuContext*>(context);
}

// The default BeginPaint/EndPaint registered by update_register_client_callbacks
// build and send fastpath orders over a transport that does not exist offline,
// so both are replaced (the live session replaces them too).
BOOL CpuBeginPaint(rdpContext* context) {
  if (context != nullptr) {
    CpuContext* ctx = CpuOf(context);
    if (ctx->owner != nullptr) {
      ctx->owner->OnBeginPaint();
    }
  }
  return TRUE;
}

BOOL CpuEndPaint(rdpContext* context) {
  if (context != nullptr) {
    CpuContext* ctx = CpuOf(context);
    if (ctx->owner != nullptr) {
      ctx->owner->OnEndPaint();
    }
  }
  return TRUE;
}

BOOL CpuDesktopResize(rdpContext* context) {
  if (context != nullptr) {
    CpuContext* ctx = CpuOf(context);
    if (ctx->owner != nullptr) {
      ctx->owner->OnDesktopResize();
    }
  }
  return TRUE;
}

// Kills every surface through the RDPGFX interface while the gdi binding is still
// valid. The context's own teardown (rdpgfx_client_context_free -> free_surfaces)
// walks them through DeleteSurface too, but by then
// gdi_graphics_pipeline_uninit() has already cleared context->custom, and
// gdi_DeleteSurface reads gdi from there. Without it a desktop-mirror surface
// cannot tell that its buffer is gdi's primary (the sharing test is
// `surface->data == gdi->primary_buffer`, doc_agent/present-pipeline.md §4.5) and would
// free memory it does not own - on the zero-copy path that is the presenter's
// host-visible buffer, i.e. an invalid free.
void DestroyGfxSurfaces(RdpgfxClientContext* context) {
  if (context == nullptr || context->GetSurfaceIds == nullptr ||
      context->DeleteSurface == nullptr) {
    return;
  }
  UINT16 count = 0;
  UINT16* ids = nullptr;
  context->GetSurfaceIds(context, &ids, &count);
  if (ids == nullptr) {
    return;
  }
  for (UINT16 index = 0; index < count; ++index) {
    RDPGFX_DELETE_SURFACE_PDU pdu = {0};
    pdu.surfaceId = ids[index];
    context->DeleteSurface(context, &pdu);
  }
  free(ids);
}

}  // namespace

GfxCpuDesktop::~GfxCpuDesktop() {
  Shutdown();
}

rdpGdi* GfxCpuDesktop::gdi() const {
  if (instance_ == nullptr || instance_->context == nullptr) {
    return nullptr;
  }
  return instance_->context->gdi;
}

bool GfxCpuDesktop::Init(int width, int height, std::string* error) {  auto fail = [this, error](const char* why) {
    if (error != nullptr) {
      *error = why;
    }
    Shutdown();
    return false;
  };
  if (width <= 0 || height <= 0) {
    return fail("invalid desktop size");
  }
  width_ = width;
  height_ = height;

  instance_ = freerdp_new();
  if (instance_ == nullptr) {
    return fail("freerdp_new failed");
  }
  instance_->ContextSize = sizeof(CpuContext);
  if (!freerdp_context_new(instance_)) {
    return fail("freerdp_context_new failed");
  }
  rdpContext* context = instance_->context;
  if (context == nullptr || context->settings == nullptr || context->update == nullptr) {
    return fail("no context/settings/update");
  }
  CpuOf(context)->owner = this;

  rdpSettings* settings = context->settings;
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, static_cast<UINT32>(width));
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, static_cast<UINT32>(height));
  freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
  freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
  freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_NATIVE_XSERVER);

  // gdi_ResetGraphics calls freerdp_client_codecs_reset(context->codecs, ...),
  // which the connection layer would normally have created (rdp_client_reset_
  // codecs in core/connection.c). Build the equivalent here.
  const UINT32 threading = freerdp_settings_get_uint32(settings, FreeRDP_ThreadingFlags);
  context->codecs = freerdp_client_codecs_new(threading);
  if (context->codecs == nullptr) {
    return fail("codecs alloc failed");
  }
  if (!freerdp_client_codecs_prepare(context->codecs, freerdp_settings_get_codecs_flags(settings),
                                     static_cast<UINT32>(width), static_cast<UINT32>(height))) {
    return fail("codecs prepare failed");
  }

  if (!InitGdiWithPresenter(instance_, presenter_, width, height)) {
    return fail("gdi_init failed");
  }

  context->update->DesktopResize = CpuDesktopResize;
  context->update->BeginPaint = CpuBeginPaint;
  context->update->EndPaint = CpuEndPaint;

  if (HmrdpGfxReplayNewWithContext == nullptr || HmrdpGfxReplayFreeWithContext == nullptr) {
    return fail("FreeRDP was not built with the HmRdp replay-context patch");
  }
  // The RDPGFX context (FreeRDP's own ZGX + PDU parsing) is bound to this
  // desktop's real rdpContext, so gfx->rdpcontext carries the actual
  // settings/update instead of a bare settings-only stand-in; the stock gdi
  // pipeline is then attached to it.
  gfx_ = HmrdpGfxReplayNewWithContext(context);
  if (gfx_ == nullptr) {
    return fail("HmrdpGfxReplayNewWithContext failed");
  }
  if (!gdi_graphics_pipeline_init(context->gdi, gfx_)) {
    return fail("gdi_graphics_pipeline_init failed");
  }
  HMRDP_LOGI("gfx cpu desktop: ready %{public}dx%{public}d", width, height);
  return true;
}

void GfxCpuDesktop::Shutdown() {
  if (gfx_ != nullptr) {
    if (instance_ != nullptr && instance_->context != nullptr &&
        instance_->context->gdi != nullptr) {
      // Must run before the uninit clears the gdi binding (DestroyGfxSurfaces).
      DestroyGfxSurfaces(gfx_);
      gdi_graphics_pipeline_uninit(instance_->context->gdi, gfx_);
    }
    if (HmrdpGfxReplayFreeWithContext != nullptr) {
      HmrdpGfxReplayFreeWithContext(gfx_);
    }
    gfx_ = nullptr;
  }
  if (instance_ != nullptr) {
    gdi_free(instance_);
    freerdp_context_free(instance_);
    freerdp_free(instance_);
    instance_ = nullptr;
  }
  // gdi is gone, so the presenter's buffer is free to drop (it owns it).
  if (desktopAttached_ && presenter_ != nullptr) {
    presenter_->ReleaseDesktopBuffer();
  }
  desktopAttached_ = false;
  frameFn_ = nullptr;
  width_ = 0;
  height_ = 0;
}

void GfxCpuDesktop::OnFrameBegin() {
  // The frame's first pixel write is about to happen: when gdi composes into the
  // presenter's desktop buffer (the zero-copy path), the GPU must be done copying
  // the previous frame out of it, or this frame's decode overwrites the bytes that
  // copy has not read yet and the upload becomes a mix of two frames. This is the
  // point that actually protects the in-place decode writes; the BeginPaint call
  // below only covers the compose, which happens later in the frame (a no-op once
  // this one has waited - the pending flag is cleared by the wait).
  if (desktopAttached_ && presenter_ != nullptr) {
    const int64_t startUs = NowUs();
    presenter_->BeginDesktopBufferWrite();
    const uint64_t waitedUs = static_cast<uint64_t>(NowUs() - startUs);
    presentSyncUs_ += waitedUs;
    // This runs before the frame's first command, so that wait sits inside the
    // window the meter charges to `zgx+parse`; hand it over so it is not counted
    // there as well (hmrdp_gfx_work.h).
    if (GfxWorkMeter* meter = ActiveWorkMeter()) {
      meter->OnBlockedBeforeFrameWork(waitedUs);
    }
  }
}

void GfxCpuDesktop::OnBeginPaint() {
  // gdi is about to compose this frame into its primary buffer (the mirrored
  // surface's pixels are already there); the same wait as OnFrameBegin, and free
  // when that one already drained the previous frame's copy. Normally free either
  // way: a whole frame's decode sits between the two (doc_agent/present-pipeline.md §4.5).
  // No accounting here: this one runs inside gdi's EndFrame, where the meter
  // already subtracts the sync wait from the compose share.
  if (desktopAttached_ && presenter_ != nullptr) {
    const int64_t startUs = NowUs();
    presenter_->BeginDesktopBufferWrite();
    presentSyncUs_ += static_cast<uint64_t>(NowUs() - startUs);
  }
}

uint64_t GfxCpuDesktop::TakePresentSyncUs() {
  const uint64_t us = presentSyncUs_;
  presentSyncUs_ = 0;
  return us;
}

void GfxCpuDesktop::OnEndPaint() {
  // gdi_resize_ex() calls update_end_paint() while the primary buffer is being
  // rebuilt; that is not a decoded frame, so it must not present.
  if (resizing_) {
    return;
  }
  // The frame is composed and not yet presented: the right moment to move gdi onto
  // the presenter's buffer (no-op once that happened).
  if (!desktopAttached_ && presenter_ != nullptr) {
    desktopAttached_ = AttachPresenterDesktopBuffer(gdi(), presenter_);
  }
  if (frameFn_) {
    frameFn_();
  }
}

void GfxCpuDesktop::OnDesktopResize() {
  if (instance_ == nullptr || instance_->context == nullptr) {
    return;
  }
  rdpSettings* settings = instance_->context->settings;
  const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  Resize(static_cast<int>(width), static_cast<int>(height));
}

bool GfxCpuDesktop::Resize(int width, int height) {
  if (width <= 0 || height <= 0 || instance_ == nullptr || instance_->context == nullptr ||
      instance_->context->gdi == nullptr) {
    return false;
  }
  if (width == width_ && height == height_) {
    return true;
  }
  const int oldWidth = width_;
  const int oldHeight = height_;
  width_ = width;
  height_ = height;
  resizing_ = true;
  const int64_t t0 = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const bool ok =
      gdi_resize(instance_->context->gdi, static_cast<UINT32>(width), static_cast<UINT32>(height));
  resizing_ = false;
  // gdi_resize() built a gdi-owned buffer again; the next EndPaint moves the new
  // desktop back onto the presenter's buffer (sized to the new geometry).
  desktopAttached_ = false;
  const int64_t dt = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count() -
                     t0;
  HMRDP_LOGI("gfx cpu desktop: resize %{public}dx%{public}d -> %{public}dx%{public}d took "
             "%{public}lld us",
             oldWidth, oldHeight, width, height, static_cast<long long>(dt));
  return ok;
}

namespace {

// Upper bound on the rects handed to one present (matches the presenter's own
// cap). gdi can accumulate more (gdi_InvalidateRegion grows its list by
// doubling); past this the merged box is used instead, so neither the command
// list nor the copy-region array can grow with the stream. Measured on the
// scrolling sample: ~15% of frames carry more than 64 rects, so the A/B endpoint
// ("always the rect list") needs headroom well above that to be meaningful.
constexpr int kMaxPresentRects = 256;

// gdi's local framebuffer is always BGRA32 here, so its row pitch is width * 4
// and the presenter's desktop buffer uses the very same packing.
constexpr int kGdiBytesPerPixel = 4;

// Moves gdi's primary buffer onto `buffer` (geometry unchanged) *without* going
// through gdi_resize_ex: that calls update_end_paint() again from inside a frame
// and unbalances update->mux. A buffer gdi allocated itself is released through
// its own free hook, so it cannot leak.
void SwapGdiPrimaryBuffer(rdpGdi* gdi, uint8_t* buffer, int stride) {
  gdiBitmap* primary = gdi->primary;
  if (primary != nullptr && primary->bitmap != nullptr) {
    HGDI_BITMAP bitmap = primary->bitmap;
    if (bitmap->data != nullptr && bitmap->data != buffer && bitmap->free != nullptr) {
      bitmap->free(bitmap->data);
    }
    bitmap->data = buffer;
    bitmap->free = nullptr;
    bitmap->scanline = static_cast<UINT32>(stride);
  }
  gdi->stride = static_cast<UINT32>(stride);
  gdi->primary_buffer = buffer;
}

// Clips one gdi rect to the desktop; returns false when nothing remains.
bool ClipPresentRect(int desktopWidth, int desktopHeight, int x, int y, int w, int h,
                     PresentRect* out) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > desktopWidth) {
    w = desktopWidth - x;
  }
  if (y + h > desktopHeight) {
    h = desktopHeight - y;
  }
  if (w <= 0 || h <= 0) {
    return false;
  }
  out->x = x;
  out->y = y;
  out->width = w;
  out->height = h;
  return true;
}

}  // namespace

bool InitGdiWithPresenter(freerdp* instance, FramePresenter* presenter, int width, int height) {
  int stride = 0;
  uint8_t* buffer =
      presenter != nullptr ? presenter->AcquireDesktopBuffer(width, height, &stride) : nullptr;
  if (buffer != nullptr && stride > 0) {
    // gdi does not initialise a caller-provided buffer (its own
    // gdi_CreateCompatibleBitmap is what fills 0xFF); the presenter filled it.
    return gdi_init_ex(instance, PIXEL_FORMAT_BGRA32, static_cast<UINT32>(stride), buffer, nullptr);
  }
  return gdi_init(instance, PIXEL_FORMAT_BGRA32);
}

bool AttachPresenterDesktopBuffer(rdpGdi* gdi, FramePresenter* presenter) {
  if (gdi == nullptr || presenter == nullptr || gdi->primary == nullptr ||
      gdi->primary_buffer == nullptr || gdi->width <= 0 || gdi->height <= 0) {
    return false;
  }
  int stride = 0;
  uint8_t* buffer = presenter->AcquireDesktopBuffer(gdi->width, gdi->height, &stride);
  if (buffer == nullptr || stride <= 0) {
    return false;
  }
  if (gdi->primary_buffer != buffer) {
    // Carry the composed desktop over: gdi's primitives read the destination
    // (memblt / srcalpha / cache restore), so swapping in the presenter's fresh
    // 0xFF buffer would change later frames' pixels.
    const size_t rowBytes = static_cast<size_t>(gdi->width) * kGdiBytesPerPixel;
    for (int y = 0; y < gdi->height; ++y) {
      std::memcpy(buffer + static_cast<size_t>(y) * static_cast<size_t>(stride),
                  gdi->primary_buffer + static_cast<size_t>(y) * static_cast<size_t>(gdi->stride),
                  rowBytes);
    }
    SwapGdiPrimaryBuffer(gdi, buffer, stride);
    HMRDP_LOGI("gfx cpu desktop: gdi composes into the presenter's desktop buffer "
               "(%{public}dx%{public}d stride=%{public}d)",
               gdi->width, gdi->height, stride);
  } else {
    gdi->stride = static_cast<UINT32>(stride);
  }
  return true;
}

bool PresentGdiFrame(rdpGdi* gdi, FramePresenter* presenter, PresentUploadInfo* info) {
  if (gdi == nullptr || presenter == nullptr || gdi->primary == nullptr ||
      gdi->primary_buffer == nullptr) {
    return false;
  }
  HGDI_WND hwnd = gdi->primary->hdc->hwnd;
  if (hwnd == nullptr || hwnd->invalid == nullptr || hwnd->invalid->null) {
    return false;
  }
  const int desktopWidth = static_cast<int>(gdi->width);
  const int desktopHeight = static_cast<int>(gdi->height);
  if (desktopWidth <= 0 || desktopHeight <= 0) {
    return false;
  }

  PresentRect box;
  const bool haveBox = ClipPresentRect(desktopWidth, desktopHeight, hwnd->invalid->x,
                                       hwnd->invalid->y, hwnd->invalid->w, hwnd->invalid->h,
                                       &box);
  // The region is consumed either way: FreeRDP set it up for exactly this frame
  // (the next begin_paint resets it).
  hwnd->invalid->null = TRUE;

  const int64_t boxArea = haveBox ? static_cast<int64_t>(box.width) * box.height : 0;
  PresentRect rects[kMaxPresentRects];

  // Which dirty shape to hand over depends on what the presenter does with it:
  //
  //  - zero-copy (`gdi` composes into the presenter's own buffer, see
  //    FramePresenter::AcquireDesktopBuffer): the pixels are already where the GPU
  //    reads them, so a rect costs a copy region plus a cache flush and the
  //    *count* is what the frame pays for. Measured: a fragmented frame reading
  //    only 2.3MB took 1.91ms with ~250 rects while a full-screen one reading
  //    20.6MB took 0.64ms. The merged box is one region and bounded by a single
  //    screen read (~0.6ms), and it measured better or equal on both samples
  //    (-17% present on the fragmented one), so it is used unconditionally.
  //  - staging (`cinvalid` rects are memcpy'd into a staging buffer first): the
  //    cost is the bytes, and the box can be several times the changed pixels
  //    (measured: 3.6x the bytes and 33% more present time on scattered content),
  //    so the rect list is used while it is bounded.
  //
  // `hwnd->invalid` is the merged bounding box of the frame's dirty regions while
  // `hwnd->cinvalid`/`ninvalid` hold the individual rects (libfreerdp/gdi/region.c
  // gdi_InvalidateRegion).
  const bool zeroCopy = presenter->usesDesktopBuffer();
  int rectCount = 0;
  int64_t rectArea = 0;
  bool tooManyRects = false;
  if (!zeroCopy && hwnd->cinvalid != nullptr) {
    for (INT32 i = 0; i < hwnd->ninvalid; ++i) {
      if (rectCount >= kMaxPresentRects) {
        tooManyRects = true;
        break;
      }
      PresentRect clipped;
      if (!ClipPresentRect(desktopWidth, desktopHeight, hwnd->cinvalid[i].x, hwnd->cinvalid[i].y,
                           hwnd->cinvalid[i].w, hwnd->cinvalid[i].h, &clipped)) {
        continue;
      }
      rectArea += static_cast<int64_t>(clipped.width) * clipped.height;
      rects[rectCount++] = clipped;
    }
  }
  // A single rect *is* the box, and past the cap the box is the only bounded
  // option (a video frame can carry ~2900 rects, which cannot become ~2900 copy
  // regions).
  const bool useRects = !zeroCopy && rectCount >= 2 && !tooManyRects && haveBox;

  if (info != nullptr) {
    info->boxBytes = boxArea * 4;
    info->uploadedBytes = (useRects ? rectArea : boxArea) * 4;
    info->rectCount = useRects ? rectCount : (haveBox ? 1 : 0);
    info->totalRects = static_cast<int>(hwnd->ninvalid);
    info->usedRects = useRects;
    info->rectListTruncated = tooManyRects;
  }

  if (!useRects) {
    if (!haveBox) {
      return false;
    }
    rects[0] = box;
    return presenter->PresentBgra(gdi->primary_buffer, static_cast<int>(gdi->stride), desktopWidth,
                                 desktopHeight, rects, 1);
  }
  return presenter->PresentBgra(gdi->primary_buffer, static_cast<int>(gdi->stride), desktopWidth,
                               desktopHeight, rects, rectCount);
}

}  // namespace hmrdp
