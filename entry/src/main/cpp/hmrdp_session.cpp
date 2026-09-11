/*
 * HmRdp - HarmonyOS RDP client
 * FreeRDP client session implementation.
 */
#include "hmrdp_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include <freerdp/channels/channels.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/codec/color.h>
#include <freerdp/gdi/gdi.h>
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

// The rdpsnd backend is replaced on OHOS (see native/patches/rdpsnd_opensles.c):
// instead of opening an OpenSL ES device it hands decoded 16-bit PCM to a sink
// registered through HmrdpSetAudioSink. The sink routes the buffer to the
// owning Session, which forwards it over the Node-API bridge to ArkTS.
using HmrdpAudioSink = void (*)(void* context, const void* data, size_t size,
                                int sampleRate, int channels);

extern "C" void HmrdpSetAudioSink(HmrdpAudioSink sink);

// Defined by the patched rdpei client (native/scripts/patch-freerdp.ps1). The
// weak reference keeps libhmrdp linkable against stock FreeRDP, where touch
// high-rate simply stays unavailable.
extern "C" void HmrdpSetTouchFrameInterval(UINT32 intervalMs) __attribute__((weak));

namespace hmrdp {
namespace {

// Whether remote cursor updates drive the HarmonyOS system cursor. Global (like
// the touch frame interval) because it is a user setting, and read when a
// session connects (HmrdpPostConnect).
std::atomic<bool> g_useRdpCursor{true};

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

// Standard base64 (no line breaks). Cursor bitmaps are small, and base64 keeps
// the Node-API event payload free of NUL bytes / invalid UTF-8 so it survives
// napi_create_string_utf8 intact.
std::string Base64Encode(const uint8_t* data, size_t len) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back(kTable[n & 0x3F]);
    i += 3;
  }
  if (i + 1 == len) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == len) {
    const uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(kTable[(n >> 18) & 0x3F]);
    out.push_back(kTable[(n >> 12) & 0x3F]);
    out.push_back(kTable[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

// Local clipboard content kinds; must match Session::LocalClipKind.
constexpr int kClipNone = 0;
constexpr int kClipText = 1;
constexpr int kClipHtml = 2;
constexpr int kClipImage = 3;

// Client-side id and name for the registered "HTML Format" clipboard format.
// Registered ids start at 0xC000 (WinPR convention); with CB_USE_LONG_FORMAT_NAMES
// the server maps it by name.
constexpr UINT32 kHtmlFormatId = 0xC000;
const char kHtmlFormatName[] = "HTML Format";

// Builds a CF_HTML payload: an ASCII header of byte offsets followed by the HTML
// fragment. See "HTML Clipboard Format" (MSDN). The offset fields are
// fixed-width, so the header length is constant and offsets can be computed up
// front.
std::string BuildCfHtml(const std::string& html) {
  static const char kBodyStart[] = "<html><body>";
  static const char kBodyEnd[] = "</body></html>";
  static const char kStartFragment[] = "<!--StartFragment-->";
  static const char kEndFragment[] = "<!--EndFragment-->";
  const size_t headerLen = std::strlen("Version:0.9\r\n") +
                           std::strlen("StartHTML:0000000000\r\n") +
                           std::strlen("EndHTML:0000000000\r\n") +
                           std::strlen("StartFragment:0000000000\r\n") +
                           std::strlen("EndFragment:0000000000\r\n");
  const std::string body =
      std::string(kBodyStart) + kStartFragment + html + kEndFragment + kBodyEnd;
  const size_t startHtml = headerLen;
  const size_t endHtml = headerLen + body.size();
  const size_t startFragment =
      headerLen + std::strlen(kBodyStart) + std::strlen(kStartFragment);
  const size_t endFragment = startFragment + html.size();
  char header[128] = {0};
  std::snprintf(header, sizeof(header),
                "Version:0.9\r\n"
                "StartHTML:%010zu\r\n"
                "EndHTML:%010zu\r\n"
                "StartFragment:%010zu\r\n"
                "EndFragment:%010zu\r\n",
                startHtml, endHtml, startFragment, endFragment);
  return std::string(header) + body;
}

// Extracts the HTML fragment from a CF_HTML payload, falling back to the whole
// payload when the fragment offsets are missing.
std::string ParseCfHtml(const char* data, size_t size) {
  const std::string text(data, size);
  const size_t headerLimit = size < 512 ? size : 512;
  const std::string header = text.substr(0, headerLimit);
  auto readOffset = [&header](const char* key) -> long {
    const size_t pos = header.find(key);
    if (pos == std::string::npos) {
      return -1;
    }
    size_t begin = pos + std::strlen(key);
    while (begin < header.size() && header[begin] == ' ') {
      ++begin;
    }
    size_t end = begin;
    while (end < header.size() && header[end] >= '0' && header[end] <= '9') {
      ++end;
    }
    if (end == begin) {
      return -1;
    }
    return std::strtol(header.substr(begin, end - begin).c_str(), nullptr, 10);
  };
  const long startFragment = readOffset("StartFragment:");
  const long endFragment = readOffset("EndFragment:");
  if (startFragment >= 0 && endFragment > startFragment &&
      static_cast<size_t>(endFragment) <= size) {
    return text.substr(static_cast<size_t>(startFragment),
                       static_cast<size_t>(endFragment - startFragment));
  }
  const long startHtml = readOffset("StartHTML:");
  if (startHtml >= 0 && static_cast<size_t>(startHtml) < size) {
    const long endHtml = readOffset("EndHTML:");
    const size_t end = (endHtml > startHtml && static_cast<size_t>(endHtml) <= size)
                           ? static_cast<size_t>(endHtml)
                           : size;
    return text.substr(static_cast<size_t>(startHtml),
                       end - static_cast<size_t>(startHtml));
  }
  return text;
}

// Minimal HTML -> plain text fallback for the CF_UNICODETEXT half of an HTML
// push. Not a renderer: it only needs to be readable in a plain text target.
std::string StripHtmlToText(const std::string& html) {
  std::string out;
  for (size_t i = 0; i < html.size(); ++i) {
    if (html[i] != '<') {
      out.push_back(html[i]);
      continue;
    }
    const size_t close = html.find('>', i);
    if (close == std::string::npos) {
      break;
    }
    const std::string tag = html.substr(i + 1, close - i - 1);
    const char first = tag.empty() ? '\0' : static_cast<char>(tag[0] | 0x20);
    if (first == 'b' && tag.size() >= 2 && (tag[1] == 'r' || tag[1] == 'R')) {
      out.push_back('\n');
    } else if (first == 'p' || first == 'd' || first == 'l' || first == 't') {
      out.push_back('\n');
    }
    i = close;
  }
  struct Entity {
    const char* code;
    char ch;
  };
  static const Entity kEntities[] = {
      {"&amp;", '&'},   {"&lt;", '<'},   {"&gt;", '>'},
      {"&quot;", '"'},  {"&#39;", '\''}, {"&nbsp;", ' '},
  };
  for (const Entity& entity : kEntities) {
    const size_t len = std::strlen(entity.code);
    size_t pos = 0;
    while ((pos = out.find(entity.code, pos)) != std::string::npos) {
      out.replace(pos, len, 1, entity.ch);
      ++pos;
    }
  }
  return out;
}

uint16_t ReadDibU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadDibU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct DibImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> bgra;
};

// Parses a CF_DIB (BITMAPINFOHEADER + BI_RGB, 24 or 32bpp) into top-down BGRA.
bool ParseDibToBgra(const uint8_t* data, size_t size, DibImage* out) {
  if (data == nullptr || out == nullptr || size < 40) {
    return false;
  }
  const uint32_t headerSize = ReadDibU32(data + 0);
  const int32_t width = static_cast<int32_t>(ReadDibU32(data + 4));
  const int32_t heightRaw = static_cast<int32_t>(ReadDibU32(data + 8));
  const uint16_t bpp = ReadDibU16(data + 14);
  const uint32_t compression = ReadDibU32(data + 16);
  if (headerSize < 40 || headerSize > size || width <= 0 || heightRaw == 0) {
    return false;
  }
  // BI_RGB only; bit-fields / palette layouts are not handled (rare from a
  // normal Windows copy, and keeping the parser simple avoids wrong pixels).
  if ((bpp != 24 && bpp != 32) || compression != 0) {
    return false;
  }
  const uint64_t w = static_cast<uint64_t>(width);
  const uint64_t h = heightRaw < 0
                         ? static_cast<uint64_t>(-static_cast<int64_t>(heightRaw))
                         : static_cast<uint64_t>(heightRaw);
  if (w == 0 || h == 0 || w > 16384 || h > 16384) {
    return false;
  }
  const bool topDown = heightRaw < 0;
  const size_t stride = static_cast<size_t>(((w * bpp + 31) / 32) * 4);
  if (static_cast<size_t>(headerSize) + stride * static_cast<size_t>(h) > size) {
    return false;
  }
  out->width = static_cast<uint32_t>(w);
  out->height = static_cast<uint32_t>(h);
  out->bgra.assign(static_cast<size_t>(w * h * 4), 0);
  for (uint64_t y = 0; y < h; ++y) {
    const uint64_t srcY = topDown ? y : (h - 1 - y);
    const uint8_t* row = data + headerSize + static_cast<size_t>(srcY) * stride;
    uint8_t* dst = out->bgra.data() + static_cast<size_t>(y * w * 4);
    for (uint64_t x = 0; x < w; ++x) {
      const uint8_t* px = row + static_cast<size_t>(x) * (bpp / 8);
      uint8_t alpha = (bpp == 32) ? px[3] : 255;
      if (alpha == 0) {
        alpha = 255;  // BI_RGB carries no alpha; 0 means opaque.
      }
      dst[x * 4 + 0] = px[0];
      dst[x * 4 + 1] = px[1];
      dst[x * 4 + 2] = px[2];
      dst[x * 4 + 3] = alpha;
    }
  }
  return true;
}

// Converts a HarmonyOS PixelMap buffer to BGRA. `pixelFormat` is
// image.PixelMapFormat: 1 ARGB_8888, 3 RGBA_8888, 4 BGRA_8888, 5 RGB_888.
bool ConvertToBgra(const uint8_t* pixels, size_t byteCount, uint32_t width,
                   uint32_t height, int32_t pixelFormat, uint8_t* out) {
  if (pixels == nullptr || out == nullptr || width == 0 || height == 0) {
    return false;
  }
  const size_t count = static_cast<size_t>(width) * height;
  if (pixelFormat == 3) {  // RGBA_8888
    if (byteCount < count * 4) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* src = pixels + i * 4;
      uint8_t* dst = out + i * 4;
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
    }
    return true;
  }
  if (pixelFormat == 1 || pixelFormat == 4) {  // ARGB_8888 / BGRA_8888
    if (byteCount < count * 4) {
      return false;
    }
    std::memcpy(out, pixels, count * 4);
    return true;
  }
  if (pixelFormat == 5) {  // RGB_888
    if (byteCount < count * 3) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* src = pixels + i * 3;
      uint8_t* dst = out + i * 4;
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = 255;
    }
    return true;
  }
  return false;
}

