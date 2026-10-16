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

// freerdp_context_new() allocates instance->ContextSize bytes and callers cast the
// resulting rdpContext* straight back, so `context` must stay first.
struct CpuContext {
  rdpContext context;
  GfxCpuDesktop* owner;
};

CpuContext* CpuOf(rdpContext* context) {
  return reinterpret_cast<CpuContext*>(context);
}

// The default BeginPaint/EndPaint build fastpath orders over a transport that does not
// exist offline, so both are replaced.
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

// The frame's dirty bounding box, false when the frame wrote nothing.
bool FrameDirtyBox(rdpGdi* gdi, PresentRect* out) {
  if (gdi == nullptr || gdi->primary == nullptr || gdi->primary->hdc == nullptr) {
    return false;
  }
  HGDI_WND hwnd = gdi->primary->hdc->hwnd;
  if (hwnd == nullptr || hwnd->invalid == nullptr || hwnd->invalid->null) {
    return false;
  }
  return ClipPresentRect(static_cast<int>(gdi->width), static_cast<int>(gdi->height),
                         hwnd->invalid->x, hwnd->invalid->y, hwnd->invalid->w, hwnd->invalid->h,
                         out);
}

// Kill surfaces while the gdi binding is still valid: after gdi_graphics_pipeline_uninit() a
// mirrored surface cannot tell its buffer is gdi's primary and frees memory it does not own.
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

void GfxCpuDesktop::SetPresenter(FramePresenter* presenter) {
  presenter_ = presenter;
  host_.SetPresenter(presenter);
}

void GfxCpuDesktop::SetMeter(GfxWorkMeter* meter) {
  meter_ = meter;
  host_.SetMeter(meter);
}

void GdiFrameHost::OnFrameBegin() {
  // First pixel write of the frame: on the zero-copy path the GPU must be done copying the
  // previous frame out of the presenter's buffer, or this decode overwrites unread bytes.
  if (!desktopAttached_ || presenter_ == nullptr) {
    return;
  }
  const uint64_t startUs = static_cast<uint64_t>(NowUs());
  presenter_->BeginDesktopBufferWrite();
  const uint64_t waitedUs = static_cast<uint64_t>(NowUs()) - startUs;
  if (meter_ != nullptr) {
    // Blocked time: reported as `sync` and handed out of the pending `zgx+parse`
    // window, so 本机 never counts it twice.
    meter_->OnPresentSync(waitedUs);
    meter_->OnBlockedBeforeFrameWork(waitedUs);
  }
}

void GdiFrameHost::OnBeginPaint() {
  if (!desktopAttached_ || presenter_ == nullptr) {
    return;
  }
  const uint64_t startUs = static_cast<uint64_t>(NowUs());
  presenter_->BeginDesktopBufferWrite();
  if (meter_ != nullptr) {
    meter_->OnPresentSync(static_cast<uint64_t>(NowUs()) - startUs);
  }
}

void GdiFrameHost::SetPresentPacing(bool enabled) {
  presentPacing_ = enabled;
  carryValid_ = false;
}

int64_t GdiFrameHost::PendingPresentWaitUs() const {
  return carryValid_ ? slots_.WaitUs() : -1;
}

bool GdiFrameHost::PresentNow(rdpGdi* gdi, const PresentRect* carry, uint64_t* presentUs,
                              PresentUploadInfo* info) {
  const uint64_t startUs = static_cast<uint64_t>(NowUs());
  const bool presented = PresentGdiFrame(gdi, presenter_, info, carry);
  const uint64_t spanUs = static_cast<uint64_t>(NowUs()) - startUs;
  // Only the work half goes into `present`; the display-blocked part is its own blocked
  // sub-item.
  const uint64_t waitUs = presenter_->TakePresentWaitUs();
  const uint64_t presentWorkUs = spanUs > waitUs ? spanUs - waitUs : 0;
  if (meter_ != nullptr) {
    meter_->OnPresent(presentWorkUs);
    meter_->OnPresentWait(waitUs);
  }
  if (presentUs != nullptr) {
    *presentUs = spanUs;
  }
  return presented;
}

