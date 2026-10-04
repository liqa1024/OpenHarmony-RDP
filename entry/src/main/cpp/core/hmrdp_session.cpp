/*
 * HmRdp - HarmonyOS RDP client
 * FreeRDP client session implementation.
 */
#include "hmrdp_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <freerdp/autodetect.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/codec/color.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/gfx.h>
#include <freerdp/graphics.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <freerdp/utils/signal.h>
#include <winpr/crt.h>
#include <winpr/string.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/wlog.h>

#include "hmrdp_log.h"
#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_parallel.h"

// The rdpsnd backend is replaced on OHOS (see native/patches/rdpsnd_opensles.c):
// instead of opening an OpenSL ES device it hands decoded 16-bit PCM to a sink
// registered through HmrdpSetAudioSink. The sink routes the buffer to the
// owning Session's AudioSink ring, which the ArkTS AudioRenderer pulls from. It
// returns the render latency in ms (the playback time queued after the packet),
// which rdpsnd puts into the Wave Confirm sent back to the server.
using HmrdpAudioSink = int (*)(void* context, const void* data, size_t size,
                               int sampleRate, int channels);

extern "C" void HmrdpSetAudioSink(HmrdpAudioSink sink);

// Defined by the patched rdpei client (native/scripts/patch-freerdp.ps1). The
// weak reference keeps libhmrdp linkable against stock FreeRDP, where touch
// high-rate simply stays unavailable.
extern "C" void HmrdpSetTouchFrameInterval(UINT32 intervalMs) __attribute__((weak));

// Touch pacing handed to the patched rdpei client. The interval is a floor on the
// flush period, so it is the ceiling on the touch input rate: upstream's default
// is 50Hz, and the high-rate option raises that to 125Hz. Neither is zero, because
// a zero interval forwards every input poll and the rate is then bounded by the
// network alone.
static constexpr UINT32 kTouchFrameIntervalDefaultMs = 20;
static constexpr UINT32 kTouchFrameIntervalHighRateMs = 8;

// FreeRDP hands us every raw (still ZGX-compressed) GFX channel chunk through
// this callback. It is only registered when FreeRDP carries the HmRdp GFX
// capture patch (native/scripts/patch-steps/07-gfx-capture-replay.ps1) - see
// GfxDumpRaw. The capture write happens first so its disk I/O never lands inside
// the measured phases; the frame-meter arrival stamp lives on the per-context
// paths instead (the data sink and the replay pump), because this hook carries no
// context with which to find the owning pipeline's meter.
extern "C" void HmrdpGfxRawCapture(const BYTE* data, UINT32 size) {
  hmrdp::GfxDumpRaw(data, static_cast<uint32_t>(size));
}

// Defined by the patched rdpgfx client; weak so stock FreeRDP links unchanged.
extern "C" void HmrdpSetGfxRawCapture(void (*fn)(const BYTE* data, UINT32 size))
    __attribute__((weak));

// Defined by the patched winpr (native/scripts/patch-freerdp.ps1): the pool
// workers and the drdynvc thread call the registered callback once per thread.
// Weak so a stock FreeRDP still links - the QoS marking is then simply absent.
extern "C" void HmrdpSetThreadQoSApplier(void (*fn)(void)) __attribute__((weak));

namespace hmrdp {

// HarmonyOS QoS (Kernel_Enhance_Kit, "QoS 开发指导"): a thread can tell the
// scheduler how important its work is, and a marked producer/consumer pair has
// both its wake-up and its preemption latency reduced - the same work then
// finishes in a shorter active window, which is what a mobile device wants.
// The level is set from *inside* the thread (the API only affects the caller),
// which is why FreeRDP calls HmrdpThreadQoSHook() on its pool workers and on the
// drdynvc thread that runs the frame pipeline.
//
// libqos.so is dlopen()ed like libohaudio/libvulkan: a device without it (or
// without the patched FreeRDP) simply runs unmarked.
using SetThreadQoSFn = int (*)(int level);
// QoS_Level (qos/qos.h): 0 BACKGROUND, 1 UTILITY, 2 DEFAULT, 3 USER_INITIATED,
// 4 DEADLINE_REQUEST, 5 USER_INTERACTIVE. The frame pipeline is "user initiated
// and visibly progressing", i.e. 3 - deliberately below the UI/animation level
// so the session window's own drawing is never starved by decoding.
constexpr int kFramePipelineQoS = 3;

SetThreadQoSFn ResolveQoS() {
  static SetThreadQoSFn fn = []() -> SetThreadQoSFn {
    void* lib = dlopen("libqos.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<SetThreadQoSFn>(dlsym(lib, "OH_QoS_SetThreadQoS"));
  }();
  return fn;
}

}  // namespace hmrdp

// Called once per thread on the frame pipeline (see the FreeRDP patch): the
// libwinpr pool workers and the drdynvc thread, plus our own receive thread.
extern "C" void HmrdpThreadQoSHook(void) {
  static thread_local bool applied = false;
  if (applied) {
    return;
  }
  applied = true;
  hmrdp::SetThreadQoSFn setQos = hmrdp::ResolveQoS();
  if (setQos != nullptr) {
    (void)setQos(hmrdp::kFramePipelineQoS);
  }
}

namespace hmrdp {
// The file-clipboard domain lives in its own unit (hmrdp_file_clipboard.*); pull
// its names in so the session reads as a wire-up.
using fileclip::FileSizeOfPath;
using fileclip::kFileContentsId;
using fileclip::kFileContentsName;
using fileclip::kFileGroupDescriptorId;
using fileclip::kFileGroupDescriptorName;

namespace {

// Whether remote cursor updates drive the HarmonyOS system cursor. Global (like
// the touch frame interval) because it is a user setting, and read when a
// session connects (HmrdpPostConnect).
std::atomic<bool> g_useRdpCursor{true};

uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

BOOL HmrdpWLogMessage(const wLogMessage* msg) {
  if (msg == nullptr || msg->TextString == nullptr) {
    return TRUE;
  }
  const char* prefix = msg->PrefixString != nullptr ? msg->PrefixString : "";
  if (msg->Level <= WLOG_ERROR) {
    HMRDP_LOGE("%{public}s%{public}s", prefix, msg->TextString);
  } else if (msg->Level == WLOG_WARN) {
    HMRDP_LOGW("%{public}s%{public}s", prefix, msg->TextString);
  } else {
    HMRDP_LOGI("%{public}s%{public}s", prefix, msg->TextString);
  }
  return TRUE;
}

void SetupFreeRdpLogging() {
  static bool configured = false;
  if (configured) {
    return;
  }
  configured = true;
  static wLogCallbacks callbacks = {};
  callbacks.message = HmrdpWLogMessage;
  wLog* root = WLog_GetRoot();
  if (root == nullptr) {
    return;
  }
  WLog_SetLogLevel(root, WLOG_INFO);
  WLog_SetLogAppenderType(root, WLOG_APPENDER_CALLBACK);
  wLogAppender* appender = WLog_GetLogAppender(root);
  if (appender != nullptr) {
    WLog_ConfigureAppender(appender, "callbacks", &callbacks);
  }
}

// Local clipboard content kinds; must match Session::LocalClipKind.
constexpr int kClipText = 1;
constexpr int kClipHtml = 2;
constexpr int kClipImage = 3;
constexpr int kClipRtf = 4;
constexpr int kClipFiles = 5;

// Client-side id and name for the registered "HTML Format" clipboard format.
// Registered ids start at 0xC000 (WinPR convention); with CB_USE_LONG_FORMAT_NAMES
// the server maps it by name.
constexpr UINT32 kHtmlFormatId = 0xC000;
const char kHtmlFormatName[] = "HTML Format";

// Client-side id/name for the registered "PNG" clipboard format. Windows offers
// an image as CF_DIB and, when the client advertises it, this compressed form
// (named "PNG"; the server maps it by name under CB_USE_LONG_FORMAT_NAMES).
constexpr UINT32 kPngFormatId = 0xC003;
const char kPngFormatName[] = "PNG";

// HarmonyOS custom cursors cap at 256x256 (see setCustomCursorSync). Larger RDP
// cursors (large pointer, up to 384x384) are downscaled to fit instead of being
// dropped, so the remote still drives the system cursor. The area average is
// computed on premultiplied alpha and un-premultiplied afterwards, otherwise
// transparent pixels (RGB 0) bleed dark fringes into the cursor edges.
void DownscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                   uint32_t dstW, uint32_t dstH, std::vector<uint8_t>* dst) {
  dst->assign(static_cast<size_t>(dstW) * dstH * 4, 0);
  const double scaleX = static_cast<double>(srcW) / dstW;
  const double scaleY = static_cast<double>(srcH) / dstH;
  for (uint32_t dy = 0; dy < dstH; dy++) {
    const uint32_t y0 = static_cast<uint32_t>(dy * scaleY);
    uint32_t y1 = static_cast<uint32_t>((dy + 1) * scaleY);
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    if (y1 > srcH) {
      y1 = srcH;
    }
    for (uint32_t dx = 0; dx < dstW; dx++) {
      const uint32_t x0 = static_cast<uint32_t>(dx * scaleX);
      uint32_t x1 = static_cast<uint32_t>((dx + 1) * scaleX);
      if (x1 <= x0) {
        x1 = x0 + 1;
      }
      if (x1 > srcW) {
        x1 = srcW;
      }
      uint64_t aSum = 0;
      uint64_t rSum = 0;
      uint64_t gSum = 0;
      uint64_t bSum = 0;
      uint32_t count = 0;
      for (uint32_t y = y0; y < y1; y++) {
        const uint8_t* row = &src[(static_cast<size_t>(y) * srcW + x0) * 4];
        for (uint32_t x = x0; x < x1; x++) {
          const uint32_t alpha = row[3];
          bSum += static_cast<uint64_t>(row[0]) * alpha;
          gSum += static_cast<uint64_t>(row[1]) * alpha;
          rSum += static_cast<uint64_t>(row[2]) * alpha;
          aSum += alpha;
          row += 4;
          count++;
        }
      }
      uint8_t* out = &(*dst)[(static_cast<size_t>(dy) * dstW + dx) * 4];
      if (count == 0 || aSum == 0) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
        continue;
      }
      out[0] = static_cast<uint8_t>(bSum / aSum);
      out[1] = static_cast<uint8_t>(gSum / aSum);
      out[2] = static_cast<uint8_t>(rSum / aSum);
      out[3] = static_cast<uint8_t>(aSum / count);
    }
  }
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// Sharpening handed to the cursor's upscale, in the units of a 5-tap unsharp mask.
// The result is clamped to the local neighbourhood, so it sharpens an edge without
// ringing into a halo.
constexpr float kCursorSrSharpness = 0.8f;

// Lanczos window (a = 3). Sharper than a bilinear or bicubic tap at the small
// magnifications 超分 uses (output ÷ session), which is the whole point of the
// cursor going through a real resample rather than a stretch.
double Lanczos3Weight(double x) {
  const double ax = x < 0.0 ? -x : x;
  if (ax < 1e-9) {
    return 1.0;
  }
  if (ax >= 3.0) {
    return 0.0;
  }
  const double px = kPi * ax;
  return (std::sin(px) / px) * (std::sin(px / 3.0) / (px / 3.0));
}

// One separable Lanczos pass over RGBA float samples that are already
// premultiplied. `horizontal` picks which axis is resampled; the other keeps its
// extent. Source samples outside the bitmap mirror the edge, so the outermost
// pixels are not pulled towards black.
void ResampleLanczos3Pass(const std::vector<float>& src, int srcW, int srcH, int outW, int outH,
                          bool horizontal, std::vector<float>* dst) {
  dst->assign(static_cast<size_t>(outW) * outH * 4, 0.0f);
  const int inLen = horizontal ? srcW : srcH;
  const int outLen = horizontal ? outW : outH;
  const int otherLen = horizontal ? outH : outW;
  const double scale = static_cast<double>(inLen) / outLen;
  // Widening the kernel while downscaling is what keeps a shrink from aliasing;
  // for a magnification it stays at the base support.
  const double filterScale = scale > 1.0 ? scale : 1.0;
  for (int line = 0; line < otherLen; ++line) {
    for (int i = 0; i < outLen; ++i) {
      const double center = (static_cast<double>(i) + 0.5) * scale - 0.5;
      const int first = static_cast<int>(std::ceil(center - 3.0 * filterScale));
      const int last = static_cast<int>(std::floor(center + 3.0 * filterScale));
      double sum[4] = {0.0, 0.0, 0.0, 0.0};
      double weightSum = 0.0;
      for (int tap = first; tap <= last; ++tap) {
        const double weight = Lanczos3Weight((center - tap) / filterScale);
        if (weight == 0.0) {
          continue;
        }
        int clamped = tap < 0 ? 0 : (tap >= inLen ? inLen - 1 : tap);
        const size_t index = horizontal ? (static_cast<size_t>(line) * srcW + clamped) * 4
                                        : (static_cast<size_t>(clamped) * srcW + line) * 4;
        for (int c = 0; c < 4; ++c) {
          sum[c] += weight * src[index + c];
        }
        weightSum += weight;
      }
      if (weightSum == 0.0) {
        continue;
      }
      const size_t outIndex = horizontal ? (static_cast<size_t>(line) * outW + i) * 4
                                         : (static_cast<size_t>(i) * outW + line) * 4;
      for (int c = 0; c < 4; ++c) {
        double value = sum[c] / weightSum;
        value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
        (*dst)[outIndex + c] = static_cast<float>(value);
      }
    }
  }
}

// Contrast-adaptive sharpening on the premultiplied colour: a 5-tap unsharp mask
// clamped to the local min/max, so an edge is accentuated but can never overshoot
// into a halo. The alpha channel is left alone - sharpening it would only ruffle
// the cursor's outline.
void SharpenPremul(std::vector<float>* image, int width, int height, float strength) {
  const std::vector<float> src = *image;
  auto at = [&](int x, int y, int c) -> float {
    const int cx = x < 0 ? 0 : (x >= width ? width - 1 : x);
    const int cy = y < 0 ? 0 : (y >= height ? height - 1 : y);
    return src[(static_cast<size_t>(cy) * width + cx) * 4 + c];
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        const float center = at(x, y, c);
        const float north = at(x, y - 1, c);
        const float south = at(x, y + 1, c);
        const float west = at(x - 1, y, c);
        const float east = at(x + 1, y, c);
        const float low =
            std::min(std::min(center, north), std::min(std::min(south, west), east));
        const float high =
            std::max(std::max(center, north), std::max(std::max(south, west), east));
        if (high - low <= 1e-6f) {
          continue;
        }
        const float laplacian = 4.0f * center - (north + south + west + east);
        float value = center + strength * 0.25f * laplacian;
        value = value < low ? low : (value > high ? high : value);
        (*image)[(static_cast<size_t>(y) * width + x) * 4 + c] = value;
      }
    }
  }
}

}  // namespace