// Serialises top-down BGRA into a CF_DIB payload (BITMAPINFOHEADER, 32bpp,
// BI_RGB, bottom-up rows).
std::vector<uint8_t> BuildBgraToDib(const uint8_t* bgra, uint32_t width,
                                    uint32_t height) {
  const size_t stride = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> dib(40 + stride * height, 0);
  auto writeU16 = [&dib](size_t offset, uint16_t value) {
    dib[offset] = static_cast<uint8_t>(value & 0xFF);
    dib[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  };
  auto writeU32 = [&dib](size_t offset, uint32_t value) {
    dib[offset] = static_cast<uint8_t>(value & 0xFF);
    dib[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dib[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dib[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
  };
  writeU32(0, 40);
  writeU32(4, width);
  writeU32(8, height);
  writeU16(12, 1);
  writeU16(14, 32);
  writeU32(16, 0);
  writeU32(20, static_cast<uint32_t>(stride * height));
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(dib.data() + 40 + static_cast<size_t>(y) * stride,
                bgra + static_cast<size_t>(height - 1 - y) * stride, stride);
  }
  return dib;
}

// FNV-1a over the cursor geometry and pixels, used to drop redundant updates.
uint32_t CursorHash(uint32_t width, uint32_t height, uint32_t hotX, uint32_t hotY,
                    const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261u;
  const uint32_t header[4] = {width, height, hotX, hotY};
  for (uint32_t value : header) {
    for (int i = 0; i < 4; i++) {
      hash ^= (value >> (i * 8)) & 0xFF;
      hash *= 16777619u;
    }
  }
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

// HarmonyOS custom cursors cap at 256x256 (see setCustomCursor). Larger RDP
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

constexpr uint32_t kMaxCursorSide = 256;
// Hard cap on the source cursor size; anything beyond this is treated as
// corrupt and dropped rather than allocated.
constexpr uint32_t kMaxCursorSourceSide = 1024;

typedef struct {
  rdpClientContext common;
  Session* session;
} HmrdpContext;

BOOL HmrdpBeginPaint(rdpContext* context) {
  if (context == nullptr || context->gdi == nullptr || context->gdi->primary == nullptr) {
    return TRUE;
  }
  HGDI_WND hwnd = context->gdi->primary->hdc->hwnd;
  if (hwnd != nullptr && hwnd->invalid != nullptr) {
    hwnd->invalid->null = TRUE;
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

void HmrdpAudioSinkAdapter(void* context, const void* data, size_t size, int sampleRate,
                           int channels) {
  if (context == nullptr || data == nullptr || size == 0) {
    return;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(static_cast<rdpContext*>(context));
  if (ctx->session != nullptr) {
    ctx->session->OnAudioData(data, size, sampleRate, channels);
  }
}

// Announces every clipboard format the client understands. Sent once after
// MonitorReady so the server knows it can offer text / HTML / images; the
// concrete content advertisements follow when the user pushes something.
UINT SendCapabilityFormatList(CliprdrClientContext* cliprdr) {
  if (cliprdr == nullptr || cliprdr->ClientFormatList == nullptr) {
    return CHANNEL_RC_OK;
  }
  CLIPRDR_FORMAT formats[3] = {};
  formats[0].formatId = CF_UNICODETEXT;
  formats[1].formatId = CF_DIB;
  formats[2].formatId = kHtmlFormatId;
  formats[2].formatName = const_cast<char*>(kHtmlFormatName);
  CLIPRDR_FORMAT_LIST formatList = {};
  formatList.common.msgType = CB_FORMAT_LIST;
  formatList.common.msgFlags = 0;
  formatList.numFormats = 3;
  formatList.formats = formats;
  HMRDP_LOGI("cliprdr sending capability format list (text/dib/html)");
  return cliprdr->ClientFormatList(cliprdr, &formatList);
}

// Advertises the concrete content of the local clipboard for `kind`. The
// channel owns the ClientFormatList sender, so this only builds the list.
UINT SendLocalFormatList(CliprdrClientContext* cliprdr, int kind) {
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
  } else if (kind == kClipImage) {
    formats[count].formatId = CF_DIB;
    ++count;
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
                                    const CLIPRDR_CAPABILITIES*) {
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
  HMRDP_LOGI("preconnect: clipboard=%{public}d gfx=%{public}d",
             freerdp_settings_get_bool(settings, FreeRDP_RedirectClipboard) ? 1 : 0,
             freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline) ? 1 : 0);

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
  if (!gdi_init(instance, PIXEL_FORMAT_BGRA32)) {
    return FALSE;
  }
  rdpContext* context = instance->context;
  context->update->BeginPaint = HmrdpBeginPaint;
  context->update->EndPaint = HmrdpEndPaint;
  context->update->DesktopResize = HmrdpDesktopResize;
  context->update->PlaySound = HmrdpPlaySound;

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
    ctx->session->HandlePostConnect();
  }
  return TRUE;
}

void HmrdpPostDisconnect(freerdp* instance) {
  if (instance == nullptr || instance->context == nullptr) {
    return;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(instance->context);
  if (ctx->session != nullptr) {
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

// Error events carry "<code>|<message>"; the code lets the UI distinguish an
// authentication failure from a network problem. Internal errors use code 0.
std::string EncodeError(uint32_t code, const std::string& message) {
  if (code == 0) {
    return message;
  }
  std::ostringstream out;
  out << code << '|' << message;
  return out.str();
}

}  // namespace

Session::Session() {
  EnsureEntryPoints();
}

Session::~Session() {
  Disconnect();
}

void Session::Emit(SessionEvent event, const std::string& data) {
  if (eventFn_) {
    eventFn_(event, data);
  }
}

void Session::SetError(const std::string& error) {
  SetError(0, error);
}

void Session::SetError(uint32_t code, const std::string& error) {
  lastErrorCode_ = code;
  lastError_ = error;
  Emit(SessionEvent::kError, EncodeError(code, error));
}

bool Session::Connect(const RdpOptions& options) {
  if (running_.load()) {
    SetError("session already running");
    return false;
  }
  lastError_.clear();
  firstFrameSent_ = false;
  clipboardEnabled_ = options.enableClipboard;

  rdpContext* context = freerdp_client_context_new(&g_entryPoints);
  if (context == nullptr) {
    SetError("freerdp_client_context_new failed");
    return false;
  }
  HmrdpContext* ctx = reinterpret_cast<HmrdpContext*>(context);
  ctx->session = this;
  instance_ = context->instance;

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
  AppendArg(args, "/w:" + std::to_string(options.width));
  AppendArg(args, "/h:" + std::to_string(options.height));
  AppendArg(args, "/bpp:" + std::to_string(options.colorDepth));
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
  freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, options.enableClipboard);
  // Audio is only requested when the device actually provides an audio output;
  // otherwise the channel is left off so an unsupported device degrades to a
  // silent session instead of failing.
  const bool audioSupported = AudioOutput::Supported();
  if (options.enableAudio && !audioSupported) {
    HMRDP_LOGW("audio: requested but unsupported on this device, disabling");
  }
  freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback,
                            options.enableAudio && audioSupported);
  freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, options.enableGfx);
  freerdp_settings_set_bool(settings, FreeRDP_GfxH264, options.enableH264);
  freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, options.enableRemoteFx);
  if (options.performanceFlags != 0) {
    freerdp_settings_set_uint32(settings, FreeRDP_PerformanceFlags,
                                static_cast<uint32_t>(options.performanceFlags));
  }
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
    return false;
  }

  running_ = true;
  stopRequested_ = false;
  thread_ = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
    Session* session = static_cast<Session*>(param);
    session->EventThread();
    return 0;
  }, this, 0, nullptr);
  if (thread_ == nullptr) {
    running_ = false;
    SetError("failed to create session thread");
    freerdp_client_stop(context);
    freerdp_client_context_free(context);
    instance_ = nullptr;
    return false;
  }
  return true;
}