bool GdiFrameHost::OnEndPaint(rdpGdi* gdi, uint64_t* presentUs, PresentUploadInfo* info) {
  if (presentUs != nullptr) {
    *presentUs = 0;
  }
  if (gdi == nullptr || presenter_ == nullptr) {
    return false;
  }
  if (prePresentFn_ && !prePresentFn_(gdi)) {
    return false;
  }
  lastGdi_ = gdi;
  if (!desktopAttached_) {
    desktopAttached_ = AttachPresenterDesktopBuffer(gdi, presenter_);
  }
  if (presentPacing_ && !slots_.Take()) {
    // The display is still holding the previous frame: this one's pixels join the carried region.
    CarryFrameRegion(gdi);
    return false;
  }
  const PresentRect* carry = carryValid_ ? &carryBox_ : nullptr;
  const bool presented = PresentNow(gdi, carry, presentUs, info);
  carryValid_ = false;
  if (presentedFn_) {
    presentedFn_(presented, presentUs != nullptr ? *presentUs : 0,
                 info != nullptr ? *info : PresentUploadInfo{});
  }
  return presented;
}

bool GdiFrameHost::FlushPendingPresent() {
  if (!carryValid_ || lastGdi_ == nullptr || presenter_ == nullptr) {
    carryValid_ = false;
    return false;
  }
  const PresentRect carry = carryBox_;
  // Cleared first: a present that fails must not leave the region wedged in the carry.
  carryValid_ = false;
  const bool presented = PresentNow(lastGdi_, &carry, nullptr, nullptr);
  if (presentedFn_) {
    presentedFn_(presented, 0, PresentUploadInfo{});
  }
  return presented;
}

void GdiFrameHost::CarryFrameRegion(rdpGdi* gdi) {
  PresentRect box;
  if (!FrameDirtyBox(gdi, &box)) {
    return;
  }
  if (!carryValid_) {
    carryBox_ = box;
    carryValid_ = true;
    return;
  }
  const int right = carryBox_.x + carryBox_.width;
  const int bottom = carryBox_.y + carryBox_.height;
  const int boxRight = box.x + box.width;
  const int boxBottom = box.y + box.height;
  const int x = carryBox_.x < box.x ? carryBox_.x : box.x;
  const int y = carryBox_.y < box.y ? carryBox_.y : box.y;
  const int unionRight = right > boxRight ? right : boxRight;
  const int unionBottom = bottom > boxBottom ? bottom : boxBottom;
  carryBox_.x = x;
  carryBox_.y = y;
  carryBox_.width = unionRight - x;
  carryBox_.height = unionBottom - y;
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
  host_.SetPresenter(presenter_);
  host_.SetMeter(meter_);

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

  // gdi_ResetGraphics calls freerdp_client_codecs_reset(context->codecs, ...), which the
  // connection layer would normally have created; build the equivalent here.
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
  if (host_.desktopAttached() && presenter_ != nullptr) {
    presenter_->ReleaseDesktopBuffer();
  }
  host_.DetachDesktopBuffer();
  host_.SetPrePresentFn(nullptr);
  host_.SetPresentedFn(nullptr);
  width_ = 0;
  height_ = 0;
}

void GfxCpuDesktop::OnFrameBegin() {
  host_.OnFrameBegin();
}

void GfxCpuDesktop::OnBeginPaint() {
  host_.OnBeginPaint();
}