// Cursor 超分: a genuine resample of the pointer bitmap, not a stretch. Everything
// happens on the *premultiplied* bitmap - the source's transparent surround is
// RGB 0, and any linear filter run on straight-alpha colour mixes that black into
// the shape's edges (the classic dark fringe); premultiplied, the blend stays
// physically correct, and un-premultiplying at the end restores the straight alpha
// the system cursor expects. Lanczos3 recovers the sharpness the reduced session
// scale cost, and the contrast-adaptive sharpen puts the edge back.
void UpscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                 uint32_t dstW, uint32_t dstH, std::vector<uint8_t>* dst) {
  const size_t srcPixels = static_cast<size_t>(srcW) * srcH;
  std::vector<float> premul(srcPixels * 4, 0.0f);
  for (size_t i = 0; i < srcPixels; ++i) {
    const double alpha = src[i * 4 + 3] / 255.0;
    premul[i * 4 + 0] = static_cast<float>(src[i * 4 + 0] * alpha / 255.0);
    premul[i * 4 + 1] = static_cast<float>(src[i * 4 + 1] * alpha / 255.0);
    premul[i * 4 + 2] = static_cast<float>(src[i * 4 + 2] * alpha / 255.0);
    premul[i * 4 + 3] = static_cast<float>(alpha);
  }
  std::vector<float> horizontal;
  ResampleLanczos3Pass(premul, static_cast<int>(srcW), static_cast<int>(srcH),
                       static_cast<int>(dstW), static_cast<int>(srcH), true, &horizontal);
  std::vector<float> upscaled;
  ResampleLanczos3Pass(horizontal, static_cast<int>(dstW), static_cast<int>(srcH),
                       static_cast<int>(dstW), static_cast<int>(dstH), false, &upscaled);
  SharpenPremul(&upscaled, static_cast<int>(dstW), static_cast<int>(dstH), kCursorSrSharpness);

  const size_t dstPixels = static_cast<size_t>(dstW) * dstH;
  dst->assign(dstPixels * 4, 0);
  for (size_t i = 0; i < dstPixels; ++i) {
    const double alpha = upscaled[i * 4 + 3];
    const double alphaByte = alpha * 255.0 + 0.5;
    (*dst)[i * 4 + 3] = static_cast<uint8_t>(alphaByte > 255.0 ? 255.0 : alphaByte);
    if (alpha <= 0.0) {
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      double value = upscaled[i * 4 + c] / alpha;
      value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
      (*dst)[i * 4 + c] = static_cast<uint8_t>(value * 255.0 + 0.5);
    }
  }
}

constexpr uint32_t kMaxCursorSide = 256;
// Hard cap on the source cursor size; anything beyond this is treated as
// corrupt and dropped rather than allocated.
constexpr uint32_t kMaxCursorSourceSide = 1024;

typedef struct {
  rdpClientContext common;
  Session* session;
} HmrdpContext;

