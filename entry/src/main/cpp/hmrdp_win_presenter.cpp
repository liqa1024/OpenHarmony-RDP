/*
 * HmRdp - CPU frame presenter on the native window buffer queue
 * (see hmrdp_win_presenter.h).
 */
#include "hmrdp_win_presenter.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>

#include <native_buffer/buffer_common.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

constexpr int kMaxErrorLogs = 8;

// Zero-filled black with an opaque alpha. Alpha is the last byte in both BGRA and
// RGBA, so this is correct whatever the window buffer's channel order is.
void FillBlack(uint8_t* dst, size_t bytes) {
  std::memset(dst, 0x00, bytes);
  for (size_t i = 3; i < bytes; i += 4) {
    dst[i] = 0xFF;
  }
}

}  // namespace

WinPresenter::~WinPresenter() {
  Reset();
}

void WinPresenter::ResetLocked() {
  window_ = nullptr;
  surfaceWidth_ = 0;
  surfaceHeight_ = 0;
  geometryDirty_ = true;
  desktop_.clear();
  desktop_.shrink_to_fit();
  desktopWidth_ = 0;
  desktopHeight_ = 0;
  fullUpload_ = true;
  formatLogged_ = false;
}

void WinPresenter::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  ResetLocked();
  errorLogs_ = 0;
}

void WinPresenter::SetSurface(void* nativeWindow, int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (window_ != nativeWindow) {
    // The buffers belong to the previous window.
    ResetLocked();
  }
  window_ = nativeWindow;
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  geometryDirty_ = true;
  errorLogs_ = 0;
}

void WinPresenter::ResizeSurface(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  geometryDirty_ = true;
}

void WinPresenter::DestroySurface() {
  std::lock_guard<std::mutex> lock(mutex_);
  ResetLocked();
}

bool WinPresenter::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return window_ != nullptr && surfaceWidth_ > 0 && surfaceHeight_ > 0;
}

void WinPresenter::Prepare() {
  std::lock_guard<std::mutex> lock(mutex_);
  PresentSolidBlackLocked();
}

bool WinPresenter::PresentSolidBlackLocked() {
  return WithMappedBufferLocked(
      [](uint8_t* pixels, int stride, int width, int height) {
        for (int row = 0; row < height; ++row) {
          FillBlack(pixels + static_cast<size_t>(row) * stride, static_cast<size_t>(width) * 4u);
        }
        return true;
      });
}