void GfxCpuDesktop::OnEndPaint() {
  // gdi_resize_ex() calls update_end_paint() while the primary buffer is rebuilt, which
  // is not a decoded frame and must not present.
  if (resizing_) {
    return;
  }
  rdpGdi* desktop = gdi();
  if (desktop == nullptr) {
    return;
  }
  uint64_t presentUs = 0;
  PresentUploadInfo info;
  (void)host_.OnEndPaint(desktop, &presentUs, &info);
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
  host_.DetachDesktopBuffer();
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

// Past this the merged box is used instead, so neither the command list nor the
// copy-region array can grow with the stream.
constexpr int kMaxPresentRects = 256;

constexpr int kGdiBytesPerPixel = 4;

// Swaps gdi's primary buffer *without* gdi_resize_ex: that calls update_end_paint() again
// from inside a frame and unbalances update->mux.
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

}  // namespace

bool InitGdiWithPresenter(freerdp* instance, FramePresenter* presenter, int width, int height) {
  int stride = 0;
  uint8_t* buffer =
      presenter != nullptr ? presenter->AcquireDesktopBuffer(width, height, &stride) : nullptr;
  if (buffer != nullptr && stride > 0) {
    // gdi does not initialise a caller-provided buffer; the presenter filled it.
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
    // (memblt / srcalpha / cache restore).
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

bool PresentGdiFrame(rdpGdi* gdi, FramePresenter* presenter, PresentUploadInfo* info,
                     const PresentRect* carry) {
  if (gdi == nullptr || presenter == nullptr || gdi->primary == nullptr ||
      gdi->primary_buffer == nullptr) {
    return false;
  }
  HGDI_WND hwnd = gdi->primary->hdc->hwnd;
  if (hwnd == nullptr || hwnd->invalid == nullptr) {
    return false;
  }
  const int desktopWidth = static_cast<int>(gdi->width);
  const int desktopHeight = static_cast<int>(gdi->height);
  if (desktopWidth <= 0 || desktopHeight <= 0) {
    return false;
  }

  PresentRect box;
  bool haveBox = FrameDirtyBox(gdi, &box);
  if (carry != nullptr) {
    // A carried region is a union of boxes: it can only be presented as one box, and it makes the
    // frame's own region part of that union.
    if (haveBox) {
      const int right = box.x + box.width > carry->x + carry->width ? box.x + box.width
                                                                    : carry->x + carry->width;
      const int bottom = box.y + box.height > carry->y + carry->height
                             ? box.y + box.height
                             : carry->y + carry->height;
      box.x = box.x < carry->x ? box.x : carry->x;
      box.y = box.y < carry->y ? box.y : carry->y;
      box.width = right - box.x;
      box.height = bottom - box.y;
    } else {
      box = *carry;
      haveBox = true;
    }
  }
  // The region is consumed either way: FreeRDP set it up for exactly this frame.
  hwnd->invalid->null = TRUE;
  if (!haveBox) {
    return false;
  }

  const int64_t boxArea = static_cast<int64_t>(box.width) * box.height;
  PresentRect rects[kMaxPresentRects];

  // Zero-copy: a rect costs a copy region plus a cache flush, so the merged box is used
  // unconditionally. Staging: cost is the bytes, so the rect list is used while bounded.
  const bool zeroCopy = presenter->usesDesktopBuffer() || carry != nullptr;
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
  const bool useRects = !zeroCopy && rectCount >= 2 && !tooManyRects;

  if (info != nullptr) {
    info->boxBytes = boxArea * 4;
    info->uploadedBytes = (useRects ? rectArea : boxArea) * 4;
    info->rectCount = useRects ? rectCount : 1;
    info->totalRects = static_cast<int>(hwnd->ninvalid);
    info->usedRects = useRects;
    info->rectListTruncated = tooManyRects;
  }

  if (!useRects) {
    rects[0] = box;
    return presenter->PresentBgra(gdi->primary_buffer, static_cast<int>(gdi->stride), desktopWidth,
                                 desktopHeight, rects, 1);
  }
  return presenter->PresentBgra(gdi->primary_buffer, static_cast<int>(gdi->stride), desktopWidth,
                               desktopHeight, rects, rectCount);
}

}  // namespace hmrdp