// Server-reported network characteristics (MS-RDPBCGR Network Characteristics
// Result PDU). FreeRDP's client parses the PDU but only forwards it through
// this callback, so libhmrdp registers its own handler to capture the RTT.
BOOL HmrdpNetworkCharacteristicsResult(rdpAutoDetect* autodetect, RDP_TRANSPORT_TYPE, UINT16,
                                       const rdpNetworkCharacteristicsResult* result) {
  if (autodetect == nullptr || autodetect->context == nullptr || result == nullptr) {
    return TRUE;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(autodetect->context);
  if (ctx->session != nullptr) {
    ctx->session->OnNetworkCharacteristics(result->baseRTT, result->averageRTT,
                                           result->bandwidth);
  }
  return TRUE;
}

BOOL HmrdpBeginPaint(rdpContext* context) {
  if (context == nullptr || context->gdi == nullptr || context->gdi->primary == nullptr) {
    return TRUE;
  }
  HGDI_WND hwnd = context->gdi->primary->hdc->hwnd;
  if (hwnd != nullptr && hwnd->invalid != nullptr) {
    hwnd->invalid->null = TRUE;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  if (ctx->session != nullptr) {
    ctx->session->HandleBeginPaint();
  }
  return TRUE;
}

BOOL HmrdpEndPaint(rdpContext* context) {
  if (context == nullptr) {
    return TRUE;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  if (ctx->session != nullptr) {
    ctx->session->HandleEndPaint();
  }
  return TRUE;
}

BOOL HmrdpDesktopResize(rdpContext* context) {
  if (context == nullptr) {
    return TRUE;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  if (ctx->session != nullptr) {
    ctx->session->HandleDesktopResize();
  }
  return TRUE;
}

// Custom pointer class: forward the remote cursor bitmap to the UI so it can be
// installed as the HarmonyOS system cursor. `New` is a no-op (we keep no client
// cursor state) and `Free` must not release the struct: FreeRDP's pointer cache
// owns it (see libfreerdp/cache/pointer.c).
BOOL HmrdpPointerNew(rdpContext*, rdpPointer*) {
  return TRUE;
}

void HmrdpPointerFree(rdpContext*, rdpPointer*) {}

BOOL HmrdpPointerSet(rdpContext* context, rdpPointer* pointer) {
  if (context != nullptr && pointer != nullptr) {
    HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
    if (ctx->session != nullptr) {
      ctx->session->HandlePointerShape(pointer->width, pointer->height, pointer->xPos,
                                       pointer->yPos, pointer->xorBpp, pointer->xorMaskData,
                                       pointer->lengthXorMask, pointer->andMaskData,
                                       pointer->lengthAndMask);
    }
  }
  return TRUE;
}

BOOL HmrdpPointerSetDefault(rdpContext* context) {
  if (context != nullptr) {
    HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
    if (ctx->session != nullptr) {
      ctx->session->HandlePointerDefault();
    }
  }
  return TRUE;
}

BOOL HmrdpPointerSetNull(rdpContext* context) {
  if (context != nullptr) {
    HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
    if (ctx->session != nullptr) {
      ctx->session->HandlePointerHidden();
    }
  }
  return TRUE;
}

// Only used when FreeRDP_GrabMouse is set (relative mouse); the HmRdp sessions
// are absolute, so there is nothing to move.
BOOL HmrdpPointerSetPosition(rdpContext*, UINT32, UINT32) {
  return TRUE;
}

BOOL HmrdpPlaySound(rdpContext*, const PLAY_SOUND_UPDATE*) {
  return TRUE;
}

// Server Set Keyboard Indicators PDU: the remote session's toggle-key state.
// The client-side default for this slot sends the PDU straight back (it exists
// for proxy forwarding), so it must be replaced to consume it here.
BOOL HmrdpSetKeyboardIndicators(rdpContext* context, UINT16 ledFlags) {
  if (context == nullptr) {
    return TRUE;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  if (ctx->session != nullptr) {
    ctx->session->HandleRemoteLockState(ledFlags);
  }
  return TRUE;
}

int HmrdpAudioSinkAdapter(void* context, const void* data, size_t size, int sampleRate,
                          int channels) {
  if (context == nullptr || data == nullptr || size == 0) {
    return 0;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(static_cast<rdpContext*>(context));
  if (ctx->session != nullptr) {
    return ctx->session->OnAudioData(data, size, sampleRate, channels);
  }
  return 0;
}

// Announces every clipboard format the client understands. Sent once after
// MonitorReady so the server knows it can offer text / HTML / images; the
// concrete content advertisements follow when the user pushes something.
UINT SendCapabilityFormatList(CliprdrClientContext* cliprdr) {
  if (cliprdr == nullptr || cliprdr->ClientFormatList == nullptr) {
    return CHANNEL_RC_OK;
  }
  CLIPRDR_FORMAT formats[5] = {};
  formats[0].formatId = CF_UNICODETEXT;
  formats[1].formatId = CF_DIB;
  formats[2].formatId = kHtmlFormatId;
  formats[2].formatName = const_cast<char*>(kHtmlFormatName);
  formats[3].formatId = kFileGroupDescriptorId;
  formats[3].formatName = const_cast<char*>(kFileGroupDescriptorName);
  formats[4].formatId = kFileContentsId;
  formats[4].formatName = const_cast<char*>(kFileContentsName);
  CLIPRDR_FORMAT_LIST formatList = {};
  formatList.common.msgType = CB_FORMAT_LIST;
  formatList.common.msgFlags = 0;
  formatList.numFormats = 5;
  formatList.formats = formats;
  HMRDP_LOGI("cliprdr sending capability format list (text/dib/html/files)");
  return cliprdr->ClientFormatList(cliprdr, &formatList);
}

// Advertises the concrete content of the local clipboard for `kind`. The
// channel owns the ClientFormatList sender, so this only builds the list.
UINT SendLocalFormatList(CliprdrClientContext* cliprdr, int kind, bool hasPng,
                         bool hasDib) {
  if (cliprdr == nullptr || cliprdr->ClientFormatList == nullptr) {
    return CHANNEL_RC_OK;
  }
  CLIPRDR_FORMAT formats[2] = {};
  UINT32 count = 0;
  if (kind == kClipHtml) {
    // HTML plus a plain-text fallback so text-only remote targets still paste.
    formats[count].formatId = kHtmlFormatId;
    formats[count].formatName = const_cast<char*>(kHtmlFormatName);
    ++count;
    formats[count].formatId = CF_UNICODETEXT;
    ++count;
  } else if (kind == kClipFiles) {
    // The descriptor list is what the server pulls to learn the files; the
    // matching FileContents pseudo-format is announced alongside it, as the
    // Windows client does.
    formats[count].formatId = kFileGroupDescriptorId;
    formats[count].formatName = const_cast<char*>(kFileGroupDescriptorName);
    ++count;
    formats[count].formatId = kFileContentsId;
    formats[count].formatName = const_cast<char*>(kFileContentsName);
    ++count;
  } else if (kind == kClipImage) {
    // Offer the compressed PNG first when we have it; keep CF_DIB as a fallback
    // for servers without PNG support (and for A/B comparison).
    if (hasPng) {
      formats[count].formatId = kPngFormatId;
      formats[count].formatName = const_cast<char*>(kPngFormatName);
      ++count;
    }
    if (hasDib) {
      formats[count].formatId = CF_DIB;
      ++count;
    }
  } else {
    formats[count].formatId = CF_UNICODETEXT;
    ++count;
  }
  CLIPRDR_FORMAT_LIST formatList = {};
  formatList.common.msgType = CB_FORMAT_LIST;
  formatList.common.msgFlags = 0;
  formatList.numFormats = count;
  formatList.formats = formats;
  HMRDP_LOGI("cliprdr sending local format list (kind=%{public}d)", kind);
  return cliprdr->ClientFormatList(cliprdr, &formatList);
}

UINT HmrdpCliprdrMonitorReady(CliprdrClientContext* cliprdr,
                              const CLIPRDR_MONITOR_READY*) {
  if (cliprdr == nullptr || cliprdr->custom == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  return static_cast<Session*>(cliprdr->custom)->OnCliprdrMonitorReady();
}

UINT HmrdpCliprdrServerCapabilities(CliprdrClientContext*,
                                    const CLIPRDR_CAPABILITIES* capabilities) {
  // Logged because the file-clip features the channel lets through are the
  // intersection with these flags (cliprdr_client_capabilities).
  if (capabilities == nullptr) {
    return CHANNEL_RC_OK;
  }
  for (UINT32 i = 0; i < capabilities->cCapabilitiesSets; i++) {
    const CLIPRDR_CAPABILITY_SET& set = capabilities->capabilitySets[i];
    if (set.capabilitySetType != CB_CAPSTYPE_GENERAL ||
        set.capabilitySetLength < CB_CAPSTYPE_GENERAL_LEN) {
      continue;
    }
    const CLIPRDR_GENERAL_CAPABILITY_SET* general =
        reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(&set);
    HMRDP_LOGI("cliprdr server caps: version=%{public}u flags=0x%{public}x",
               general->version, general->generalFlags);
  }
  return CHANNEL_RC_OK;
}

UINT HmrdpCliprdrServerFormatList(CliprdrClientContext* cliprdr,
                                  const CLIPRDR_FORMAT_LIST* formatList) {
  if (cliprdr == nullptr || cliprdr->custom == nullptr || formatList == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  return static_cast<Session*>(cliprdr->custom)->OnCliprdrServerFormatList(formatList);
}

UINT HmrdpCliprdrServerFormatListResponse(CliprdrClientContext*,
                                          const CLIPRDR_FORMAT_LIST_RESPONSE*) {
  return CHANNEL_RC_OK;
}

UINT HmrdpCliprdrServerLockClipboardData(CliprdrClientContext*,
                                         const CLIPRDR_LOCK_CLIPBOARD_DATA*) {
  return CHANNEL_RC_OK;
}

UINT HmrdpCliprdrServerUnlockClipboardData(CliprdrClientContext*,
                                           const CLIPRDR_UNLOCK_CLIPBOARD_DATA*) {
  return CHANNEL_RC_OK;
}

UINT HmrdpCliprdrServerFormatDataRequest(CliprdrClientContext* cliprdr,
                                         const CLIPRDR_FORMAT_DATA_REQUEST* request) {
  if (cliprdr == nullptr || cliprdr->custom == nullptr || request == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  return static_cast<Session*>(cliprdr->custom)->OnCliprdrServerFormatDataRequest(request);
}

UINT HmrdpCliprdrServerFormatDataResponse(CliprdrClientContext* cliprdr,
                                          const CLIPRDR_FORMAT_DATA_RESPONSE* response) {
  if (cliprdr == nullptr || cliprdr->custom == nullptr || response == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  return static_cast<Session*>(cliprdr->custom)->OnCliprdrServerFormatDataResponse(response);
}

UINT HmrdpCliprdrServerFileContentsRequest(CliprdrClientContext* cliprdr,
                                           const CLIPRDR_FILE_CONTENTS_REQUEST* request) {
  if (cliprdr == nullptr || cliprdr->custom == nullptr || request == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  return static_cast<Session*>(cliprdr->custom)
      ->HandleLocalFileContentsRequest(request);
}

UINT HmrdpCliprdrServerFileContentsResponse(CliprdrClientContext* cliprdr,
                                            const CLIPRDR_FILE_CONTENTS_RESPONSE* response) {
  if (cliprdr == nullptr || cliprdr->custom == nullptr || response == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  return static_cast<Session*>(cliprdr->custom)
      ->HandleRemoteFileContentsResponse(response);
}

// Captures the cliprdr channel interface while still letting the default
// handler run (it is what initialises the GFX pipeline).
void HmrdpChannelConnected(void* context, const ChannelConnectedEventArgs* e) {
  freerdp_client_OnChannelConnectedEventHandler(context, e);
  if (context == nullptr || e == nullptr || e->name == nullptr) {
    return;
  }
  HMRDP_LOGI("channel connected: %{public}s iface=%{public}d", e->name,
             e->pInterface != nullptr ? 1 : 0);
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(static_cast<rdpContext*>(context));
  if (ctx->session != nullptr && strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0) {
    ctx->session->HandleCliprdrConnected(
        reinterpret_cast<CliprdrClientContext*>(e->pInterface));
  }
  if (ctx->session != nullptr && strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) == 0) {
    // Runs after freerdp_client_OnChannelConnectedEventHandler (which sets up the
    // GFX pipeline), so the decode callback is already installed. The wrappers
    // feed the frame work meter, and the same wrappers are used by the offline
    // replay so both report the same "本机" (hmrdp_gfx_work.h).
    RdpgfxClientContext* gfx = reinterpret_cast<RdpgfxClientContext*>(e->pInterface);
    GfxWorkInstall(gfx, ctx->session->workMeter());
    // START_FRAME is the point that is still *before* the frame's writes: gdi
    // composes into the presenter's own desktop buffer (zero-copy), so the previous
    // frame's GPU copy out of it has to drain here, not in BeginPaint (which FreeRDP
    // calls after the frame's decode - see GfxWorkSetFrameBeginHook).
    Session* session = ctx->session;
    GfxWorkSetFrameBeginHook(gfx, [session]() { session->HandleFrameBegin(); });
    // The frame pipeline is offloaded only once the connection is fully
    // established (HmrdpPostConnect): during the connect sequence a failure tears
    // the channels down from inside freerdp_connect, which would race the worker.
    ctx->session->SetGfxContext(gfx);
  }
}

BOOL HmrdpPreConnect(freerdp* instance) {
  if (instance == nullptr || instance->context == nullptr) {
    return FALSE;
  }
  rdpSettings* settings = instance->context->settings;
  if (settings == nullptr) {
    return FALSE;
  }
  freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
  freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_NATIVE_XSERVER);
  HMRDP_LOGI("preconnect: clipboard=%{public}d gfx=%{public}d accel=%{public}d",
             freerdp_settings_get_bool(settings, FreeRDP_RedirectClipboard) ? 1 : 0,
             freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline) ? 1 : 0,
             hmrdp::HardwareAccelEnabled() ? 1 : 0);

  // Register the raw GFX capture hook (no-op unless the user enabled the
  // capture). Harmless when FreeRDP was not built with the HmRdp patch.
  if (HmrdpSetGfxRawCapture != nullptr) {
    HmrdpSetGfxRawCapture(&HmrdpGfxRawCapture);
  }
  // Hand the QoS hook to the patched winpr: its pool workers (tile decoding) and
  // the drdynvc thread (the frame pipeline) call it once per thread.
  if (HmrdpSetThreadQoSApplier != nullptr) {
    HmrdpSetThreadQoSApplier(&HmrdpThreadQoSHook);
    HMRDP_LOGI("qos: applier registered, libqos.so %{public}s",
               hmrdp::ResolveQoS() != nullptr ? "available" : "unavailable");
  }

  // The graphics pipeline (RDPGFX) callbacks are registered by the client
  // library when the rdpgfx channel connects. Without these subscriptions the
  // GFX surface commands are never decoded and the screen stays black.
  PubSub_SubscribeChannelConnected(instance->context->pubSub, HmrdpChannelConnected);
  PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
                                      freerdp_client_OnChannelDisconnectedEventHandler);
  return TRUE;
}

BOOL HmrdpPostConnect(freerdp* instance) {
  if (instance == nullptr || instance->context == nullptr) {
    return FALSE;
  }
  rdpContext* context = instance->context;
  HmrdpContext* pre = reinterpret_cast<HmrdpContext*>(context);
  const rdpSettings* settings = context->settings;
  const int desktopWidth = settings != nullptr
                               ? static_cast<int>(freerdp_settings_get_uint32(
                                     settings, FreeRDP_DesktopWidth))
                               : 0;
  const int desktopHeight = settings != nullptr
                                ? static_cast<int>(freerdp_settings_get_uint32(
                                      settings, FreeRDP_DesktopHeight))
                                : 0;
  // gdi composes straight into the presenter's desktop buffer when the surface
  // (and with it the Vulkan device) is already up; otherwise it owns its buffer
  // and HandleEndPaint moves it over as soon as the presenter can provide one.
  if (!InitGdiWithPresenter(instance, pre->session != nullptr ? pre->session->presenter() : nullptr,
                            desktopWidth, desktopHeight)) {
    return FALSE;
  }
  context->update->BeginPaint = HmrdpBeginPaint;
  context->update->EndPaint = HmrdpEndPaint;
  context->update->DesktopResize = HmrdpDesktopResize;
  context->update->PlaySound = HmrdpPlaySound;
  // Consume the server's lock-key indicator updates instead of echoing them back
  // (see HmrdpSetKeyboardIndicators).
  context->update->SetKeyboardIndicators = HmrdpSetKeyboardIndicators;

  // Take over cursor handling so the remote pointer shape (text caret, hand,
  // resize arrows, ...) drives the HarmonyOS system cursor. Without this the
  // server's pointer updates are dropped and only the local arrow is shown.
  // The user can turn this off to fall back to the plain default cursor.
  if (g_useRdpCursor.load()) {
    rdpPointer pointer = {};
    pointer.size = sizeof(rdpPointer);
    pointer.New = HmrdpPointerNew;
    pointer.Free = HmrdpPointerFree;
    pointer.Set = HmrdpPointerSet;
    pointer.SetNull = HmrdpPointerSetNull;
    pointer.SetDefault = HmrdpPointerSetDefault;
    pointer.SetPosition = HmrdpPointerSetPosition;
    graphics_register_pointer(context->graphics, &pointer);
  }

  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  HMRDP_LOGI("post connect gdi %{public}ux%{public}u stride=%{public}u fmt=%{public}u",
             context->gdi->width, context->gdi->height, context->gdi->stride,
             context->gdi->dstFormat);
  if (ctx->session != nullptr) {
    // Now that the connection is established, move the frame pipeline off the
    // drdynvc dispatch thread: from here the chunks are only copied there and
    // processed on this session's own thread, so audio and input queued on that
    // thread are no longer held behind a frame.
    Session* session = ctx->session;
    auto* gfx = static_cast<RdpgfxClientContext*>(session->gfxContext());
    if (gfx != nullptr) {
      GfxWorkSetDataSinkHook(gfx, [session](const uint8_t* data, size_t size) {
        session->HandleGfxData(data, size);
      });
      session->StartGfxWorker(gfx);
    }
    session->HandlePostConnect();
  }
  return TRUE;
}

void HmrdpPostDisconnect(freerdp* instance) {
  if (instance == nullptr || instance->context == nullptr) {
    return;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(instance->context);
  if (ctx->session != nullptr) {
    // The GFX worker was joined before the disconnect (EventThread), so the
    // wrapped callbacks are no longer running and can be put back here; the
    // channel interface is dropped right before FreeRDP frees it below.
    auto* gfx = static_cast<RdpgfxClientContext*>(ctx->session->gfxContext());
    GfxWorkSetFrameBeginHook(gfx, nullptr);
    GfxWorkUninstall(gfx);
    ctx->session->SetGfxContext(nullptr);
    ctx->session->HandlePostDisconnect();
  }
  PubSub_UnsubscribeChannelConnected(instance->context->pubSub, HmrdpChannelConnected);
  PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub,
                                        freerdp_client_OnChannelDisconnectedEventHandler);
  gdi_free(instance);
}

BOOL HmrdpClientNew(freerdp* instance, rdpContext* context) {
  if (instance == nullptr || context == nullptr) {
    return FALSE;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  ctx->session = nullptr;
  instance->PreConnect = HmrdpPreConnect;
  instance->PostConnect = HmrdpPostConnect;
  instance->PostDisconnect = HmrdpPostDisconnect;
  return TRUE;
}

void HmrdpClientFree(freerdp*, rdpContext*) {}

int HmrdpClientStart(rdpContext*) { return 0; }
int HmrdpClientStop(rdpContext*) { return 0; }

BOOL HmrdpGlobalInit() {
  if (freerdp_handle_signals() != 0) {
    return FALSE;
  }
  return TRUE;
}

void HmrdpGlobalUninit() {}

RDP_CLIENT_ENTRY_POINTS g_entryPoints = {};

void EnsureEntryPoints() {
  // Two session windows can connect at the same time, so this construction is
  // serialised: without it the shared g_entryPoints could be zeroed/filled
  // concurrently.
  static std::mutex entryPointsMutex;
  std::lock_guard<std::mutex> lock(entryPointsMutex);
  if (g_entryPoints.Size != 0) {
    return;
  }
  SetupFreeRdpLogging();
  ZeroMemory(&g_entryPoints, sizeof(g_entryPoints));
  g_entryPoints.Version = RDP_CLIENT_INTERFACE_VERSION;
  g_entryPoints.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
  g_entryPoints.GlobalInit = HmrdpGlobalInit;
  g_entryPoints.GlobalUninit = HmrdpGlobalUninit;
  g_entryPoints.ContextSize = sizeof(HmrdpContext);
  g_entryPoints.ClientNew = HmrdpClientNew;
  g_entryPoints.ClientFree = HmrdpClientFree;
  g_entryPoints.ClientStart = HmrdpClientStart;
  g_entryPoints.ClientStop = HmrdpClientStop;
  HmrdpSetAudioSink(&HmrdpAudioSinkAdapter);
}

void AppendArg(std::vector<std::string>& args, const std::string& value) {
  args.push_back(value);
}

// 超分: the size the server is asked to render, i.e. the configured output
// resolution divided by the upscale ratio, rounded to an even number of pixels
// (some servers render odd desktop sizes badly). Mirrors SettingsStore.srSessionSize.
int SuperResolutionSessionSize(int output, int ratioPercent) {
  if (output <= 0 || ratioPercent <= 100) {
    return output;
  }
  const int scaled = (output * 100 + ratioPercent / 2) / ratioPercent;
  const int even = scaled - (scaled % 2);
  return even > 0 ? even : 2;
}

// Error events carry a code (to distinguish an authentication failure from a
// network problem) plus a message; internal errors use code 0.
}  // namespace

namespace {

// RAII for Session::AcquireContext(): keeps the FreeRDP context and its cliprdr
// channel alive for the duration of the call and guarantees the matching
// release on every path, including early returns.
class ContextGuard {
 public:
  explicit ContextGuard(Session& session) : session_(session), lease_(session.AcquireContext()) {}

  ~ContextGuard() { session_.ReleaseContext(); }

  ContextGuard(const ContextGuard&) = delete;
  ContextGuard& operator=(const ContextGuard&) = delete;

  bool valid() const { return lease_.valid(); }
  freerdp* instance() const { return lease_.instance; }
  CliprdrClientContext* cliprdr() const { return lease_.cliprdr; }

 private:
  Session& session_;
  Session::ContextLease lease_;
};

}  // namespace

Session::ContextLease Session::AcquireContext() {
  std::lock_guard<std::mutex> lock(lifecycleMutex_);
  // A lease is only ever handed out for a live instance; once Teardown() has
  // started (tearingDown_) or the instance is gone, callers get an invalid lease
  // and must do nothing.
  if (instance_ == nullptr || tearingDown_) {
    return ContextLease{};
  }
  activeLeases_++;
  return ContextLease{instance_, cliprdr_};
}

void Session::ReleaseContext() {
  std::lock_guard<std::mutex> lock(lifecycleMutex_);
  if (activeLeases_ > 0) {
    activeLeases_--;
  }
  if (activeLeases_ == 0) {
    leasesCv_.notify_all();
  }
}

void Session::MarkDestroyed() {
  phase_.store(SessionPhase::kDestroyed);
  {
    std::lock_guard<std::mutex> lock(destroyedMutex_);
    destroyed_ = true;
  }
  destroyedCv_.notify_all();
}

bool Session::WaitDestroyed(uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lock(destroyedMutex_);
  return destroyedCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [this] { return destroyed_; });
}

Session::Session() {
  // The live observer on the same chain the replay uses: account for a frame that
  // actually reached the surface (telemetry / input response / first frame).
  pipeline_.host()->SetPresentedFn(
      [this](bool presented, uint64_t, const PresentUploadInfo&) {
        if (presented) {
          AfterPresent();
        }
      });
  EnsureEntryPoints();
}

Session::~Session() {
  // Teardown() (the RDP thread) owns the context, so reaching here with one still
  // alive means it never ran. Surface that instead of pretending to clean up.
  if (instance_ != nullptr) {
    HMRDP_LOGE("session destroyed with a live FreeRDP context (teardown did not run)");
  }
  Disconnect();
}

void Session::Emit(SessionEvent event) {
  Emit(event, EventData{}, {});
}

void Session::Emit(SessionEvent event, const std::string& text) {
  EventData data;
  data.text = text;
  Emit(event, data, {});
}

void Session::EmitValues(SessionEvent event, std::initializer_list<int64_t> values) {
  EmitValues(event, values, {});
}

void Session::EmitValues(SessionEvent event, std::initializer_list<int64_t> values,
                         const std::vector<uint8_t>& bytes) {
  EventData data;
  size_t index = 0;
  for (int64_t value : values) {
    if (index >= 5) {
      break;
    }
    data.values[index] = value;
    index++;
  }
  Emit(event, data, bytes);
}

void Session::EmitBytes(SessionEvent event, const std::vector<uint8_t>& bytes) {
  Emit(event, EventData{}, bytes);
}

void Session::EmitError(SessionEvent event, uint32_t code, const std::string& message) {
  EventData data;
  data.values[0] = code;
  data.text = message;
  Emit(event, data, {});
}

void Session::Emit(SessionEvent event, const EventData& data,
                   const std::vector<uint8_t>& bytes) {
  if (eventFn_) {
    eventFn_(event, data, bytes);
  }
}

void Session::OnNetworkCharacteristics(uint32_t baseRtt, uint32_t averageRtt,
                                       uint32_t bandwidth) {
  if (baseRtt != 0) {
    netCharBaseRtt_.store(baseRtt);
  }
  if (averageRtt != 0) {
    netCharAverageRtt_.store(averageRtt);
  }
  if (bandwidth != 0) {
    netCharBandwidth_.store(bandwidth);
  }
}

void Session::SetGfxContext(void* gfx) {
  gfxContext_ = gfx;
}

void Session::SampleTelemetry() {
  const uint64_t now = NowMs();
  const uint64_t elapsedMs = metricsStarted_ ? now - lastMetricsTick_ : 0;
  lastMetricsTick_ = now;

  SessionTelemetry sample;
  sample.sampleSeq = ++telemetrySeq_;
  sample.windowElapsedMs = elapsedMs;

  uint64_t inBytes = 0;
  uint64_t outBytes = 0;
  rdpContext* context = instance_ != nullptr ? instance_->context : nullptr;
  if (context != nullptr) {
    freerdp_get_stats(context->rdp, &inBytes, &outBytes, nullptr, nullptr);
  }
  const uint32_t frames = frameCount_.load();
  if (metricsStarted_) {
    sample.rxBytesWindow = inBytes - lastInBytes_;
    sample.txBytesWindow = outBytes - lastOutBytes_;
    sample.framesWindow = frames - lastFrameCount_;
  }
  lastInBytes_ = inBytes;
  lastOutBytes_ = outBytes;
  lastFrameCount_ = frames;
  metricsStarted_ = true;

  // Average RTT is the steady-state value; base RTT is the fallback before the
  // first average is reported. -1 means "not measured yet".
  const uint32_t averageRtt = netCharAverageRtt_.load();
  const uint32_t baseRtt = netCharBaseRtt_.load();
  const uint32_t measured = averageRtt != 0 ? averageRtt : baseRtt;
  sample.rttMs = measured != 0 ? static_cast<int32_t>(measured) : -1;

  // The work sources drain here, on the RDP thread, so the window is exactly one
  // tick and the drain never races the pipeline's teardown. No field is a
  // pre-summed total: the reader adds the phases it shows.
  const GfxWorkMeter::Sample work = pipeline_.meter()->Drain();
  sample.windowFrames = work.frames;
  sample.zgxParseUs = work.zgxParseUs;
  sample.decodeUs = work.decodeUs;
  sample.composeUs = work.composeUs;
  sample.presentUs = work.presentUs;
  sample.presentWaitUs = work.presentWaitUs;
  sample.syncUs = work.syncUs;
  sample.windowBytes = work.bytes;
  sample.windowCommands = work.commands;

  sample.audioRateHz = audio_.TakeLossStats(&sample.audioLostBytes, &sample.audioTotalBytes);
  if (pipeline_.presenter() != nullptr) {
    pipeline_.presenter()->TakeGpuTimings(&sample.gpuCopyUs, &sample.gpuSrUs, &sample.gpuBlitUs);
  }
  {
    std::lock_guard<std::mutex> lock(telemetryMutex_);
    telemetry_ = sample;
  }
}

SessionTelemetry Session::ReadTelemetry() {
  std::lock_guard<std::mutex> lock(telemetryMutex_);
  return telemetry_;
}

void Session::SetError(const std::string& error) {
  SetError(0, error);
}

void Session::SetError(uint32_t code, const std::string& error) {
  lastErrorCode_ = code;
  lastError_ = error;
  EmitError(SessionEvent::kError, code, error);
}

bool Session::Connect(const RdpOptions& options) {
  // A Session is created per attempt and never reused, so kIdle -> kStarting is
  // the only entry. A second Connect on the same object is a programming error.
  SessionPhase expected = SessionPhase::kIdle;
  if (!phase_.compare_exchange_strong(expected, SessionPhase::kStarting)) {
    SetError("session already running");
    return false;
  }
  lastError_.clear();
  firstFrameSent_ = false;
  frameCount_ = 0;
  pipeline_.meter()->Reset();
  gfxContext_ = nullptr;
  lastInBytes_ = 0;
  lastOutBytes_ = 0;
  lastFrameCount_ = 0;
  telemetrySeq_ = 0;
  netCharBaseRtt_ = 0;
  netCharAverageRtt_ = 0;
  netCharBandwidth_ = 0;
  metricsStarted_ = false;

  rdpContext* context = freerdp_client_context_new(&g_entryPoints);
  if (context == nullptr) {
    SetError("freerdp_client_context_new failed");
    MarkDestroyed();
    return false;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  ctx->session = this;
  instance_ = context->instance;
  // Capture the server's network characteristics via the public autodetect
  // callback (the client does not store them internally).
  rdpAutoDetect* autodetect = autodetect_get(context);
  if (autodetect != nullptr) {
    autodetect->NetworkCharacteristicsResult = HmrdpNetworkCharacteristicsResult;
  }

  std::vector<std::string> args;
  AppendArg(args, "hmrdp");
  std::ostringstream address;
  address << "/v:" << options.host << ":" << options.port;
  AppendArg(args, address.str());
  if (!options.username.empty()) {
    AppendArg(args, "/u:" + options.username);
  }
  if (!options.domain.empty()) {
    AppendArg(args, "/d:" + options.domain);
  }
  // 超分: the resolution the server renders is the configured output divided by
  // the upscale ratio; the presenter upscales the frames back to the output size
  // (see hmrdp_vk_renderer.cpp). Without it the session desktop *is* the output.
  int sessionWidth = options.width;
  int sessionHeight = options.height;
  desktopMagnification_ = 1.0;
  if (options.srEnabled && options.srRatioPercent > 100) {
    sessionWidth = SuperResolutionSessionSize(options.width, options.srRatioPercent);
    sessionHeight = SuperResolutionSessionSize(options.height, options.srRatioPercent);
    if (sessionWidth > 0) {
      // The factor the presenter upscales the picture by; the pointer bitmap is
      // magnified by the same one (HandlePointerShape).
      desktopMagnification_ = static_cast<double>(options.width) / sessionWidth;
    }
    if (desktopMagnification_ < 1.0) {
      desktopMagnification_ = 1.0;
    }
    if (pipeline_.presenter() != nullptr) {
      pipeline_.presenter()->SetSuperResolution(true, options.srBackend, options.width, options.height);
    }
    HMRDP_LOGI("session %{public}dx%{public}d (super resolution %{public}s %{public}d%% -> "
               "%{public}dx%{public}d)",
               options.width, options.height,
               options.srBackend == SuperResolutionBackend::kFsr ? "fsr" : "xengine",
               options.srRatioPercent, sessionWidth, sessionHeight);
  } else if (pipeline_.presenter() != nullptr) {
    pipeline_.presenter()->SetSuperResolution(false, options.srBackend, 0, 0);
  }
  AppendArg(args, "/w:" + std::to_string(sessionWidth));
  AppendArg(args, "/h:" + std::to_string(sessionHeight));
  AppendArg(args, "/bpp:32");
  if (!options.gatewayHost.empty()) {
    AppendArg(args, "/g:" + options.gatewayHost + ":" +
                        std::to_string(options.gatewayPort));
    if (!options.gatewayUsername.empty()) {
      AppendArg(args, "/gu:" + options.gatewayUsername);
    }
    if (!options.gatewayDomain.empty()) {
      AppendArg(args, "/gd:" + options.gatewayDomain);
    }
  }

  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  const int status = freerdp_client_settings_parse_command_line(
      context->settings, static_cast<int>(argv.size()), argv.data(), FALSE);
  if (status != 0) {
    HMRDP_LOGE("settings parse failed, status=%{public}d", status);
    freerdp_client_settings_command_line_status_print(
        context->settings, status, static_cast<int>(argv.size()), argv.data());
    SetError("invalid connection settings");
    freerdp_client_context_free(context);
    instance_ = nullptr;
    MarkDestroyed();
    return false;
  }

  rdpSettings* settings = context->settings;
  // Credentials are applied through the settings API, never through the
  // command line, so they cannot leak via the parser's failure logging.
  if (!options.password.empty()) {
    freerdp_settings_set_string(settings, FreeRDP_Password, options.password.c_str());
  }
  if (!options.gatewayPassword.empty()) {
    freerdp_settings_set_string(settings, FreeRDP_GatewayPassword,
                                options.gatewayPassword.c_str());
  }
  freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, options.ignoreCertificate);
  if (!options.ignoreCertificate) {
    freerdp_settings_set_bool(settings, FreeRDP_AutoAcceptCertificate, TRUE);
  }
  // Clipboard redirection is always on: text / HTML / image / file, triggered
  // manually from the toolbar (see OnCliprdrServerFormatList).
  freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
  // Audio playback is requested only when ArkTS asked for it (it owns the
  // device probe and the AudioRenderer). The sink ring is native either way;
  // without the channel no PCM ever arrives, so an unsupported device simply
  // stays silent instead of failing.
  freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, options.enableAudio);
  // GFX (RDPGFX) is mandatory: without it the session does not render at all.
  freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
  // H.264/AVC444 stay off: RemoteFX Progressive is the desktop codec this client
  // uses, and AVC444 is a niche path with no benefit here without a GPU decoder.
  freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
  freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
  freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE);
  freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE);
  freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, TRUE);
  // Enables the RDPEI (touch/pen input) channel so ArkUI touch events can be
  // forwarded as native remote touch instead of mouse emulation.
  freerdp_settings_set_bool(settings, FreeRDP_MultiTouchInput, TRUE);
  // Remote desktop DPI scaling. The UI only offers the fixed Windows presets
  // (100/125/150/175/200/225), so the value is passed through as-is.
  if (options.scalePercent > 0) {
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopScaleFactor,
                                static_cast<UINT32>(options.scalePercent));
  }

  if (freerdp_client_start(context) != 0) {
    SetError("freerdp_client_start failed");
    freerdp_client_context_free(context);
    instance_ = nullptr;
    MarkDestroyed();
    return false;
  }

  stopRequested_ = false;
  // The RDP thread keeps the session alive for as long as it runs (it holds a
  // shared reference) and performs the teardown itself, so neither the FreeRDP
  // context nor this object can be freed from under it by the UI thread.
  auto* self = new std::shared_ptr<Session>(shared_from_this());
  HANDLE thread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
    std::unique_ptr<std::shared_ptr<Session>> keep(
        static_cast<std::shared_ptr<Session>*>(param));
    (*keep)->EventThread();
    return 0;
  }, self, 0, nullptr);
  if (thread == nullptr) {
    delete self;
    SetError("failed to create session thread");
    freerdp_client_stop(context);
    freerdp_client_context_free(context);
    instance_ = nullptr;
    MarkDestroyed();
    return false;
  }
  CloseHandle(thread);
  return true;
}