bool WinPresenter::WithMappedBufferLocked(
    const std::function<bool(uint8_t*, int, int, int)>& write) {
  if (window_ == nullptr || surfaceWidth_ <= 0 || surfaceHeight_ <= 0) {
    return false;
  }
  auto fail = [this](const char* what, int32_t rc) {
    if (errorLogs_ < kMaxErrorLogs) {
      HMRDP_LOGW("win present: %{public}s failed (rc=%{public}d)", what, rc);
      errorLogs_++;
    }
    return false;
  };

  if (geometryDirty_) {
    // The buffer geometry must be set before the first request (and after every
    // surface resize); without it the surface has no buffers to hand out.
    const int32_t rc = OH_NativeWindow_NativeWindowHandleOpt(
        static_cast<OHNativeWindow*>(window_), SET_BUFFER_GEOMETRY, surfaceWidth_, surfaceHeight_);
    if (rc != 0) {
      return fail("SET_BUFFER_GEOMETRY", rc);
    }
    geometryDirty_ = false;
  }

  OHNativeWindowBuffer* buffer = nullptr;
  int fenceFd = -1;
  auto* win = static_cast<OHNativeWindow*>(window_);
  int32_t rc = OH_NativeWindow_NativeWindowRequestBuffer(win, &buffer, &fenceFd);
  if (rc != 0 || buffer == nullptr) {
    if (fenceFd >= 0) {
      close(fenceFd);
    }
    return fail("RequestBuffer", rc);
  }

  // The window buffer stores the display order (usually RGBA); FreeRDP hands us
  // BGRA, so the order has to be read from the buffer itself.
  BufferHandle* handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
  if (handle == nullptr) {
    if (fenceFd >= 0) {
      close(fenceFd);
    }
    OH_NativeWindow_NativeWindowAbortBuffer(win, buffer);
    return fail("GetBufferHandle", -1);
  }
  if (!formatLogged_) {
    formatLogged_ = true;
    swapRb_ = (handle->format != NATIVEBUFFER_PIXEL_FMT_BGRA_8888) &&
              (handle->format != NATIVEBUFFER_PIXEL_FMT_BGRX_8888);
    HMRDP_LOGI("win present: buffer %{public}dx%{public}d stride=%{public}d format=0x%{public}x swapRb=%d",
               handle->width, handle->height, handle->stride, handle->format, swapRb_ ? 1 : 0);
  }

  OH_NativeBuffer* nativeBuffer = nullptr;
  rc = OH_NativeBuffer_FromNativeWindowBuffer(buffer, &nativeBuffer);
  void* pixels = nullptr;
  if (rc == 0 && nativeBuffer != nullptr) {
    // The release fence from the previous consumer must be waited before writing
    // (the buffer may still be in use); -1 means there is nothing to wait for.
    if (fenceFd >= 0) {
      rc = OH_NativeBuffer_MapWaitFence(nativeBuffer, fenceFd, &pixels);
      close(fenceFd);
      fenceFd = -1;
    } else {
      rc = OH_NativeBuffer_Map(nativeBuffer, &pixels);
    }
  }
  if (rc != 0 || pixels == nullptr) {
    if (fenceFd >= 0) {
      close(fenceFd);
    }
    OH_NativeWindow_NativeWindowAbortBuffer(win, buffer);
    return fail("Map", rc);
  }

  // The compositor only needs to look at what was written; the whole picture is
  // rewritten every frame (the buffers are recycled), so the region is the full
  // surface.
  const int stride = handle->stride;
  const int width = handle->width;
  const int height = handle->height;
  const bool ok = write(static_cast<uint8_t*>(pixels), stride, width, height);
  OH_NativeBuffer_Unmap(nativeBuffer);

  if (!ok) {
    OH_NativeWindow_NativeWindowAbortBuffer(win, buffer);
    return false;
  }

  // The whole picture is rewritten every frame (the buffers are recycled), so the
  // default region - rects=nullptr / rectNumber=0 means "the whole buffer" - is
  // exactly right; no rect list is built.
  Region region{};
  // -1: the pixels are CPU-written shared memory, so there is no release fence to
  // hand over.
  rc = OH_NativeWindow_NativeWindowFlushBuffer(win, buffer, -1, region);
  if (rc != 0) {
    return fail("FlushBuffer", rc);
  }
  return true;
}

bool WinPresenter::AccumulateLocked(const uint8_t* data, int srcStride, int desktopWidth,
                                    int desktopHeight, int x, int y, int width, int height) {
  if (data == nullptr || desktopWidth <= 0 || desktopHeight <= 0) {
    return false;
  }
  const size_t rowBytes = static_cast<size_t>(desktopWidth) * 4u;
  if (desktop_.size() != rowBytes * static_cast<size_t>(desktopHeight) ||
      desktopWidth_ != desktopWidth || desktopHeight_ != desktopHeight) {
    desktop_.assign(rowBytes * static_cast<size_t>(desktopHeight), 0xFF);
    desktopWidth_ = desktopWidth;
    desktopHeight_ = desktopHeight;
    fullUpload_ = true;
  }
  if (fullUpload_) {
    // The caller's frame covers the whole desktop.
    x = 0;
    y = 0;
    width = desktopWidth;
    height = desktopHeight;
    fullUpload_ = false;
  }
  if (x < 0) {
    width += x;
    x = 0;
  }
  if (y < 0) {
    height += y;
    y = 0;
  }
  if (x + width > desktopWidth) {
    width = desktopWidth - x;
  }
  if (y + height > desktopHeight) {
    height = desktopHeight - y;
  }
  if (width <= 0 || height <= 0) {
    return false;
  }
  for (int row = 0; row < height; ++row) {
    const uint8_t* src = data + static_cast<size_t>(y + row) * static_cast<size_t>(srcStride) +
                         static_cast<size_t>(x) * 4u;
    uint8_t* dst = desktop_.data() + static_cast<size_t>(y + row) * rowBytes +
                   static_cast<size_t>(x) * 4u;
    std::memcpy(dst, src, static_cast<size_t>(width) * 4u);
  }
  return true;
}

