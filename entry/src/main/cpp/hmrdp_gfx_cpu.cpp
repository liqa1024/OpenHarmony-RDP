/*
 * HmRdp - offline FreeRDP CPU (gdi) desktop (see hmrdp_gfx_cpu.h).
 */
#include "hmrdp_gfx_cpu.h"

#include <freerdp/codec/color.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/settings.h>

#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_renderer.h"

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

const uint8_t* GfxCpuDesktop::SurfaceData(uint16_t surfaceId, int* width, int* height, int* stride,
                                         uint32_t* format) const {
  if (gfx_ == nullptr || gfx_->GetSurfaceData == nullptr) {
    return nullptr;
  }
  void* data = gfx_->GetSurfaceData(gfx_, surfaceId);
  if (data == nullptr) {
    return nullptr;
  }
  const gdiGfxSurface* surface = static_cast<const gdiGfxSurface*>(data);
  if (surface->data == nullptr || surface->width == 0 || surface->height == 0) {
    return nullptr;
  }
  if (width != nullptr) {
    *width = static_cast<int>(surface->width);
  }
  if (height != nullptr) {
    *height = static_cast<int>(surface->height);
  }
  if (stride != nullptr) {
    *stride = static_cast<int>(surface->scanline);
  }
  if (format != nullptr) {
    *format = surface->format;
  }
  return surface->data;
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

bool PresentGdiFrame(rdpGdi* gdi, Renderer* renderer) {
  if (gdi == nullptr || renderer == nullptr || gdi->primary == nullptr ||
      gdi->primary_buffer == nullptr) {
    return false;
  }
  HGDI_WND hwnd = gdi->primary->hdc->hwnd;
  if (hwnd == nullptr || hwnd->invalid == nullptr || hwnd->invalid->null) {
    return false;
  }
  const INT32 x = hwnd->invalid->x;
  const INT32 y = hwnd->invalid->y;
  const INT32 width = hwnd->invalid->w;
  const INT32 height = hwnd->invalid->h;
  hwnd->invalid->null = TRUE;
  if (width <= 0 || height <= 0) {
    return false;
  }
  return renderer->DrawFrame(gdi->primary_buffer, static_cast<int>(gdi->stride), x, y, width,
                             height);
}

}  // namespace hmrdp