void Session::EventThread() {
  freerdp* instance = instance_;
  if (instance == nullptr) {
    // The thread only exists for a live instance, so this is unreachable in
    // practice; still finish the lifecycle so WaitDestroyed() cannot time out.
    MarkDestroyed();
    return;
  }
  // This thread reads the transport and hands the dynamic-channel data to the
  // drdynvc thread; mark it like the frame pipeline (HarmonyOS QoS).
  HmrdpThreadQoSHook();
  const BOOL ok = freerdp_connect(instance);
  if (!ok) {
    // freerdp_connect() already ran freerdp_disconnect() on failure
    // (libfreerdp/core/freerdp.c), so it is deliberately not called again here.
    // The attempt's terminal event is this single kError, never a kDisconnected
    // as well: an attempt either fails or drops, it does not do both.
    const UINT32 code = static_cast<UINT32>(freerdp_get_last_error(instance->context));
    const char* error = freerdp_get_last_error_string(code);
    SetError(code, error != nullptr ? error : "connection failed");
    Teardown();
    return;
  }

  phase_.store(SessionPhase::kConnected);
  {
    HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {0};
    while (!stopRequested_.load() &&
           !freerdp_shall_disconnect_context(instance->context)) {
      const DWORD count = freerdp_get_event_handles(instance->context, handles,
                                                    MAXIMUM_WAIT_OBJECTS);
      if (count == 0) {
        SetError("freerdp_get_event_handles failed");
        break;
      }
      // Wake only when the transport has work or the next telemetry sample is
      // due; the loop is event-driven, so polling on a fixed period would wake it
      // needlessly while idle. SampleTelemetry runs on the loop after the wait.
      DWORD timeoutMs = 0;
      if (!metricsStarted_) {
        // Establish the metrics baseline promptly on the first iteration.
        timeoutMs = 0;
      } else {
        const uint64_t since = NowMs() - lastMetricsTick_;
        timeoutMs = since >= 1000 ? 0 : static_cast<DWORD>(1000 - since);
      }
      const DWORD status = WaitForMultipleObjects(count, handles, FALSE, timeoutMs);
      if (status == WAIT_FAILED) {
        SetError("wait for events failed");
        break;
      }
      if (!freerdp_check_event_handles(instance->context)) {
        if (freerdp_get_last_error(instance->context) != FREERDP_ERROR_SUCCESS) {
          const UINT32 code = static_cast<UINT32>(freerdp_get_last_error(instance->context));
          const char* error = freerdp_get_last_error_string(code);
          if (error != nullptr) {
            lastErrorCode_ = code;
            lastError_ = error;
          }
        }
        break;
      }

      // Take one telemetry sample once the baseline is established and roughly
      // once per second. It runs on this (RDP) thread so draining the work meter /
      // audio / GPU sources cannot race the pipeline's teardown; ArkTS pulls the
      // stored sample and computes the displayed figures. The loop wakes at least
      // every 100ms.
      if (!metricsStarted_ || NowMs() - lastMetricsTick_ >= 1000) {
        SampleTelemetry();
      }
    }
  }

  // Why the session ended, so the UI can tell "another connection took over" from
  // a network drop instead of showing one generic text. The server's ErrorInfo
  // PDU (ERRINFO_*, e.g. 断开原因=被其他连接接管) is more specific than the
  // client-side last error, so it wins; a user-requested stop has no reason and is
  // reported as an empty payload (the UI's own "disconnected" wording covers it).
  if (stopRequested_.load()) {
    lastErrorCode_ = 0;
    lastError_.clear();
  } else if (instance->context != nullptr) {
    const UINT32 errorInfo = freerdp_error_info(instance);
    const UINT32 lastError = static_cast<UINT32>(freerdp_get_last_error(instance->context));
    if (errorInfo != 0) {
      lastErrorCode_ = (static_cast<UINT32>(FREERDP_ERROR_ERRINFO_CLASS) << 16) | errorInfo;
      const char* reason = freerdp_get_last_error_string(lastErrorCode_);
      lastError_ = reason != nullptr ? reason : "";
    } else if (lastError != FREERDP_ERROR_SUCCESS) {
      lastErrorCode_ = lastError;
      const char* reason = freerdp_get_last_error_string(lastError);
      lastError_ = reason != nullptr ? reason : "";
    }
  }

  // Single teardown order (doc_agent/architecture.md §4.3): join the GFX worker
  // here, before the disconnect. The worker parses into the rdpgfx plugin, which
  // freerdp_disconnect frees (freerdp_channels_close), so it must be joined
  // first; this is the only StopGfxWorker() call for a live session.
  StopGfxWorker();
  freerdp_disconnect(instance);
  EmitError(SessionEvent::kDisconnected, lastErrorCode_, lastError_);
  Teardown();
}