void WinPresenter::CopyDesktopLocked(uint8_t* dst, int dstStride, int dstWidth, int dstHeight) {
  if (desktopWidth_ <= 0 || desktopHeight_ <= 0 || desktop_.empty()) {
    return;
  }
  // 1:1, centred; cropped when the desktop is larger than the surface (the
  // picture is never scaled here - the CPU path must stay cheap).
  const int offX = (dstWidth - desktopWidth_) / 2;
  const int offY = (dstHeight - desktopHeight_) / 2;
  const int srcX = std::max(0, -offX);
  const int srcY = std::max(0, -offY);
  const int dstX = std::max(0, offX);
  const int dstY = std::max(0, offY);
  const int copyW = std::min(desktopWidth_ - srcX, dstWidth - dstX);
  const int copyH = std::min(desktopHeight_ - srcY, dstHeight - dstY);
  if (copyW <= 0 || copyH <= 0) {
    return;
  }

  // The letterbox bars are part of the picture we own, so they are repainted
  // every frame (the recycled buffer may hold anything).
  if (dstX > 0) {
    for (int row = 0; row < dstHeight; ++row) {
      uint8_t* line = dst + static_cast<size_t>(row) * dstStride;
      FillBlack(line, static_cast<size_t>(dstX) * 4u);
      FillBlack(line + static_cast<size_t>(dstX + copyW) * 4u,
                static_cast<size_t>(dstWidth - dstX - copyW) * 4u);
    }
  }
  if (dstY > 0) {
    FillBlack(dst, static_cast<size_t>(dstY) * dstStride);
  }
  if (dstY + copyH < dstHeight) {
    FillBlack(dst + static_cast<size_t>(dstY + copyH) * dstStride,
              static_cast<size_t>(dstHeight - dstY - copyH) * dstStride);
  }

  const size_t rowBytes = static_cast<size_t>(copyW) * 4u;
  for (int row = 0; row < copyH; ++row) {
    const uint8_t* src = desktop_.data() +
                         static_cast<size_t>(srcY + row) * static_cast<size_t>(desktopWidth_) * 4u +
                         static_cast<size_t>(srcX) * 4u;
    uint8_t* out = dst + static_cast<size_t>(dstY + row) * dstStride +
                   static_cast<size_t>(dstX) * 4u;
    if (!swapRb_) {
      std::memcpy(out, src, rowBytes);
      continue;
    }
    for (int col = 0; col < copyW; ++col) {
      out[col * 4 + 0] = src[col * 4 + 2];
      out[col * 4 + 1] = src[col * 4 + 1];
      out[col * 4 + 2] = src[col * 4 + 0];
      out[col * 4 + 3] = src[col * 4 + 3];
    }
  }
}

bool WinPresenter::PresentBgra(const uint8_t* data, int srcStride, int desktopWidth,
                               int desktopHeight, int x, int y, int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!AccumulateLocked(data, srcStride, desktopWidth, desktopHeight, x, y, width, height)) {
    return false;
  }
  return WithMappedBufferLocked(
      [this](uint8_t* pixels, int stride, int w, int h) {
        CopyDesktopLocked(pixels, stride, w, h);
        return true;
      });
}

}  // namespace hmrdp