void Session::EventThread() {
  freerdp* instance = instance_;
  if (instance == nullptr) {
    return;
  }
  const BOOL ok = freerdp_connect(instance);
  if (!ok) {
    const UINT32 code = static_cast<UINT32>(freerdp_get_last_error(instance->context));
    const char* error = freerdp_get_last_error_string(code);
    SetError(code, error != nullptr ? error : "connection failed");
  }

  if (ok) {
    HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {0};
    while (!stopRequested_.load() &&
           !freerdp_shall_disconnect_context(instance->context)) {
      const DWORD count = freerdp_get_event_handles(instance->context, handles,
                                                    MAXIMUM_WAIT_OBJECTS);
      if (count == 0) {
        SetError("freerdp_get_event_handles failed");
        break;
      }
      const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
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
    }
  }

  freerdp_disconnect(instance);
  Emit(SessionEvent::kDisconnected, EncodeError(lastErrorCode_, lastError_));
}

void Session::Disconnect() {
  if (!running_.load() && instance_ == nullptr) {
    return;
  }
  stopRequested_ = true;
  if (instance_ != nullptr && instance_->context != nullptr) {
    freerdp_abort_connect_context(instance_->context);
  }
  if (thread_ != nullptr) {
    WaitForSingleObject(static_cast<HANDLE>(thread_), 5000);
    CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
  }
  if (instance_ != nullptr && instance_->context != nullptr) {
    if (instance_->context->settings != nullptr) {
      freerdp_settings_set_string(instance_->context->settings, FreeRDP_Password, nullptr);
      freerdp_settings_set_string(instance_->context->settings, FreeRDP_GatewayPassword,
                                  nullptr);
    }
    freerdp_client_stop(instance_->context);
    freerdp_client_context_free(instance_->context);
  }
  instance_ = nullptr;
  // The channel interface dies with the context; drop it before it can be used
  // from the UI thread.
  cliprdr_ = nullptr;
  clipboardReady_ = false;
  running_ = false;
  audio_.Close();
  renderer_.Reset();
}