void Session::Disconnect() {
  // Runs on the UI thread and only *signals*: the RDP thread owns the teardown
  // (Teardown()), so the FreeRDP context is never freed while that thread may
  // still be using it. Idempotent and safe at any point - before the connect
  // finished, while connected, or after the thread already ended.
  stopRequested_ = true;
  {
    // A live attempt moves to kStopping; a never-started (kIdle) or already gone
    // (kStopping/kDestroyed) session keeps its phase.
    const SessionPhase phase = phase_.load();
    if (phase == SessionPhase::kStarting || phase == SessionPhase::kConnected) {
      SessionPhase expected = phase;
      phase_.compare_exchange_strong(expected, SessionPhase::kStopping);
    }
  }
  std::lock_guard<std::mutex> lock(lifecycleMutex_);
  if (instance_ != nullptr && instance_->context != nullptr) {
    // The abort event is what the RDP loop waits on
    // (freerdp_shall_disconnect_context reads it) and what freerdp_disconnect
    // sets itself, so both a running connect and an established loop wake at
    // once. Signalled under the lifecycle lock so Teardown() cannot free the
    // context between the test and the call.
    freerdp_abort_connect_context(instance_->context);
  }
}

void Session::Teardown() {
  // Runs on the RDP thread once its connect/loop has ended. It is the sole owner
  // of the FreeRDP context: it stops handing out leases, waits for the
  // outstanding ones to be returned and only then frees - so no other thread can
  // be holding the context (or its cliprdr channel) when it goes away. This is
  // the single teardown order of doc_agent/architecture.md §4.3; the GFX worker
  // was already joined before the disconnect (EventThread), so nothing else runs
  // against the context here.
  freerdp* instance = nullptr;
  {
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    tearingDown_ = true;
    instance = instance_;
    instance_ = nullptr;
    cliprdr_ = nullptr;
    // Every lease holder only performs a short, bounded FreeRDP call (and the
    // abort/disconnect this teardown follows makes a blocked send fail), so this
    // returns promptly. The wait is unbounded: the context must never be freed
    // under a live lease.
    leasesCv_.wait(lock, [this] { return activeLeases_ == 0; });
  }
  if (instance != nullptr && instance->context != nullptr) {
    if (instance->context->settings != nullptr) {
      freerdp_settings_set_string(instance->context->settings, FreeRDP_Password, nullptr);
      freerdp_settings_set_string(instance->context->settings, FreeRDP_GatewayPassword,
                                  nullptr);
    }
    freerdp_client_stop(instance->context);
    freerdp_client_context_free(instance->context);
  }
  // The channel interface died with the context.
  clipboardReady_ = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    // The channel is gone; a pending download request can never be answered.
    fileContentsPending_ = false;
  }
  audio_.Reset();
  // The desktop buffer dies with the presenter; the next connect re-attaches.
  pipeline_.host()->DetachDesktopBuffer();
  if (pipeline_.presenter() != nullptr) {
    pipeline_.presenter()->Reset();
  }
  // Terminal: publishes the phase and wakes WaitDestroyed().
  MarkDestroyed();
}

void Session::HandlePostConnect() {
  Emit(SessionEvent::kConnected);
}

// Defensive high-water mark for the GFX offload queue. The queue cannot be
// capped by dropping (every chunk is part of the GFX stream and must reach the
// parser), so crossing this only warns: it means the frame worker is far behind
// and memory is growing, which is worth seeing rather than silently absorbing.
constexpr size_t kGfxQueueHighWaterBytes = 64 * 1024 * 1024;

void Session::HandleGfxData(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  // The raw chunk arrived, still ZGX-compressed and before PDU parse: stamp it on
  // this session's own meter so `zgx+parse` is measured from the same point the
  // capture hook used to (the hook has no context to find the meter with).
  pipeline_.meter()->OnChunk(static_cast<uint32_t>(size));
  // The drdynvc dispatch thread must never be blocked: it carries every dynamic
  // channel, so waiting here for the frame worker to drain would hold audio and
  // input behind the frame pipeline - exactly the coupling the offload exists to
  // remove. The queue is therefore left free to grow; it is a latency buffer, not
  // a throttle.
  bool queued = false;
  {
    std::lock_guard<std::mutex> lock(gfxQueueMutex_);
    if (gfxWorkerRun_) {
      gfxQueue_.emplace_back(data, data + size);
      gfxQueueBytes_.fetch_add(size);
      queued = true;
    }
  }
  if (queued) {
    const size_t pending = gfxQueueBytes_.load();
    if (pending >= kGfxQueueHighWaterBytes && !gfxQueueHighWaterWarned_.exchange(true)) {
      HMRDP_LOGW("gfx queue over high-water: %{public}zu bytes pending", pending);
    }
    gfxQueueCv_.notify_one();
    return;
  }
  // Worker not running (before it starts, or after it stopped): process inline on
  // this thread so the chunk is not lost. The lease keeps the context alive
  // across it, and refuses once teardown has begun.
  auto* gfx = static_cast<RdpgfxClientContext*>(gfxWorkerContext_);
  if (gfx == nullptr) {
    return;
  }
  ContextGuard ctx(*this);
  if (!ctx.valid()) {
    return;
  }
  DecodeExecutorScope decodeScope(pipeline_.executor());
  HmrdpGfxReplayRecv(gfx, data, static_cast<UINT32>(size));
}

void Session::GfxWorkerLoop() {
  // The whole frame pipeline runs here - ZGX decompress, PDU parse, image decode,
  // composite, present and the frame acknowledge - so the drdynvc dispatch thread
  // is never held behind a frame and audio/input queued on it are serviced at
  // once.
  for (;;) {
    std::vector<uint8_t> chunk;
    {
      std::unique_lock<std::mutex> lock(gfxQueueMutex_);
      gfxQueueCv_.wait(lock, [this] { return !gfxWorkerRun_ || !gfxQueue_.empty(); });
      if (!gfxWorkerRun_) {
        break;
      }
      chunk = std::move(gfxQueue_.front());
      gfxQueue_.pop_front();
      gfxQueueBytes_.fetch_sub(chunk.size());
    }
    if (gfxQueueBytes_.load() < kGfxQueueHighWaterBytes) {
      // Re-arm the high-water warning once the backlog has drained.
      gfxQueueHighWaterWarned_.store(false);
    }
    auto* gfx = static_cast<RdpgfxClientContext*>(gfxWorkerContext_);
    if (gfx == nullptr) {
      continue;
    }
    ContextGuard ctx(*this);
    if (!ctx.valid()) {
      continue;
    }
    DecodeExecutorScope decodeScope(pipeline_.executor());
    HmrdpGfxReplayRecv(gfx, chunk.data(), static_cast<UINT32>(chunk.size()));
  }
}

void Session::StartGfxWorker(void* gfx) {
  if (gfx == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(gfxQueueMutex_);
    if (gfxWorkerRun_) {
      return;
    }
    gfxWorkerContext_ = gfx;
    gfxWorkerRun_ = true;
    gfxWorkerThread_ = std::thread([this]() {
      // QoS for the frame worker (see HmrdpThreadQoSHook / patch 09): the platform
      // then schedules the frame work with less wake-up and preemption latency.
      HmrdpThreadQoSHook();
      GfxWorkerLoop();
    });
  }
  HMRDP_LOGI("gfx worker: started");
}

void Session::StopGfxWorker() {
  void* gfx = nullptr;
  {
    std::lock_guard<std::mutex> lock(gfxQueueMutex_);
    if (!gfxWorkerRun_) {
      return;
    }
    gfxWorkerRun_ = false;
    gfx = gfxWorkerContext_;
    gfxQueue_.clear();
    gfxQueueBytes_.store(0);
  }
  gfxQueueCv_.notify_all();
  if (gfxWorkerThread_.joinable()) {
    gfxWorkerThread_.join();
  }
  // Past this point the context must not be handed any more chunks; the sink
  // hook is what would hand them over.
  if (gfx != nullptr) {
    GfxWorkSetDataSinkHook(static_cast<RdpgfxClientContext*>(gfx), nullptr);
  }
  HMRDP_LOGI("gfx worker: stopped");
}

void Session::HandleFrameBegin() {
  // RDPGFX START_FRAME: this frame's first pixel write is about to happen. The
  // zero-copy wait and its `sync` accounting live in the shared frame host, which
  // the offline replay drives too (hmrdp_gfx_cpu.h), so there is one
  // implementation and one timing shape for both.
  pipeline_.host()->OnFrameBegin();
}

void Session::HandleBeginPaint() {
  // gdi is about to compose this frame into the primary buffer (the mirrored
  // surface's pixels are already there). Same wait as HandleFrameBegin, and a
  // no-op when that one already drained the previous frame's copy - it is kept
  // because the compose of a *non-mirrored* surface happens here.
  pipeline_.host()->OnBeginPaint();
}

void Session::HandleEndPaint() {
  // Runs on a FreeRDP channel thread: take a lease so teardown cannot free the
  // context (and with it gdi) while this frame is being presented.
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr) {
    return;
  }
  // FreeRDP's gdi owns the desktop; a completed frame shows up here with the
  // invalid rectangle final, and it is uploaded through the presenter.
  rdpGdi* gdi = ctx.instance()->context->gdi;
  static int endPaintCount = 0;
  if (gdi == nullptr || gdi->primary == nullptr || gdi->primary_buffer == nullptr) {
    if (endPaintCount < 3) {
      HMRDP_LOGW("endpaint: gdi not ready");
      endPaintCount++;
    }
    return;
  }
  HGDI_WND hwnd = gdi->primary->hdc->hwnd;
  if (hwnd == nullptr || hwnd->invalid == nullptr) {
    return;
  }
  const INT32 x = hwnd->invalid->x;
  const INT32 y = hwnd->invalid->y;
  const INT32 width = hwnd->invalid->w;
  const INT32 height = hwnd->invalid->h;
  if (endPaintCount < 12) {
    HMRDP_LOGI("endpaint #%{public}d null=%{public}d rect %{public}d,%{public}d %{public}dx%{public}d stride=%{public}u",
               endPaintCount, hwnd->invalid->null ? 1 : 0, x, y, width, height,
               gdi->stride);
    endPaintCount++;
  }
  if (hwnd->invalid->null) {
    return;
  }
  // Attach gdi to the presenter's desktop buffer (zero-copy), wait for the GPU to
  // release it, and present the dirty region: all shared with the offline CPU
  // replay route (hmrdp_gfx_cpu.cpp), so the live path and the replay present
  // exactly the same way. The frame's own accounting runs in the observer
  // installed above, exactly where the replay's does.
  uint64_t presentUs = 0;
  (void)pipeline_.host()->OnEndPaint(gdi, &presentUs, nullptr);
}

