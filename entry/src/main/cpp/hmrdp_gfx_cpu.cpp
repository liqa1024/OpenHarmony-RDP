/*
 * HmRdp - offline FreeRDP CPU (gdi) desktop (see hmrdp_gfx_cpu.h).
 */
#include "hmrdp_gfx_cpu.h"

#include <freerdp/codec/color.h>
#include <freerdp/codecs.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/settings.h>

#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

namespace {

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
  (void)context;
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

  if (!gdi_init(instance_, PIXEL_FORMAT_BGRA32)) {
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
  frameFn_ = nullptr;
  width_ = 0;
  height_ = 0;
}

void GfxCpuDesktop::OnEndPaint() {
  // gdi_resize_ex() calls update_end_paint() while the primary buffer is being
  // rebuilt; that is not a decoded frame, so it must not present.
  if (resizing_) {
    return;
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
  width_ = width;
  height_ = height;
  resizing_ = true;
  const bool ok =
      gdi_resize(instance_->context->gdi, static_cast<UINT32>(width), static_cast<UINT32>(height));
  resizing_ = false;
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

  // `hwnd->invalid` is the merged bounding box of the frame's dirty regions,
  // while `hwnd->cinvalid`/`ninvalid` hold the individual rects (see
  // libfreerdp/gdi/region.c gdi_InvalidateRegion). Uploading the box copies the
  // pixels that changed *plus* everything between them - for a frame made of a
  // few scattered updates that can be several times the bytes that actually
  // changed - so prefer the rect list whenever it is meaningfully smaller.
  PresentRect rects[kMaxPresentRects];
  int rectCount = 0;
  int64_t rectArea = 0;
  bool tooManyRects = false;
  if (hwnd->cinvalid != nullptr) {
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

  PresentRect box;
  const bool haveBox = ClipPresentRect(desktopWidth, desktopHeight, hwnd->invalid->x,
                                       hwnd->invalid->y, hwnd->invalid->w, hwnd->invalid->h,
                                       &box);
  // The region is consumed either way: FreeRDP set it up for exactly this frame
  // (the next begin_paint resets it).
  hwnd->invalid->null = TRUE;

  // Upload the rects themselves: measured on both sample scenarios, the merged
  // box is never cheaper (scattered content: 3.6x the bytes and 33% more present
  // time) and the rect list is never worse (video: equal within noise). The box
  // survives only as the bounded fallback below.
  //   - a single rect *is* the box, so there is nothing to gain;
  //   - past the cap the box is the only bounded option (a video frame can carry
  //     ~2900 rects, which cannot become ~2900 copy regions).
  const bool useRects = rectCount >= 2 && !tooManyRects && haveBox;
  const int64_t boxArea = haveBox ? static_cast<int64_t>(box.width) * box.height : 0;

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