void Session::HandlePostConnect() {
  rdpSettings* settings = instance_->context->settings;
  const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  renderer_.SetDesktopSize(static_cast<int>(width), static_cast<int>(height));
  Emit(SessionEvent::kConnected, "");
}

void Session::HandleEndPaint() {
  rdpGdi* gdi = instance_->context->gdi;
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
  hwnd->invalid->null = TRUE;

  renderer_.DrawFrame(gdi->primary_buffer, gdi->stride, x, y, width, height);

  if (!firstFrameSent_) {
    firstFrameSent_ = true;
    Emit(SessionEvent::kFirstFrame, "");
  }
}

void Session::HandleDesktopResize() {
  rdpSettings* settings = instance_->context->settings;
  const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  renderer_.SetDesktopSize(static_cast<int>(width), static_cast<int>(height));
  std::ostringstream payload;
  payload << width << "x" << height;
  Emit(SessionEvent::kResize, payload.str());
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
  // Custom cursors cap at 256x256, so bigger ones (large pointer) are scaled to
  // fit -- hot spot included -- instead of falling back to the default arrow.
  uint32_t outW = width;
  uint32_t outH = height;
  uint32_t outHotX = hotX;
  uint32_t outHotY = hotY;
  if (width > kMaxCursorSide || height > kMaxCursorSide) {
    const uint32_t maxSide = width > height ? width : height;
    const double factor = static_cast<double>(kMaxCursorSide) / maxSide;
    outW = static_cast<uint32_t>(static_cast<double>(width) * factor + 0.5);
    outH = static_cast<uint32_t>(static_cast<double>(height) * factor + 0.5);
    if (outW < 1) {
      outW = 1;
    }
    if (outH < 1) {
      outH = 1;
    }
    outHotX = static_cast<uint32_t>(static_cast<double>(hotX) * factor + 0.5);
    outHotY = static_cast<uint32_t>(static_cast<double>(hotY) * factor + 0.5);
    if (outHotX > outW) {
      outHotX = outW;
    }
    if (outHotY > outH) {
      outHotY = outH;
    }
    std::vector<uint8_t> scaled;
    DownscaleBgra(bgra, width, height, outW, outH, &scaled);
    bgra.swap(scaled);
  }
  const uint32_t hash = CursorHash(outW, outH, outHotX, outHotY, bgra.data(), bgra.size());
  if (cursorHashValid_ && hash == cursorHash_) {
    return;
  }
  cursorHash_ = hash;
  cursorHashValid_ = true;
  std::ostringstream payload;
  payload << outW << ',' << outH << ',' << outHotX << ',' << outHotY << '|'
          << Base64Encode(bgra.data(), bgra.size());
  Emit(SessionEvent::kCursorShape, payload.str());
}