void Session::AfterPresent() {
  // The frame's present is charged to the meter by the shared frame host itself
  // (work and blocked halves), so that the live session and the replay cannot
  // report it differently; this only does the per-frame bookkeeping.
  frameCount_.fetch_add(1);

  if (!firstFrameSent_) {
    firstFrameSent_ = true;
    Emit(SessionEvent::kFirstFrame);
  }
}

void Session::HandleDesktopResize() {
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr) {
    return;
  }
  rdpSettings* settings = ctx.instance()->context->settings;
  const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  EmitValues(SessionEvent::kResize, {width, height});
}

void Session::HandlePointerShape(uint32_t width, uint32_t height, uint32_t hotX, uint32_t hotY,
                                 uint32_t xorBpp, const uint8_t* xorMask, uint32_t xorLen,
                                 const uint8_t* andMask, uint32_t andLen) {
  if (width == 0 || height == 0 || width > kMaxCursorSourceSide ||
      height > kMaxCursorSourceSide || xorMask == nullptr || xorLen == 0) {
    return;
  }
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> bgra(stride * height, 0);
  if (!freerdp_image_copy_from_pointer_data(bgra.data(), PIXEL_FORMAT_BGRA32,
                                            static_cast<UINT32>(stride), 0, 0, width, height,
                                            xorMask, xorLen, andMask, andLen, xorBpp, nullptr)) {
    return;
  }
  // 超分辨率: the server sizes the pointer bitmap in *session* pixels, so with
  // 超分 on it arrives output÷session smaller than the picture it sits on and is
  // magnified by the same factor the desktop is upscaled by - otherwise the cursor
  // stops matching the picture. The magnification is a real resample (premultiplied
  // Lanczos + contrast-adaptive sharpen, see UpscaleBgra), not a bilinear stretch.
  // Custom cursors cap at 256x256, so anything that would exceed it (a large
  // pointer, or a magnified session bitmap) is scaled to fit -- hot spot included
  // -- instead of falling back to the default arrow.
  const double magnification = desktopMagnification_ > 1.0 ? desktopMagnification_ : 1.0;
  uint32_t outW = static_cast<uint32_t>(static_cast<double>(width) * magnification + 0.5);
  uint32_t outH = static_cast<uint32_t>(static_cast<double>(height) * magnification + 0.5);
  if (outW < 1) {
    outW = 1;
  }
  if (outH < 1) {
    outH = 1;
  }
  const uint32_t srcSide = width > height ? width : height;
  uint32_t dstSide = outW > outH ? outW : outH;
  if (dstSide > kMaxCursorSide) {
    const double factor = static_cast<double>(kMaxCursorSide) / dstSide;
    outW = static_cast<uint32_t>(static_cast<double>(outW) * factor + 0.5);
    outH = static_cast<uint32_t>(static_cast<double>(outH) * factor + 0.5);
    if (outW < 1) {
      outW = 1;
    }
    if (outH < 1) {
      outH = 1;
    }
    dstSide = outW > outH ? outW : outH;
  }
  // The anchor follows the size actually produced, so a clamped cursor keeps it.
  uint32_t outHotX = static_cast<uint32_t>(
      static_cast<double>(hotX) * (static_cast<double>(outW) / static_cast<double>(width)) + 0.5);
  uint32_t outHotY = static_cast<uint32_t>(
      static_cast<double>(hotY) * (static_cast<double>(outH) / static_cast<double>(height)) + 0.5);
  if (outHotX > outW) {
    outHotX = outW;
  }
  if (outHotY > outH) {
    outHotY = outH;
  }
  if (outW != width || outH != height) {
    std::vector<uint8_t> scaled;
    if (dstSide <= srcSide) {
      DownscaleBgra(bgra, width, height, outW, outH, &scaled);
    } else {
      UpscaleBgra(bgra, width, height, outW, outH, &scaled);
    }
    bgra.swap(scaled);
  }
  // Identical repeats (pointer cache hits) are dropped on the ArkTS side, so the
  // bitmap is handed over on every shape the server sends.
  EmitValues(SessionEvent::kCursorShape, {outW, outH, outHotX, outHotY}, bgra);
}

void Session::HandlePointerDefault() {
  Emit(SessionEvent::kCursorDefault);
}

void Session::HandlePointerHidden() {
  Emit(SessionEvent::kCursorHidden);
}

void Session::HandlePostDisconnect() {
  clipboardReady_ = false;
  {
    // FreeRDP closes the channels right after this callback
    // (freerdp_disconnect -> freerdp_channels_close), so the channel pointer is
    // dropped under the lifecycle lock and the outstanding leases are waited out
    // here: once this returns, no lease can still point at the channel that is
    // about to be freed. The wait is unbounded, matching the teardown order of
    // doc_agent/architecture.md §4.3.
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    cliprdr_ = nullptr;
    leasesCv_.wait(lock, [this] { return activeLeases_ == 0; });
  }
  remoteRequestInFlight_ = false;
  remoteRequestKind_ = LocalClipKind::kNone;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    // The channel is gone: a pending download request can never be answered.
    fileContentsPending_ = false;
  }
  // Make the capture durable before the buffers are dropped with the session.
  hmrdp::GfxDumpFlush();
}

int Session::OnAudioData(const void* data, size_t size, int sampleRate, int channels) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  const int latency = audio_.Write(data, size, sampleRate, channels);
  int rate = 0;
  int count = 0;
  if (audio_.TakeFormatChange(&rate, &count)) {
    EmitValues(SessionEvent::kAudioFormat, {rate, count});
  }
  return latency;
}

int Session::ReadAudio(uint8_t* dest, size_t size) {
  return static_cast<int>(audio_.Read(dest, size));
}

void Session::AudioFormat(int* sampleRate, int* channels) {
  audio_.Format(sampleRate, channels);
}

bool Session::SendMouse(uint16_t flags, uint16_t x, uint16_t y) {
  static int mouseLog = 0;
  if (mouseLog < 20) {
    HMRDP_LOGI("sendMouse flags=0x%{public}x x=%{public}u y=%{public}u", flags, x, y);
    mouseLog++;
  }
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr ||
      ctx.instance()->context->input == nullptr) {
    return false;
  }
  return freerdp_input_send_mouse_event(ctx.instance()->context->input, flags, x, y);
}

bool Session::SetSuppressed(bool suppressed) {
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr ||
      ctx.instance()->context->gdi == nullptr) {
    return false;
  }
  return gdi_send_suppress_output(ctx.instance()->context->gdi, suppressed ? TRUE : FALSE);
}

bool Session::SendTouch(uint32_t flags, int32_t finger, uint32_t pressure, int32_t x,
                        int32_t y) {
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr) {
    return false;
  }
  rdpClientContext* client = reinterpret_cast<rdpClientContext*>(ctx.instance()->context);
  // Diagnostic only: without the RDPEI channel FreeRDP silently degrades touch
  // to concentrated mouse emulation, which shows up as stray clicks while
  // dragging. The first few contacts make that visible in the logs.
  static int touchLog = 0;
  if (touchLog < 24) {
    HMRDP_LOGI("sendTouch flags=0x%{public}x finger=%{public}d pressure=%{public}u rdpei=%{public}d",
               flags, finger, pressure, client->rdpei != nullptr ? 1 : 0);
    touchLog++;
  }
  return freerdp_client_handle_touch(client, flags, finger, pressure, x, y) ? true : false;
}

void Session::SetTouchHighRate(bool enabled) {
  if (HmrdpSetTouchFrameInterval != nullptr) {
    // The interval is the floor on how often the changed contacts may be flushed,
    // so it is what bounds the touch input rate the remote has to absorb: a lower
    // value is a higher rate. Upstream coalesces at 20ms (50Hz); the high-rate
    // option lowers that to 8ms (125Hz) rather than to 0, which would flush on
    // every input poll and let a drag become an unbounded stream of contact
    // updates that only the network, never the pacing, limits.
    HmrdpSetTouchFrameInterval(enabled ? kTouchFrameIntervalHighRateMs
                                       : kTouchFrameIntervalDefaultMs);
  }
}

void Session::SetRdpCursor(bool enabled) {
  g_useRdpCursor.store(enabled);
}

void Session::SetHardwareAccel(bool enabled) {
  // The presenter backend reads this (hmrdp_presenter.h); the session path picks
  // it up when the next presenter is created.
  hmrdp::SetHardwareAccelEnabled(enabled);
}

void Session::SetGfxCapture(bool enabled, const std::string& dir) {
  const bool wantEnabled = enabled && !dir.empty();
  // The capture writes the full GFX command stream (hmrdp_gfx.bin) used by the
  // replay harness. GfxDumpConfigure is idempotent, so re-applying the same
  // value (startup + settings page) does not truncate a running capture.
  if (wantEnabled) {
    hmrdp::GfxDumpConfigure(true, dir);
  } else {
    hmrdp::GfxDumpShutdown();
  }
  HMRDP_LOGI("gfx capture: %{public}s", wantEnabled ? "enabled" : "disabled");
}

bool Session::SendKey(uint8_t scancode, bool down, bool extended) {
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr ||
      ctx.instance()->context->input == nullptr) {
    return false;
  }
  UINT16 flags = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;
  if (extended) {
    flags |= KBD_FLAGS_EXTENDED;
  }
  return freerdp_input_send_keyboard_event(ctx.instance()->context->input, flags, scancode);
}

bool Session::SendUnicode(uint16_t codepoint, bool down) {
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr ||
      ctx.instance()->context->input == nullptr) {
    return false;
  }
  UINT16 flags = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;
  return freerdp_input_send_unicode_keyboard_event(ctx.instance()->context->input, flags,
                                                   codepoint);
}

namespace {
// Toggle keys the local keyboard can control; the Client Synchronize PDU also
// carries a Kana bit and nothing here can produce it.
constexpr uint32_t kLockMask = KBD_SYNC_SCROLL_LOCK | KBD_SYNC_NUM_LOCK | KBD_SYNC_CAPS_LOCK;
}  // namespace

bool Session::SendSynchronize(uint32_t flags) {
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr ||
      ctx.instance()->context->input == nullptr) {
    return false;
  }
  return freerdp_input_send_synchronize_event(ctx.instance()->context->input, flags);
}

bool Session::SendLockState(uint32_t flags) {
  const uint32_t locks = flags & kLockMask;
  // The same value the server was already told about needs no PDU: the input
  // path calls this on every key event so a local change is picked up even when
  // it happened while the window was not focused.
  if (localLocksKnown_.load() && localLocks_.load() == locks) {
    return true;
  }
  localLocks_.store(locks);
  localLocksKnown_.store(true);
  return SendSynchronize(locks);
}

bool Session::ReassertLockState(uint32_t flags) {
  // Unconditional: ArkTS decided the server disagrees and must be put back on the
  // local state, so the SendLockState() dedup is deliberately bypassed here.
  return SendSynchronize(flags & kLockMask);
}

void Session::HandleRemoteLockState(uint32_t ledFlags) {
  // Native only relays the server's report: whether it disagrees with the local
  // keyboard and should be corrected is ArkTS's decision (see kRemoteLockState /
  // ReassertLockState). Dedup and rate-limit live there too.
  EmitValues(SessionEvent::kRemoteLockState, {ledFlags & kLockMask});
}

void Session::HandleCliprdrConnected(CliprdrClientContext* cliprdr) {
  if (cliprdr == nullptr) {
    return;
  }
  {
    // Publish the channel under the lifecycle lock (readers snapshot it there),
    // and refuse to publish it if teardown has already begun.
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (tearingDown_) {
      return;
    }
    cliprdr_ = cliprdr;
  }
  cliprdr->custom = this;
  cliprdr->MonitorReady = HmrdpCliprdrMonitorReady;
  cliprdr->ServerCapabilities = HmrdpCliprdrServerCapabilities;
  cliprdr->ServerFormatList = HmrdpCliprdrServerFormatList;
  cliprdr->ServerFormatListResponse = HmrdpCliprdrServerFormatListResponse;
  cliprdr->ServerLockClipboardData = HmrdpCliprdrServerLockClipboardData;
  cliprdr->ServerUnlockClipboardData = HmrdpCliprdrServerUnlockClipboardData;
  cliprdr->ServerFormatDataRequest = HmrdpCliprdrServerFormatDataRequest;
  cliprdr->ServerFormatDataResponse = HmrdpCliprdrServerFormatDataResponse;
  cliprdr->ServerFileContentsRequest = HmrdpCliprdrServerFileContentsRequest;
  cliprdr->ServerFileContentsResponse = HmrdpCliprdrServerFileContentsResponse;
  HMRDP_LOGI("cliprdr channel connected");
}

UINT Session::OnCliprdrMonitorReady() {
  if (cliprdr_ == nullptr) {
    return CHANNEL_RC_OK;
  }
  CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilitySet = {};
  generalCapabilitySet.capabilitySetType = CB_CAPSTYPE_GENERAL;
  generalCapabilitySet.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
  generalCapabilitySet.version = CB_CAPS_VERSION_2;
  // File transfer needs CB_STREAM_FILECLIP_ENABLED. Deliberately NOT asking for
  // CB_CAN_LOCK_CLIPDATA: once locking is negotiated the server hands out a
  // clipDataId and every file-contents request must echo it, and one without it
  // is answered with CB_RESPONSE_FAIL. FreeRDP's own file-transfer client leaves
  // the same bit unset (client_cliprdr_file.c, cliprdr_file_context_current_flags).
  // The channel intersects these with the server's capabilities, so a server that
  // does not offer file clip simply keeps them masked out (cliprdr_main.c).
  generalCapabilitySet.generalFlags = CB_USE_LONG_FORMAT_NAMES |
                                      CB_STREAM_FILECLIP_ENABLED |
                                      CB_FILECLIP_NO_FILE_PATHS |
                                      CB_HUGE_FILE_SUPPORT_ENABLED;
  CLIPRDR_CAPABILITIES capabilities = {};
  capabilities.cCapabilitiesSets = 1;
  capabilities.capabilitySets =
      reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&generalCapabilitySet);
  if (cliprdr_->ClientCapabilities != nullptr) {
    cliprdr_->ClientCapabilities(cliprdr_, &capabilities);
  }
  clipboardReady_ = true;
  HMRDP_LOGI("cliprdr monitor ready");
  // Always advertise the formats we support right after MonitorReady, even if
  // the local clipboard is empty: the server needs to know what the client
  // accepts before it offers its own clipboard content.
  SendCapabilityFormatList(cliprdr_);
  return CHANNEL_RC_OK;
}

UINT Session::OnCliprdrServerFormatList(const CLIPRDR_FORMAT_LIST* formatList) {
  if (cliprdr_ == nullptr || formatList == nullptr) {
    return CHANNEL_RC_OK;
  }
  UINT32 dibId = 0;
  UINT32 dibV5Id = 0;
  UINT32 textId = 0;
  UINT32 htmlId = 0;
  UINT32 rtfId = 0;
  UINT32 fileGroupId = 0;
  for (UINT32 i = 0; i < formatList->numFormats; i++) {
    const CLIPRDR_FORMAT& format = formatList->formats[i];
    if (format.formatName != nullptr &&
        strcmp(format.formatName, kFileGroupDescriptorName) == 0) {
      fileGroupId = format.formatId;
    } else if (format.formatName != nullptr && strcmp(format.formatName, kHtmlFormatName) == 0) {
      htmlId = format.formatId;
    } else if (format.formatName != nullptr &&
               strcmp(format.formatName, "Rich Text Format") == 0) {
      rtfId = format.formatId;
    } else if (format.formatId == CF_DIB) {
      dibId = format.formatId;
    } else if (format.formatId == CF_DIBV5) {
      dibV5Id = format.formatId;
    } else if (format.formatId == CF_UNICODETEXT) {
      textId = format.formatId;
    }
  }
  // Any server clipboard change supersedes a download in progress (and may
  // invalidate cached descriptors). ArkTS owns the download state machine, so it
  // is told about every change and aborts its own transfer if one is running.
  Emit(SessionEvent::kRemoteClipboardChanged);
  // Hand the supported candidates to ArkTS, one per kind ("kind:id,kind:id"),
  // which owns the choice: the pull priority, the single-in-flight rule and the
  // deferred refresh after a newer list. Native owns the wire form (so it prefers
  // DIB over DIBV5 here) and does not pick.
  std::string candidates;
  auto addCandidate = [&candidates](int kind, UINT32 id) {
    if (id == 0) {
      return;
    }
    if (!candidates.empty()) {
      candidates.push_back(',');
    }
    candidates += std::to_string(kind);
    candidates.push_back(':');
    candidates += std::to_string(id);
  };
  addCandidate(kClipFiles, fileGroupId);
  addCandidate(kClipImage, dibId != 0 ? dibId : dibV5Id);
  addCandidate(kClipHtml, htmlId);
  addCandidate(kClipRtf, rtfId);
  addCandidate(kClipText, textId);
  if (candidates.empty()) {
    HMRDP_LOGI("cliprdr server format list: no supported format");
    return CHANNEL_RC_OK;
  }
  HMRDP_LOGI("cliprdr server format list: candidates=%{public}s", candidates.c_str());
  Emit(SessionEvent::kRemoteFormatList, candidates);
  return CHANNEL_RC_OK;
}

UINT Session::SendRemoteDataRequest(UINT32 formatId, LocalClipKind kind) {
  if (cliprdr_ == nullptr || cliprdr_->ClientFormatDataRequest == nullptr) {
    return CHANNEL_RC_OK;
  }
  remoteRequestInFlight_ = true;
  remoteRequestKind_ = kind;
  CLIPRDR_FORMAT_DATA_REQUEST request = {};
  request.common.msgType = CB_FORMAT_DATA_REQUEST;
  request.common.msgFlags = 0;
  request.requestedFormatId = formatId;
  HMRDP_LOGI("cliprdr requesting remote format id=%{public}u kind=%{public}d", formatId,
             static_cast<int>(kind));
  return cliprdr_->ClientFormatDataRequest(cliprdr_, &request);
}

bool Session::RequestRemoteFormat(uint32_t formatId, uint32_t kind) {
  // Keep the channel alive while the request is sent on this (UI) thread.
  ContextGuard ctx(*this);
  if (cliprdrLocked() == nullptr) {
    return false;
  }
  return SendRemoteDataRequest(formatId, static_cast<LocalClipKind>(kind)) == CHANNEL_RC_OK;
}

UINT Session::OnCliprdrServerFormatDataRequest(
    const CLIPRDR_FORMAT_DATA_REQUEST* request) {
  if (cliprdr_ == nullptr || request == nullptr ||
      cliprdr_->ClientFormatDataResponse == nullptr) {
    return CHANNEL_RC_OK;
  }
  HMRDP_LOGI("cliprdr server data request: format=%{public}u", request->requestedFormatId);
  // Copy the requested representation out under the lock, then send without
  // holding it. Only the kind of content the user last pushed is served.
  std::vector<BYTE> payload;
  bool servedFiles = false;
  size_t servedFileCount = 0;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    const UINT32 id = request->requestedFormatId;
    // Both ids we advertise for a file clipboard carry the descriptor list:
    // servers echo the id from our FORMAT_LIST, but there is no data format
    // behind FileContents (byte ranges use FILECONTENTS_REQUEST), so accept
    // either instead of depending on which one the peer picked. The descriptor
    // was built by ArkTS (FileTransferCodec) and is served as-is.
    if (localClipKind_ == LocalClipKind::kFiles &&
        (id == kFileGroupDescriptorId || id == kFileContentsId)) {
      payload.assign(localClipboardFileDescriptor_.begin(),
                     localClipboardFileDescriptor_.end());
      servedFiles = !payload.empty();
      servedFileCount = localClipboardFilePaths_.size();
    } else if (id == kHtmlFormatId && localClipKind_ == LocalClipKind::kHtml) {
      payload.assign(localClipboardHtml_.begin(), localClipboardHtml_.end());
    } else if (id == kPngFormatId && localClipKind_ == LocalClipKind::kImage) {
      payload.assign(localClipboardPng_.begin(), localClipboardPng_.end());
    } else if (id == CF_DIB && localClipKind_ == LocalClipKind::kImage) {
      payload.assign(localClipboardDib_.begin(), localClipboardDib_.end());
    } else if (id == CF_UNICODETEXT) {
      if (localClipKind_ == LocalClipKind::kText && localClipboardValid_) {
        payload.assign(localClipboardUtf16_.begin(), localClipboardUtf16_.end());
      } else if (localClipKind_ == LocalClipKind::kHtml) {
        payload.assign(localClipboardUtf16FromHtml_.begin(),
                       localClipboardUtf16FromHtml_.end());
      }
    }
  }
  HMRDP_LOGI("cliprdr server data request: payload=%{public}u", static_cast<unsigned>(payload.size()));
  CLIPRDR_FORMAT_DATA_RESPONSE response = {};
  response.common.msgType = CB_FORMAT_DATA_RESPONSE;
  if (payload.empty()) {
    response.common.msgFlags = CB_RESPONSE_FAIL;
    response.common.dataLen = 0;
    response.requestedFormatData = nullptr;
  } else {
    response.common.msgFlags = CB_RESPONSE_OK;
    response.common.dataLen = static_cast<UINT32>(payload.size());
    response.requestedFormatData = payload.data();
  }
  const UINT rc = cliprdr_->ClientFormatDataResponse(cliprdr_, &response);
  if (servedFiles) {
    uint64_t total = 0;
    {
      std::lock_guard<std::mutex> lock(clipboardMutex_);
      total = localTransferTotal_;
    }
    if (total == 0) {
      // All advertised files are empty, so there is nothing left to stream.
      EmitValues(SessionEvent::kFileTransferDone, {static_cast<int64_t>(servedFileCount)});
      WithdrawLocalFileClipboard();
    }
  }
  return rc;
}

UINT Session::OnCliprdrServerFormatDataResponse(
    const CLIPRDR_FORMAT_DATA_RESPONSE* response) {
  if (!remoteRequestInFlight_) {
    // No request outstanding: ignore stray/duplicate responses instead of
    // mis-decoding them as text.
    return CHANNEL_RC_OK;
  }
  const LocalClipKind kind = remoteRequestKind_;
  remoteRequestInFlight_ = false;
  remoteRequestKind_ = LocalClipKind::kNone;

  const bool failed =
      response == nullptr || (response->common.msgFlags & CB_RESPONSE_FAIL) != 0;
  const BYTE* data = failed ? nullptr : response->requestedFormatData;
  const UINT32 size = failed ? 0 : response->common.dataLen;
  HMRDP_LOGI("cliprdr data response: kind=%{public}d size=%{public}u failed=%{public}d",
             static_cast<int>(kind), size, failed ? 1 : 0);
  if (failed) {
    HMRDP_LOGW("cliprdr data response failed or null");
  } else if (data == nullptr || size == 0) {
    // Nothing to decode; fall through to a possible deferred refresh.
  } else if (kind == LocalClipKind::kFiles) {
    // Hand the raw FILEGROUPDESCRIPTORW to ArkTS, which owns the parse
    // (FileTransferCodec) and the whole download state machine.
    std::vector<uint8_t> descriptor(data, data + size);
    EmitBytes(SessionEvent::kRemoteFileListRaw, descriptor);
  } else if (kind == LocalClipKind::kImage) {
    // Hand the raw CF_DIB to ArkTS, which owns the DIB decode (ClipboardCodec).
    std::vector<uint8_t> dib(data, data + size);
    EmitBytes(SessionEvent::kClipboardImage, dib);
  } else if (kind == LocalClipKind::kHtml) {
    // Hand the raw CF_HTML to ArkTS, which owns the parse (ClipboardCodec).
    std::vector<uint8_t> html(data, data + size);
    EmitBytes(SessionEvent::kClipboardHtml, html);
  } else if (kind == LocalClipKind::kRtf) {
    // Hand the raw RTF to ArkTS, which converts it (ClipboardCodec.parseRtf).
    std::vector<uint8_t> rtf(data, data + size);
    EmitBytes(SessionEvent::kClipboardRtf, rtf);
  } else if (kind == LocalClipKind::kText && size >= sizeof(WCHAR)) {
    // CF_UNICODETEXT is a NUL-terminated UTF-16LE string; make sure the buffer
    // handed to the converter is terminated even if the server omitted the NUL.
    const size_t wcharCount = (size + sizeof(WCHAR) - 1) / sizeof(WCHAR);
    std::vector<WCHAR> wide(wcharCount + 1, 0);
    memcpy(wide.data(), data, size);
    wide[wcharCount] = 0;
    size_t utf8Size = 0;
    char* utf8 = ConvertWCharToUtf8Alloc(wide.data(), &utf8Size);
    if (utf8 != nullptr) {
      HMRDP_LOGI("cliprdr remote text: %{public}u bytes",
                 static_cast<unsigned>(utf8Size));
      Emit(SessionEvent::kClipboardText, std::string(utf8, utf8Size));
      free(utf8);
    }
  }

  // Always settle, even for an empty/failed response, so ArkTS can issue the
  // request it deferred while this one was in flight.
  EmitValues(SessionEvent::kRemoteFormatResponse,
             {static_cast<int64_t>(kind), failed ? 1 : 0});
  return CHANNEL_RC_OK;
}

// Notifies the server of the local clipboard content after it changed.
void Session::AdvertiseLocalClipboard() {
  CliprdrClientContext* cliprdr = cliprdrLocked();
  if (cliprdr == nullptr || !clipboardReady_.load()) {
    return;
  }
  bool hasPng = false;
  bool hasDib = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    hasPng = !localClipboardPng_.empty();
    hasDib = !localClipboardDib_.empty();
  }
  SendLocalFormatList(cliprdr, static_cast<int>(localClipKind_), hasPng, hasDib);
}

void Session::SetLocalClipboardText(const std::string& utf8) {
  // Keep the cliprdr channel alive through the advertise below (see
  // AdvertiseLocalClipboard); a dead session just updates local state.
  ContextGuard ctx(*this);
  size_t wcharCount = 0;
  WCHAR* wide = ConvertUtf8ToWCharAlloc(utf8.c_str(), &wcharCount);
  if (wide == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    localClipboardUtf16_.assign(reinterpret_cast<const char*>(wide),
                                (wcharCount + 1) * sizeof(WCHAR));
    localClipboardValid_ = true;
    localClipboardHtml_.clear();
    localClipboardUtf16FromHtml_.clear();
    localClipboardDib_.clear();
    localClipboardPng_.clear();
    localClipKind_ = LocalClipKind::kText;
  }
  free(wide);
  HMRDP_LOGI("set local clipboard text: %{public}u bytes",
             static_cast<unsigned>(utf8.size()));
  AdvertiseLocalClipboard();
}