void Session::HandlePointerDefault() {
  cursorHashValid_ = false;
  Emit(SessionEvent::kCursorDefault, "");
}

void Session::HandlePointerHidden() {
  cursorHashValid_ = false;
  Emit(SessionEvent::kCursorHidden, "");
}

void Session::HandlePostDisconnect() {
  clipboardReady_ = false;
  cliprdr_ = nullptr;
  remoteHtmlFormatId_ = 0;
  pendingRemoteKind_ = LocalClipKind::kNone;
}

void Session::OnAudioData(const void* data, size_t size, int sampleRate, int channels) {
  if (data != nullptr && size > 0) {
    audio_.Write(data, size, sampleRate, channels);
  }
}

bool Session::SendMouse(uint16_t flags, uint16_t x, uint16_t y) {
  static int mouseLog = 0;
  if (mouseLog < 20) {
    HMRDP_LOGI("sendMouse flags=0x%{public}x x=%{public}u y=%{public}u", flags, x, y);
    mouseLog++;
  }
  if (instance_ == nullptr || instance_->context == nullptr ||
      instance_->context->input == nullptr) {
    return false;
  }
  return freerdp_input_send_mouse_event(instance_->context->input, flags, x, y);
}

bool Session::SendTouch(uint32_t flags, int32_t finger, uint32_t pressure, int32_t x,
                        int32_t y) {
  if (instance_ == nullptr || instance_->context == nullptr) {
    return false;
  }
  rdpClientContext* client = reinterpret_cast<rdpClientContext*>(instance_->context);
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
    // 0 flushes every frame; 20 is FreeRDP's upstream 50Hz coalescing.
    HmrdpSetTouchFrameInterval(enabled ? 0u : 20u);
  }
}