void Session::SetLocalClipboardHtmlRaw(const uint8_t* cfHtml, size_t byteCount,
                                       const std::string& plainText) {
  ContextGuard ctx(*this);
  if (cfHtml == nullptr || byteCount == 0) {
    return;
  }
  // The plain-text fallback is the CF_UNICODETEXT half of the same clipboard.
  size_t wcharCount = 0;
  WCHAR* wide = ConvertUtf8ToWCharAlloc(plainText.c_str(), &wcharCount);
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    localClipboardHtml_.assign(reinterpret_cast<const char*>(cfHtml), byteCount);
    if (wide != nullptr) {
      localClipboardUtf16FromHtml_.assign(reinterpret_cast<const char*>(wide),
                                          (wcharCount + 1) * sizeof(WCHAR));
    } else {
      localClipboardUtf16FromHtml_.clear();
    }
    localClipboardUtf16_.clear();
    localClipboardValid_ = false;
    localClipboardDib_.clear();
    localClipboardPng_.clear();
    localClipKind_ = LocalClipKind::kHtml;
  }
  if (wide != nullptr) {
    free(wide);
  }
  HMRDP_LOGI("set local clipboard html: CF_HTML %{public}u bytes",
             static_cast<unsigned>(byteCount));
  AdvertiseLocalClipboard();
}

void Session::SetLocalClipboardImage(const uint8_t* dib, size_t dibLen,
                                     const uint8_t* png, size_t pngLen) {
  ContextGuard ctx(*this);
  const bool hasDib = dib != nullptr && dibLen > 0;
  const bool hasPng = png != nullptr && pngLen > 0;
  if (!hasDib && !hasPng) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (hasDib) {
      localClipboardDib_.assign(reinterpret_cast<const char*>(dib), dibLen);
    } else {
      localClipboardDib_.clear();
    localClipboardPng_.clear();
    }
    if (hasPng) {
      localClipboardPng_.assign(reinterpret_cast<const char*>(png), pngLen);
    } else {
      localClipboardPng_.clear();
    }
    localClipboardUtf16_.clear();
    localClipboardValid_ = false;
    localClipboardHtml_.clear();
    localClipboardUtf16FromHtml_.clear();
    localClipKind_ = LocalClipKind::kImage;
  }
  HMRDP_LOGI("set local clipboard image: CF_DIB %{public}u bytes, PNG %{public}u bytes",
             static_cast<unsigned>(dibLen), static_cast<unsigned>(pngLen));
  AdvertiseLocalClipboard();
}

void Session::SetLocalFileOffer(const uint8_t* descriptor, size_t descriptorLen,
                               const std::vector<std::string>& paths) {
  ContextGuard ctx(*this);
  if (descriptor == nullptr || descriptorLen == 0 || paths.empty()) {
    return;
  }
  uint64_t total = 0;
  for (const std::string& path : paths) {
    uint64_t size = 0;
    if (!path.empty() && FileSizeOfPath(path, &size)) {
      total += size;
    }
  }
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    localClipboardFilePaths_ = paths;
    localClipboardFileDescriptor_.assign(reinterpret_cast<const char*>(descriptor),
                                         descriptorLen);
    localClipboardUtf16_.clear();
    localClipboardValid_ = false;
    localClipboardHtml_.clear();
    localClipboardUtf16FromHtml_.clear();
    localClipboardDib_.clear();
    localClipboardPng_.clear();
    localClipKind_ = LocalClipKind::kFiles;
    localTransferDone_ = 0;
    localTransferTotal_ = total;
  }
  HMRDP_LOGI("set local file offer: count=%{public}u descriptor=%{public}u total=%{public}llu",
             static_cast<unsigned>(paths.size()), static_cast<unsigned>(descriptorLen),
             static_cast<unsigned long long>(total));
  AdvertiseLocalClipboard();
  // Tell the UI the size of the upload so it can show a progress bar before the
  // server starts pulling.
  EmitValues(SessionEvent::kFileTransferProgress,
             {1, 0, static_cast<int64_t>(total), 0, static_cast<int64_t>(paths.size())});
}

bool Session::RequestRemoteFileContents(uint32_t listIndex, bool sizeOnly, uint64_t offset,
                                        uint32_t length) {
  // Keep the channel alive while the request is sent on this (UI) thread.
  ContextGuard ctx(*this);
  CliprdrClientContext* cliprdr = cliprdrLocked();
  if (cliprdr == nullptr || cliprdr->ClientFileContentsRequest == nullptr) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (fileContentsPending_) {
      // ArkTS serialises the requests; a second one would be untagged on return.
      return false;
    }
    fileContentsPending_ = true;
    pendingFileContentsIndex_ = listIndex;
    pendingFileContentsSizeOnly_ = sizeOnly;
  }
  CLIPRDR_FILE_CONTENTS_REQUEST request = {};
  request.common.msgType = CB_FILECONTENTS_REQUEST;
  request.streamId = listIndex + 1;
  request.listIndex = listIndex;
  request.haveClipDataId = FALSE;
  if (sizeOnly) {
    request.dwFlags = FILECONTENTS_SIZE;
    request.cbRequested = static_cast<UINT32>(sizeof(uint64_t));
    request.nPositionLow = 0;
    request.nPositionHigh = 0;
  } else {
    request.dwFlags = FILECONTENTS_RANGE;
    request.nPositionLow = static_cast<UINT32>(offset & 0xFFFFFFFFull);
    request.nPositionHigh = static_cast<UINT32>(offset >> 32);
    request.cbRequested = length;
  }
  // One line per file (SIZE / first range), not per chunk.
  if (request.dwFlags == FILECONTENTS_SIZE ||
      (request.nPositionLow == 0 && request.nPositionHigh == 0)) {
    HMRDP_LOGI("cliprdr request file content: index=%{public}u flags=0x%{public}x cb=%{public}u",
               request.listIndex, request.dwFlags, request.cbRequested);
  }
  const UINT rc = cliprdr->ClientFileContentsRequest(cliprdr, &request);
  if (rc != CHANNEL_RC_OK) {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    fileContentsPending_ = false;
  }
  return rc == CHANNEL_RC_OK;
}
void Session::CancelFileTransfer() {
  bool pulled = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (localClipKind_ != LocalClipKind::kFiles) {
      return;
    }
    pulled = localTransferDone_ > 0;
    if (pulled) {
      // Mid-transfer abort: the server is still reading the list, so it cannot
      // be withdrawn; stop serving instead, which makes the remote paste report
      // a protocol error (the UI warns about that before calling this).
      localClipboardFilePaths_.clear();
      localTransferDone_ = 0;
      localTransferTotal_ = 0;
      localClipKind_ = LocalClipKind::kNone;
    }
  }
  if (!pulled) {
    // Nothing was pulled yet (or the transfer already finished): the file list
    // can be taken back cleanly.
    WithdrawLocalFileClipboard();
  }
}

void Session::WithdrawLocalFileClipboard() {
  bool withdrawn = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (localClipKind_ != LocalClipKind::kFiles) {
      return;
    }
    // Replace the file list with an empty text clipboard. The server drops the
    // file formats and a later paste finds nothing instead of erroring (an empty
    // FORMAT_LIST would be ignored by the channel, see cliprdr_main.c).
    localClipboardFilePaths_.clear();
    localTransferDone_ = 0;
    localTransferTotal_ = 0;
    localClipboardUtf16_.assign(sizeof(WCHAR), '\0');
    localClipboardValid_ = true;
    localClipboardHtml_.clear();
    localClipboardUtf16FromHtml_.clear();
    localClipboardDib_.clear();
    localClipboardPng_.clear();
    localClipKind_ = LocalClipKind::kText;
    withdrawn = true;
  }
  if (withdrawn) {
    HMRDP_LOGI("cliprdr file list withdrawn (empty text advertised)");
    AdvertiseLocalClipboard();
  }
}

UINT Session::HandleRemoteFileContentsResponse(
    const CLIPRDR_FILE_CONTENTS_RESPONSE* response) {
  if (response == nullptr) {
    return CHANNEL_RC_OK;
  }
  uint32_t listIndex = 0;
  bool sizeOnly = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (!fileContentsPending_) {
      // No request outstanding: ignore a stray/duplicate response.
      return CHANNEL_RC_OK;
    }
    fileContentsPending_ = false;
    listIndex = pendingFileContentsIndex_;
    sizeOnly = pendingFileContentsSizeOnly_;
  }
  // Fields: listIndex, isSize, sizeValue, failed. A range response carries its
  // bytes in the byte buffer (empty for an end-of-file marker).
  const bool failed = (response->common.msgFlags & CB_RESPONSE_FAIL) != 0;
  if (failed) {
    HMRDP_LOGW("cliprdr file contents response failed: index=%{public}u", listIndex);
    EmitValues(SessionEvent::kRemoteFileChunk, {listIndex, sizeOnly ? 1 : 0, 0, 1});
    return CHANNEL_RC_OK;
  }
  if (sizeOnly) {
    if (response->requestedData == nullptr || response->cbRequested != sizeof(uint64_t)) {
      EmitValues(SessionEvent::kRemoteFileChunk, {listIndex, 1, 0, 1});
      return CHANNEL_RC_OK;
    }
    uint64_t size = 0;
    memcpy(&size, response->requestedData, sizeof(size));
    EmitValues(SessionEvent::kRemoteFileChunk,
               {listIndex, 1, static_cast<int64_t>(size), 0});
    return CHANNEL_RC_OK;
  }
  std::vector<uint8_t> data;
  if (response->requestedData != nullptr && response->cbRequested > 0) {
    data.assign(response->requestedData, response->requestedData + response->cbRequested);
  }
  EmitValues(SessionEvent::kRemoteFileChunk, {listIndex, 0, 0, 0}, data);
  return CHANNEL_RC_OK;
}
UINT Session::HandleLocalFileContentsRequest(
    const CLIPRDR_FILE_CONTENTS_REQUEST* request) {
  if (cliprdr_ == nullptr || request == nullptr ||
      cliprdr_->ClientFileContentsResponse == nullptr) {
    return CHANNEL_RC_OK;
  }
  std::string path;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (localClipKind_ != LocalClipKind::kFiles ||
        request->listIndex >= localClipboardFilePaths_.size()) {
      HMRDP_LOGW("cliprdr file request for unknown index %{public}u", request->listIndex);
    } else {
      path = localClipboardFilePaths_[request->listIndex];
    }
  }
  std::vector<uint8_t> data;
  bool ok = !path.empty();
  if (ok && (request->dwFlags & FILECONTENTS_SIZE) != 0) {
    uint64_t size = 0;
    if (FileSizeOfPath(path, &size)) {
      data.resize(sizeof(uint64_t));
      memcpy(data.data(), &size, sizeof(size));
      HMRDP_LOGI("cliprdr serve size: index=%{public}u size=%{public}llu",
                 request->listIndex, static_cast<unsigned long long>(size));
    } else {
      ok = false;
    }
  } else if (ok && (request->dwFlags & FILECONTENTS_RANGE) != 0) {
    const uint64_t position =
        (static_cast<uint64_t>(request->nPositionHigh) << 32) | request->nPositionLow;
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
      ok = false;
    } else {
      if (fseeko(file, static_cast<off_t>(position), SEEK_SET) == 0) {
        data.resize(request->cbRequested);
        const size_t readSize =
            data.empty() ? 0 : fread(data.data(), 1, data.size(), file);
        data.resize(readSize);
        if (request->nPositionLow == 0 && request->nPositionHigh == 0) {
          HMRDP_LOGI("cliprdr serve range: index=%{public}u cb=%{public}u",
                     request->listIndex, request->cbRequested);
        }
      } else {
        ok = false;
      }
      fclose(file);
    }
  } else if (ok) {
    ok = false;
  }
  if (!ok) {
    HMRDP_LOGW("cliprdr serve failed: index=%{public}u flags=0x%{public}x known=%{public}d",
               request->listIndex, request->dwFlags, path.empty() ? 0 : 1);
  }
  CLIPRDR_FILE_CONTENTS_RESPONSE response = {};
  response.common.msgType = CB_FILECONTENTS_RESPONSE;
  response.streamId = request->streamId;
  if (!ok) {
    response.common.msgFlags = CB_RESPONSE_FAIL;
    response.cbRequested = 0;
    response.requestedData = nullptr;
  } else {
    response.common.msgFlags = CB_RESPONSE_OK;
    response.cbRequested = static_cast<UINT32>(data.size());
    response.requestedData = data.empty() ? nullptr : data.data();
  }
  uint64_t done = 0;
  uint64_t total = 0;
  uint32_t fileCount = 0;
  bool uploadDone = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    // Only the range responses move the upload forward; a size response is
    // bookkeeping and must not be counted (or `done` would overshoot `total`).
    if ((request->dwFlags & FILECONTENTS_RANGE) != 0) {
      localTransferDone_ += response.cbRequested;
    }
    done = localTransferDone_;
    total = localTransferTotal_;
    fileCount = static_cast<uint32_t>(localClipboardFilePaths_.size());
    uploadDone = ok && (request->dwFlags & FILECONTENTS_RANGE) != 0 && done >= total;
  }
  const UINT rc = cliprdr_->ClientFileContentsResponse(cliprdr_, &response);
  EmitValues(SessionEvent::kFileTransferProgress,
             {1, static_cast<int64_t>(done), static_cast<int64_t>(total),
              static_cast<int64_t>(request->listIndex) + 1, static_cast<int64_t>(fileCount)});
  if (uploadDone) {
    EmitValues(SessionEvent::kFileTransferDone, {static_cast<int64_t>(fileCount)});
    // Every advertised byte is served, so the list has no reason to stay on the
    // clipboard: withdrawing it keeps a second paste on the remote side from
    // pulling the same files again (with no progress row to cancel).
    WithdrawLocalFileClipboard();
  }
  return rc;
}

}  // namespace hmrdp