void Session::SetRdpCursor(bool enabled) {
  g_useRdpCursor.store(enabled);
}

bool Session::SendKey(uint8_t scancode, bool down, bool extended) {
  if (instance_ == nullptr || instance_->context == nullptr ||
      instance_->context->input == nullptr) {
    return false;
  }
  UINT16 flags = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;
  if (extended) {
    flags |= KBD_FLAGS_EXTENDED;
  }
  return freerdp_input_send_keyboard_event(instance_->context->input, flags, scancode);
}

bool Session::SendUnicode(uint16_t codepoint, bool down) {
  if (instance_ == nullptr || instance_->context == nullptr ||
      instance_->context->input == nullptr) {
    return false;
  }
  UINT16 flags = down ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE;
  return freerdp_input_send_unicode_keyboard_event(instance_->context->input, flags,
                                                   codepoint);
}

void Session::HandleCliprdrConnected(CliprdrClientContext* cliprdr) {
  if (cliprdr == nullptr || !clipboardEnabled_.load()) {
    return;
  }
  cliprdr_ = cliprdr;
  cliprdr->custom = this;
  cliprdr->MonitorReady = HmrdpCliprdrMonitorReady;
  cliprdr->ServerCapabilities = HmrdpCliprdrServerCapabilities;
  cliprdr->ServerFormatList = HmrdpCliprdrServerFormatList;
  cliprdr->ServerFormatListResponse = HmrdpCliprdrServerFormatListResponse;
  cliprdr->ServerLockClipboardData = HmrdpCliprdrServerLockClipboardData;
  cliprdr->ServerUnlockClipboardData = HmrdpCliprdrServerUnlockClipboardData;
  cliprdr->ServerFormatDataRequest = HmrdpCliprdrServerFormatDataRequest;
  cliprdr->ServerFormatDataResponse = HmrdpCliprdrServerFormatDataResponse;
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
  generalCapabilitySet.generalFlags = CB_USE_LONG_FORMAT_NAMES;
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
  if (cliprdr_ == nullptr || formatList == nullptr ||
      cliprdr_->ClientFormatDataRequest == nullptr) {
    return CHANNEL_RC_OK;
  }
  // File clipboard (FileGroupDescriptorW / FileContents) is reserved for later:
  // it needs its own UI (progress + cancellation), so it is intentionally not
  // advertised or requested yet.
  UINT32 dibId = 0;
  UINT32 dibV5Id = 0;
  UINT32 textId = 0;
  UINT32 htmlId = 0;
  for (UINT32 i = 0; i < formatList->numFormats; i++) {
    const CLIPRDR_FORMAT& format = formatList->formats[i];
    if (format.formatName != nullptr && strcmp(format.formatName, kHtmlFormatName) == 0) {
      htmlId = format.formatId;
    } else if (format.formatId == CF_DIB) {
      dibId = format.formatId;
    } else if (format.formatId == CF_DIBV5) {
      dibV5Id = format.formatId;
    } else if (format.formatId == CF_UNICODETEXT) {
      textId = format.formatId;
    }
  }
  // Pull the richest representation, in that order. Only one format is in
  // flight at a time: the channel does not tag data responses with their
  // format, so overlapping requests could not be told apart.
  UINT32 requestId = 0;
  int kind = kClipNone;
  if (dibId != 0) {
    requestId = dibId;
    kind = kClipImage;
  } else if (dibV5Id != 0) {
    requestId = dibV5Id;
    kind = kClipImage;
  } else if (htmlId != 0) {
    requestId = htmlId;
    kind = kClipHtml;
  } else if (textId != 0) {
    requestId = textId;
    kind = kClipText;
  }
  remoteHtmlFormatId_ = htmlId;
  if (requestId == 0) {
    HMRDP_LOGI("cliprdr server format list: no supported format");
    return CHANNEL_RC_OK;
  }
  pendingRemoteKind_ = static_cast<LocalClipKind>(kind);
  CLIPRDR_FORMAT_DATA_REQUEST request = {};
  request.common.msgType = CB_FORMAT_DATA_REQUEST;
  request.common.msgFlags = 0;
  request.requestedFormatId = requestId;
  HMRDP_LOGI("cliprdr requesting remote format id=%{public}u kind=%{public}d", requestId,
             kind);
  return cliprdr_->ClientFormatDataRequest(cliprdr_, &request);
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
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    const UINT32 id = request->requestedFormatId;
    if (id == kHtmlFormatId && localClipKind_ == LocalClipKind::kHtml) {
      payload.assign(localClipboardHtml_.begin(), localClipboardHtml_.end());
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
  return cliprdr_->ClientFormatDataResponse(cliprdr_, &response);
}

UINT Session::OnCliprdrServerFormatDataResponse(
    const CLIPRDR_FORMAT_DATA_RESPONSE* response) {
  const LocalClipKind kind = pendingRemoteKind_;
  pendingRemoteKind_ = LocalClipKind::kNone;
  if (response == nullptr || (response->common.msgFlags & CB_RESPONSE_FAIL) != 0) {
    HMRDP_LOGW("cliprdr data response failed or null");
    return CHANNEL_RC_OK;
  }
  const BYTE* data = response->requestedFormatData;
  const UINT32 size = response->common.dataLen;
  HMRDP_LOGI("cliprdr data response: kind=%{public}d size=%{public}u",
             static_cast<int>(kind), size);
  if (data == nullptr || size == 0) {
    return CHANNEL_RC_OK;
  }
  if (kind == LocalClipKind::kImage) {
    DibImage image;
    if (!ParseDibToBgra(data, size, &image)) {
      HMRDP_LOGW("cliprdr remote image: unsupported DIB layout");
      return CHANNEL_RC_OK;
    }
    std::ostringstream payload;
    payload << image.width << ',' << image.height << '|'
            << Base64Encode(image.bgra.data(), image.bgra.size());
    Emit(SessionEvent::kClipboardImage, payload.str());
    return CHANNEL_RC_OK;
  }
  if (kind == LocalClipKind::kHtml) {
    const std::string html = ParseCfHtml(reinterpret_cast<const char*>(data), size);
    if (!html.empty()) {
      Emit(SessionEvent::kClipboardHtml, html);
    }
    return CHANNEL_RC_OK;
  }
  // Text (default): CF_UNICODETEXT is a NUL-terminated UTF-16LE string; make
  // sure the buffer handed to the converter is terminated even if the server
  // omitted the NUL.
  if (size < sizeof(WCHAR)) {
    return CHANNEL_RC_OK;
  }
  const size_t wcharCount = (size + sizeof(WCHAR) - 1) / sizeof(WCHAR);
  std::vector<WCHAR> wide(wcharCount + 1, 0);
  memcpy(wide.data(), data, size);
  wide[wcharCount] = 0;
  size_t utf8Size = 0;
  char* utf8 = ConvertWCharToUtf8Alloc(wide.data(), &utf8Size);
  if (utf8 == nullptr) {
    return CHANNEL_RC_OK;
  }
  HMRDP_LOGI("cliprdr remote text: %{public}u bytes", static_cast<unsigned>(utf8Size));
  Emit(SessionEvent::kClipboardText, std::string(utf8, utf8Size));
  free(utf8);
  return CHANNEL_RC_OK;
}

// Notifies the server of the local clipboard content after it changed.
void Session::AdvertiseLocalClipboard() {
  if (cliprdr_ == nullptr || !clipboardReady_.load()) {
    return;
  }
  SendLocalFormatList(cliprdr_, static_cast<int>(localClipKind_));
}

void Session::SetLocalClipboardText(const std::string& utf8) {
  if (!clipboardEnabled_.load()) {
    return;
  }
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
    localClipKind_ = LocalClipKind::kText;
  }
  free(wide);
  HMRDP_LOGI("set local clipboard text: %{public}u bytes",
             static_cast<unsigned>(utf8.size()));
  AdvertiseLocalClipboard();
}

void Session::SetLocalClipboardHtml(const std::string& html) {
  if (!clipboardEnabled_.load() || html.empty()) {
    return;
  }
  const std::string text = StripHtmlToText(html);
  size_t wcharCount = 0;
  WCHAR* wide = ConvertUtf8ToWCharAlloc(text.c_str(), &wcharCount);
  if (wide == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    localClipboardHtml_ = BuildCfHtml(html);
    localClipboardUtf16FromHtml_.assign(reinterpret_cast<const char*>(wide),
                                        (wcharCount + 1) * sizeof(WCHAR));
    localClipboardUtf16_.clear();
    localClipboardValid_ = false;
    localClipboardDib_.clear();
    localClipKind_ = LocalClipKind::kHtml;
  }
  free(wide);
  HMRDP_LOGI("set local clipboard html: %{public}u bytes",
             static_cast<unsigned>(html.size()));
  AdvertiseLocalClipboard();
}

void Session::SetLocalClipboardImage(uint32_t width, uint32_t height,
                                     int32_t pixelFormat, const uint8_t* pixels,
                                     size_t byteCount) {
  if (!clipboardEnabled_.load() || pixels == nullptr || width == 0 || height == 0) {
    return;
  }
  const size_t count = static_cast<size_t>(width) * height;
  std::vector<uint8_t> bgra(count * 4, 0);
  if (!ConvertToBgra(pixels, byteCount, width, height, pixelFormat, bgra.data())) {
    HMRDP_LOGW("set local clipboard image: unsupported pixel format %{public}d",
               pixelFormat);
    return;
  }
  const std::vector<uint8_t> dib = BuildBgraToDib(bgra.data(), width, height);
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    localClipboardDib_.assign(dib.begin(), dib.end());
    localClipboardUtf16_.clear();
    localClipboardValid_ = false;
    localClipboardHtml_.clear();
    localClipboardUtf16FromHtml_.clear();
    localClipKind_ = LocalClipKind::kImage;
  }
  HMRDP_LOGI("set local clipboard image: %{public}ux%{public}u fmt=%{public}d",
             width, height, pixelFormat);
  AdvertiseLocalClipboard();
}

}  // namespace hmrdp
