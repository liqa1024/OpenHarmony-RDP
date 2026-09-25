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

// The rdpsnd backend is replaced on OHOS (see native/patches/rdpsnd_opensles.c):
// instead of opening an OpenSL ES device it hands decoded 16-bit PCM to a sink
// registered through HmrdpSetAudioSink. The sink routes the buffer to the
// owning Session, which plays it with OHAudio. It returns the render latency in
// ms (the playback time queued after the packet), which rdpsnd puts into the
// Wave Confirm sent back to the server.
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
// capture patch (native/scripts/freerdp patch) - see GfxDumpRaw. It doubles as
// the arrival stamp for the frame work meter: the hook runs before the chunk is
// decompressed and parsed, so "本机" can charge that share too. The capture write
// itself is done first so its disk I/O never lands inside the measured phases.
extern "C" void HmrdpGfxRawCapture(const BYTE* data, UINT32 size) {
  hmrdp::GfxDumpRaw(data, static_cast<uint32_t>(size));
  hmrdp::GfxWorkMeter* meter = hmrdp::ActiveWorkMeter();
  if (meter != nullptr) {
    meter->OnChunk(static_cast<uint32_t>(size));
  }
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
constexpr int kClipRtf = 4;
constexpr int kClipFiles = 5;

// Client-side id and name for the registered "HTML Format" clipboard format.
// Registered ids start at 0xC000 (WinPR convention); with CB_USE_LONG_FORMAT_NAMES
// the server maps it by name.
constexpr UINT32 kHtmlFormatId = 0xC000;
const char kHtmlFormatName[] = "HTML Format";

// Client-side ids and names for the file clipboard formats. With long format
// names negotiated (CB_USE_LONG_FORMAT_NAMES) the server maps them by name, so
// these ids only have to stay unique and stable within the session.
constexpr UINT32 kFileGroupDescriptorId = 0xC001;
const char kFileGroupDescriptorName[] = "FileGroupDescriptorW";
constexpr UINT32 kFileContentsId = 0xC002;
const char kFileContentsName[] = "FileContents";

// A FILECONTENTS_RANGE response carries at most this many bytes; the server
// answers each request in a single message, so this only bounds the per-message
// buffer, not correctness.
constexpr uint32_t kFileChunkBytes = 256 * 1024;

// [MS-RDPECLIP] 2.2.5.2 FILEGROUPDESCRIPTORW / FILEDESCRIPTORW: a u32 count
// followed by that many fixed 592-byte entries. The times are FILETIME (two
// little-endian u32 parts), so no u64 member and no padding.
constexpr size_t kFileDescriptorNameChars = 260;
struct FileDescriptorW {
  uint32_t flags;
  uint8_t clsid[16];
  int32_t sizeCx;
  int32_t sizeCy;
  int32_t pointX;
  int32_t pointY;
  uint32_t fileAttributes;
  uint32_t creationLow;
  uint32_t creationHigh;
  uint32_t accessLow;
  uint32_t accessHigh;
  uint32_t writeLow;
  uint32_t writeHigh;
  uint32_t sizeHigh;
  uint32_t sizeLow;
  uint16_t fileName[kFileDescriptorNameChars];
};
static_assert(sizeof(FileDescriptorW) == 592, "FILEDESCRIPTORW must be 592 bytes");

constexpr uint32_t kFdAttributes = 0x00000004;
constexpr uint32_t kFdFileSize = 0x00000040;
constexpr uint32_t kFileAttributeNormal = 0x00000080;
constexpr uint32_t kFileAttributeDirectory = 0x00000010;

// A clipboard file name comes from the peer; never let one escape the target
// directory.
std::string SanitizeClipboardFileName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '\0') {
      out.push_back('_');
    } else {
      out.push_back(c);
    }
  }
  if (out.empty() || out == "." || out == "..") {
    out = "file";
  }
  return out;
}

std::string BaseNameOfPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// File-transfer progress event payload:
// "<direction>|<doneBytes>|<totalBytes>|<fileIndex>|<fileCount>".
// `fileIndex` is 1-based, 0 while nothing has started.
std::string BuildTransferProgress(int direction, uint64_t done, uint64_t total,
                                  uint32_t fileIndex, uint32_t fileCount) {
  std::ostringstream out;
  out << direction << '|' << done << '|' << total << '|' << fileIndex << '|'
      << fileCount;
  return out.str();
}

std::string JoinSandboxPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) {
    return name;
  }
  return dir.back() == '/' ? dir + name : dir + "/" + name;
}

bool FileSizeOfPath(const std::string& path, uint64_t* size) {
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0 || size == nullptr) {
    return false;
  }
  *size = static_cast<uint64_t>(st.st_size);
  return true;
}

// Descriptor names are UTF-16LE; the strings crossing the JNI/NAPI boundary are
// UTF-8.
std::string FileDescriptorNameToUtf8(const uint16_t* name, size_t maxChars) {
  size_t chars = 0;
  while (chars < maxChars && name[chars] != 0) {
    ++chars;
  }
  std::vector<WCHAR> wide(chars + 1, 0);
  for (size_t i = 0; i < chars; ++i) {
    wide[i] = static_cast<WCHAR>(name[i]);
  }
  size_t utf8Size = 0;
  char* utf8 = ConvertWCharToUtf8Alloc(wide.data(), &utf8Size);
  if (utf8 == nullptr) {
    return std::string();
  }
  std::string out(utf8, utf8Size);
  free(utf8);
  return out;
}

void Utf8ToFileDescriptorName(const std::string& utf8, uint16_t* out, size_t maxChars) {
  if (out == nullptr || maxChars == 0) {
    return;
  }
  size_t wcharCount = 0;
  WCHAR* wide = ConvertUtf8ToWCharAlloc(utf8.c_str(), &wcharCount);
  if (wide == nullptr) {
    out[0] = 0;
    return;
  }
  const size_t count = wcharCount < maxChars - 1 ? wcharCount : maxChars - 1;
  memcpy(out, wide, count * sizeof(WCHAR));
  out[count] = 0;
  free(wide);
}

// Serialises a FILEGROUPDESCRIPTORW for `paths` (basename only: paths are not
// portable across the session and the server does not need them).
std::string BuildFileGroupDescriptor(const std::vector<std::string>& paths) {
  const uint32_t count = static_cast<uint32_t>(paths.size());
  std::string out(sizeof(uint32_t) + count * sizeof(FileDescriptorW), '\0');
  memcpy(out.data(), &count, sizeof(count));
  for (size_t i = 0; i < paths.size(); ++i) {
    FileDescriptorW descriptor = {};
    descriptor.flags = kFdAttributes | kFdFileSize;
    descriptor.fileAttributes = kFileAttributeNormal;
    uint64_t size = 0;
    if (FileSizeOfPath(paths[i], &size)) {
      descriptor.sizeLow = static_cast<uint32_t>(size & 0xFFFFFFFFull);
      descriptor.sizeHigh = static_cast<uint32_t>(size >> 32);
    }
    Utf8ToFileDescriptorName(BaseNameOfPath(paths[i]), descriptor.fileName,
                             kFileDescriptorNameChars);
    memcpy(out.data() + sizeof(uint32_t) + i * sizeof(FileDescriptorW), &descriptor,
           sizeof(descriptor));
  }
  return out;
}

// ASCII case-insensitive search; the needles used here only contain letters and
// punctuation whose bit 0x20 is already set, so ORing it is safe.
size_t FindIgnoreCase(const std::string& haystack, const std::string& needle,
                      size_t from = 0) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return std::string::npos;
  }
  for (size_t i = from; i + needle.size() <= haystack.size(); ++i) {
    size_t j = 0;
    while (j < needle.size()) {
      const char a = static_cast<char>(haystack[i + j] | 0x20);
      const char b = static_cast<char>(needle[j] | 0x20);
      if (a != b) {
        break;
      }
      ++j;
    }
    if (j == needle.size()) {
      return i;
    }
  }
  return std::string::npos;
}

// Builds a CF_HTML payload: an ASCII header of byte offsets followed by the HTML
// document. See "HTML Clipboard Format" (MSDN). The offset fields are
// fixed-width, so the header length is constant and offsets can be computed up
// front. A full document is embedded as-is (keeping <head> styles); a bare
// fragment is wrapped, and the fragment markers/offsets point at the copied
// content, never at the wrapper.
// Builds a CF_HTML payload around `input`, leaving the HTML content itself
// untouched (only offsets/markers are added).
std::string BuildCfHtml(const std::string& input) {
  static const char kStartFragment[] = "<!--StartFragment-->";
  static const char kEndFragment[] = "<!--EndFragment-->";
  const size_t startMarkerLen = std::strlen(kStartFragment);
  const size_t headerLen = std::strlen("Version:0.9\r\n") +
                           std::strlen("StartHTML:0000000000\r\n") +
                           std::strlen("EndHTML:0000000000\r\n") +
                           std::strlen("StartFragment:0000000000\r\n") +
                           std::strlen("EndFragment:0000000000\r\n");

  std::string document;
  size_t fragmentOffset = 0;
  size_t fragmentLength = 0;

  const size_t existingStart = FindIgnoreCase(input, kStartFragment);
  const size_t existingEnd = FindIgnoreCase(input, kEndFragment);
  if (existingStart != std::string::npos && existingEnd > existingStart + startMarkerLen) {
    // The source already carries fragment markers (Word/WPS exports do): keep the
    // document intact and just point the offsets at the marked content, instead
    // of inserting a second, confusing pair.
    document = input;
    fragmentOffset = existingStart + startMarkerLen;
    fragmentLength = existingEnd - fragmentOffset;
  } else {
    const bool isDocument = FindIgnoreCase(input, "<html") != std::string::npos ||
                            FindIgnoreCase(input, "<!doctype") != std::string::npos ||
                            FindIgnoreCase(input, "<body") != std::string::npos;
    if (isDocument) {
      // Mark the body content as the fragment while keeping the whole document
      // (so <head><style> is preserved).
      const size_t bodyOpen = FindIgnoreCase(input, "<body");
      size_t bodyInner = std::string::npos;
      if (bodyOpen != std::string::npos) {
        const size_t tagEnd = input.find('>', bodyOpen);
        if (tagEnd != std::string::npos) {
          bodyInner = tagEnd + 1;
        }
      }
      const size_t bodyClose =
          bodyInner != std::string::npos ? FindIgnoreCase(input, "</body>", bodyInner)
                                         : std::string::npos;
      if (bodyInner != std::string::npos && bodyClose != std::string::npos) {
        const std::string fragment = input.substr(bodyInner, bodyClose - bodyInner);
        document = input.substr(0, bodyInner) + kStartFragment + fragment +
                   kEndFragment + input.substr(bodyClose);
        fragmentOffset = bodyInner + startMarkerLen;
        fragmentLength = fragment.size();
      } else {
        document = std::string(kStartFragment) + input + kEndFragment;
        fragmentOffset = startMarkerLen;
        fragmentLength = input.size();
      }
    } else {
      const std::string bodyStart = std::string("<html><body>") + kStartFragment;
      document = bodyStart + input + kEndFragment + "</body></html>";
      fragmentOffset = bodyStart.size();
      fragmentLength = input.size();
    }
  }

  const size_t startHtml = headerLen;
  const size_t endHtml = headerLen + document.size();
  const size_t startFragment = headerLen + fragmentOffset;
  const size_t endFragment = startFragment + fragmentLength;
  char header[128] = {0};
  std::snprintf(header, sizeof(header),
                "Version:0.9\r\n"
                "StartHTML:%010zu\r\n"
                "EndHTML:%010zu\r\n"
                "StartFragment:%010zu\r\n"
                "EndFragment:%010zu\r\n",
                startHtml, endHtml, startFragment, endFragment);
  return std::string(header) + document;
}

// Extracts the HTML fragment from a CF_HTML payload. The marked fragment is
// preferred (it is self-contained: inline styles + tags), then the
// StartFragment/EndFragment offsets, then the whole StartHTML..EndHTML document,
// then the raw payload. Returning the bare fragment keeps the payload small and
// avoids handing HarmonyOS a huge Word document it may reject.
std::string ParseCfHtml(const char* data, size_t size) {
  const std::string text(data, size);
  const std::string startMarker = "<!--StartFragment-->";
  const std::string endMarker = "<!--EndFragment-->";
  const size_t markerBegin = text.find(startMarker);
  const size_t markerEnd = text.find(endMarker);
  if (markerBegin != std::string::npos && markerEnd > markerBegin + startMarker.size()) {
    const size_t begin = markerBegin + startMarker.size();
    return text.substr(begin, markerEnd - begin);
  }
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
  const long endHtml = readOffset("EndHTML:");
  if (startHtml >= 0 && endHtml > startHtml && static_cast<size_t>(endHtml) <= size) {
    return text.substr(static_cast<size_t>(startHtml),
                       static_cast<size_t>(endHtml - startHtml));
  }
  return text;
}

std::string HtmlEscapeText(const char* data, size_t len) {
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    switch (data[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out.push_back(data[i]);
    }
  }
  return out;
}

void AppendUtf8(std::string* out, int32_t codepoint) {
  int32_t cp = codepoint < 0 ? codepoint + 0x10000 : codepoint;
  if (cp <= 0) {
    return;  // A NUL byte would truncate the JS string event.
  }
  const uint32_t value = static_cast<uint32_t>(cp);
  if (value < 0x80) {
    out->push_back(static_cast<char>(value));
  } else if (value < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (value >> 6)));
    out->push_back(static_cast<char>(0x80 | (value & 0x3F)));
  } else if (value < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (value >> 12)));
    out->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (value & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (value >> 18)));
    out->push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (value & 0x3F)));
  }
}

std::string RtfColorHex(int r, int g, int b) {
  char buffer[8] = {0};
  std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", r & 0xFF, g & 0xFF, b & 0xFF);
  return std::string(buffer);
}

// Parses the RTF color table; index 0 means "auto" (no explicit color).
std::vector<std::string> ParseRtfColorTable(const std::string& rtf) {
  // Index 0 (auto) is the entry produced by the colortbl's leading ';', so do
  // NOT pre-seed a entry here or every \cfN would be off by one.
  std::vector<std::string> colors;
  const size_t pos = rtf.find("\\colortbl");
  if (pos == std::string::npos) {
    return colors;
  }
  int r = -1;
  int g = -1;
  int b = -1;
  int depth = 0;
  for (size_t i = pos + 9; i < rtf.size(); i++) {
    const char c = rtf[i];
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      if (depth == 0) {
        break;
      }
      depth--;
    } else if (c == ';') {
      colors.push_back(r >= 0 ? RtfColorHex(r, g, b) : std::string());
      r = -1;
      g = -1;
      b = -1;
    } else if (c == '\\') {
      size_t j = i + 1;
      std::string word;
      while (j < rtf.size() && ((rtf[j] >= 'a' && rtf[j] <= 'z') ||
                                (rtf[j] >= 'A' && rtf[j] <= 'Z'))) {
        word.push_back(rtf[j++]);
      }
      int value = 0;
      while (j < rtf.size() && rtf[j] >= '0' && rtf[j] <= '9') {
        value = value * 10 + (rtf[j++] - '0');
      }
      if (word == "red") {
        r = value;
      } else if (word == "green") {
        g = value;
      } else if (word == "blue") {
        b = value;
      }
      i = j - 1;
    }
  }
  return colors;
}

struct RtfRunStyle {
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool strike = false;
  int colorIndex = 0;
  int highlightIndex = 0;
  int fontSizeHalf = 0;
};

std::string RtfStyleToCss(const RtfRunStyle& style,
                          const std::vector<std::string>& colors) {
  std::string css;
  if (style.bold) {
    css += "font-weight:bold;";
  }
  if (style.italic) {
    css += "font-style:italic;";
  }
  std::string decoration;
  if (style.underline) {
    decoration += "underline ";
  }
  if (style.strike) {
    decoration += "line-through ";
  }
  if (!decoration.empty()) {
    css += "text-decoration:" + decoration + ";";
  }
  if (style.colorIndex > 0 && style.colorIndex < static_cast<int>(colors.size()) &&
      !colors[style.colorIndex].empty()) {
    css += "color:" + colors[style.colorIndex] + ";";
  }
  if (style.highlightIndex > 0 &&
      style.highlightIndex < static_cast<int>(colors.size()) &&
      !colors[style.highlightIndex].empty()) {
    css += "background-color:" + colors[style.highlightIndex] + ";";
  }
  if (style.fontSizeHalf > 0) {
    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "font-size:%gpt;", style.fontSizeHalf / 2.0);
    css += buffer;
  }
  return css;
}

// Converts a CF_RTF payload into a simple HTML fragment. Many Windows apps only
// offer RTF (no HTML) for rich text, so this keeps the local clipboard rich.
std::string RtfToHtml(const uint8_t* data, size_t size) {
  const std::string rtf(reinterpret_cast<const char*>(data), size);
  const std::vector<std::string> colors = ParseRtfColorTable(rtf);
  static const char* kDestinations[] = {
      "fonttbl",   "colortbl",    "stylesheet",   "info",       "pict",
      "header",    "footer",      "footerl",      "footerr",    "footnote",
      "listtable", "listoverridetable", "xmlnstbl", "generator", "themedata",
      "colorschememapping", "latentstyles", "datastore", "field", "fldinst",
      "bkmkstart", "bkmkend",     "object",       "shppict",    "nonshppict",
  };
  std::string html = "<p>";
  std::string openCss;
  RtfRunStyle style;
  std::vector<RtfRunStyle> styleStack;
  std::vector<bool> skipStack;
  styleStack.push_back(style);
  skipStack.push_back(false);

  auto skipping = [&skipStack]() {
    for (auto it = skipStack.rbegin(); it != skipStack.rend(); ++it) {
      if (*it) {
        return true;
      }
    }
    return false;
  };
  auto closeRun = [&html, &openCss]() {
    if (!openCss.empty()) {
      html += "</span>";
      openCss.clear();
    }
  };
  auto ensureRun = [&html, &openCss, &colors, &style]() {
    const std::string css = RtfStyleToCss(style, colors);
    if (css == openCss) {
      return;
    }
    if (!openCss.empty()) {
      html += "</span>";
    }
    openCss = css;
    if (!css.empty()) {
      html += "<span style=\"" + css + "\">";
    }
  };

  for (size_t i = 0; i < rtf.size(); i++) {
    const char c = rtf[i];
    if (c == '{') {
      styleStack.push_back(style);
      skipStack.push_back(false);
      size_t j = i + 1;
      while (j < rtf.size() && (rtf[j] == '\r' || rtf[j] == '\n' || rtf[j] == ' ')) {
        j++;
      }
      if (j < rtf.size() && rtf[j] == '\\') {
        size_t k = j + 1;
        if (k < rtf.size() && rtf[k] == '*') {
          skipStack.back() = true;
        } else {
          std::string word;
          while (k < rtf.size() && ((rtf[k] >= 'a' && rtf[k] <= 'z') ||
                                    (rtf[k] >= 'A' && rtf[k] <= 'Z'))) {
            word.push_back(rtf[k++]);
          }
          for (const char* dest : kDestinations) {
            if (word == dest) {
              skipStack.back() = true;
              break;
            }
          }
        }
      }
      continue;
    }
    if (c == '}') {
      closeRun();
      if (!styleStack.empty()) {
        style = styleStack.back();
        styleStack.pop_back();
      }
      if (!skipStack.empty()) {
        skipStack.pop_back();
      }
      continue;
    }
    if (c == '\\') {
      i++;
      if (i >= rtf.size()) {
        break;
      }
      const char nc = rtf[i];
      if (nc == '\\' || nc == '{' || nc == '}') {
        if (!skipping()) {
          ensureRun();
          html.push_back(nc);
        }
        continue;
      }
      if (nc == '\'') {
        if (i + 2 < rtf.size()) {
          const char hex[3] = {rtf[i + 1], rtf[i + 2], 0};
          const int value = static_cast<int>(std::strtol(hex, nullptr, 16));
          if (!skipping()) {
            ensureRun();
            AppendUtf8(&html, value);
          }
          i += 2;
        }
        continue;
      }
      if (nc == '~') {
        if (!skipping()) {
          ensureRun();
          html += "&nbsp;";
        }
        continue;
      }
      if (nc == '-' || nc == '_') {
        continue;
      }
      if (nc == '*') {
        skipStack.back() = true;
        continue;
      }
      size_t j = i;
      std::string word;
      while (j < rtf.size() && ((rtf[j] >= 'a' && rtf[j] <= 'z') ||
                                (rtf[j] >= 'A' && rtf[j] <= 'Z'))) {
        word.push_back(rtf[j++]);
      }
      bool negative = false;
      if (j < rtf.size() && rtf[j] == '-') {
        negative = true;
        j++;
      }
      int value = 0;
      bool hasValue = false;
      while (j < rtf.size() && rtf[j] >= '0' && rtf[j] <= '9') {
        value = value * 10 + (rtf[j++] - '0');
        hasValue = true;
      }
      if (negative) {
        value = -value;
      }
      i = (j < rtf.size() && rtf[j] == ' ') ? j : j - 1;
      if (skipping()) {
        continue;
      }
      if (word == "b" || word == "ab") {
        style.bold = hasValue ? (value != 0) : true;
      } else if (word == "i" || word == "ai") {
        style.italic = hasValue ? (value != 0) : true;
      } else if (word == "ul") {
        style.underline = hasValue ? (value != 0) : true;
      } else if (word == "ulnone") {
        style.underline = false;
      } else if (word == "strike" || word == "striked") {
        style.strike = true;
      } else if (word == "cf") {
        style.colorIndex = value;
      } else if (word == "highlight") {
        style.highlightIndex = value;
      } else if (word == "fs") {
        style.fontSizeHalf = value;
      } else if (word == "plain") {
        style = RtfRunStyle();
      } else if (word == "par" || word == "pard") {
        closeRun();
        html += "</p><p>";
      } else if (word == "line") {
        closeRun();
        html += "<br>";
      } else if (word == "tab") {
        ensureRun();
        html.push_back('\t');
      } else if (word == "u") {
        ensureRun();
        AppendUtf8(&html, value);
        // Skip the single `\uc` fallback character that follows \uN.
        size_t k = i + 1;
        if (k + 2 < rtf.size() && rtf[k] == '\\' && rtf[k + 1] == '\'') {
          i = k + 3;
        } else if (k < rtf.size()) {
          i = k;
        }
      }
      continue;
    }
    if (c == '\r' || c == '\n') {
      continue;
    }
    if (!skipping()) {
      ensureRun();
      html += HtmlEscapeText(&c, 1);
    }
  }
  closeRun();
  html += "</p>";
  return html;
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

// Scales a channel extracted with `mask` from `value` up to 8 bits.
uint8_t MaskToByte(uint32_t value, uint32_t mask) {
  if (mask == 0) {
    return 255;
  }
  uint32_t shift = 0;
  uint32_t shifted = mask;
  while ((shifted & 1u) == 0 && shift < 32) {
    shifted >>= 1;
    ++shift;
  }
  int bits = 0;
  while ((shifted & 1u) != 0 && bits < 32) {
    shifted >>= 1;
    ++bits;
  }
  if (bits <= 0) {
    return 255;
  }
  const uint32_t raw = (value & mask) >> shift;
  if (bits >= 8) {
    return static_cast<uint8_t>(raw >> (bits - 8));
  }
  const uint32_t maxValue = (1u << bits) - 1u;
  return static_cast<uint8_t>((raw * 255u + maxValue / 2u) / maxValue);
}

// Parses a CF_DIB into top-down BGRA. Handles the layouts Windows actually
// sends: BITMAPINFOHEADER / V4 / V5 headers, 16/24/32bpp, BI_RGB and
// BI_BITFIELDS (channel masks; defaults to BGRX for 32bpp BI_RGB).
bool ParseDibToBgra(const uint8_t* data, size_t size, DibImage* out) {
  if (data == nullptr || out == nullptr || size < 40) {
    return false;
  }
  const uint32_t headerSize = ReadDibU32(data + 0);
  const int32_t widthRaw = static_cast<int32_t>(ReadDibU32(data + 4));
  const int32_t heightRaw = static_cast<int32_t>(ReadDibU32(data + 8));
  const uint16_t bpp = ReadDibU16(data + 14);
  const uint32_t compression = ReadDibU32(data + 16);
  if (headerSize < 40 || headerSize > size || widthRaw <= 0 || heightRaw == 0) {
    return false;
  }
  if (bpp != 16 && bpp != 24 && bpp != 32) {
    return false;
  }
  const uint64_t w = static_cast<uint64_t>(widthRaw);
  const uint64_t h = heightRaw < 0
                         ? static_cast<uint64_t>(-static_cast<int64_t>(heightRaw))
                         : static_cast<uint64_t>(heightRaw);
  if (w == 0 || h == 0 || w > 16384 || h > 16384) {
    return false;
  }
  const bool topDown = heightRaw < 0;
  const size_t stride = static_cast<size_t>(((w * bpp + 31) / 32) * 4);

  // Channel masks. For BITMAPINFOHEADER the red/green/blue masks follow the
  // 40-byte header; for V4/V5 they live at the same offset inside the header.
  uint32_t redMask = 0;
  uint32_t greenMask = 0;
  uint32_t blueMask = 0;
  uint32_t alphaMask = 0;
  size_t pixelOffset = headerSize;
  if (compression == 0) {  // BI_RGB
    if (bpp == 32) {
      redMask = 0x00FF0000;
      greenMask = 0x0000FF00;
      blueMask = 0x000000FF;
    } else if (bpp == 16) {
      redMask = 0x7C00;
      greenMask = 0x03E0;
      blueMask = 0x001F;
    }
    // 24bpp is stored as plain BGR.
  } else if (compression == 3 || compression == 6) {  // BI_BITFIELDS / ALPHABITFIELDS
    if (size < 40 + 12) {
      return false;
    }
    redMask = ReadDibU32(data + 40);
    greenMask = ReadDibU32(data + 44);
    blueMask = ReadDibU32(data + 48);
    if (compression == 6 && size >= 40 + 16) {
      alphaMask = ReadDibU32(data + 52);
    }
    // V4/V5 keep the masks inside the header; a bare header is followed by the
    // 12/16 mask bytes.
    pixelOffset = headerSize >= 108 ? headerSize : (40 + (compression == 6 ? 16 : 12));
  } else {
    return false;
  }

  if (pixelOffset + stride * static_cast<size_t>(h) > size) {
    return false;
  }
  out->width = static_cast<uint32_t>(w);
  out->height = static_cast<uint32_t>(h);
  out->bgra.assign(static_cast<size_t>(w * h * 4), 0);
  bool anyAlpha = false;
  for (uint64_t y = 0; y < h; ++y) {
    const uint64_t srcY = topDown ? y : (h - 1 - y);
    const uint8_t* row = data + pixelOffset + static_cast<size_t>(srcY) * stride;
    uint8_t* dst = out->bgra.data() + static_cast<size_t>(y * w * 4);
    for (uint64_t x = 0; x < w; ++x) {
      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;
      uint8_t a = 255;
      if (bpp == 24) {
        const uint8_t* px = row + static_cast<size_t>(x) * 3;
        b = px[0];
        g = px[1];
        r = px[2];
      } else if (bpp == 32) {
        const uint32_t px = ReadDibU32(row + static_cast<size_t>(x) * 4);
        r = MaskToByte(px, redMask);
        g = MaskToByte(px, greenMask);
        b = MaskToByte(px, blueMask);
        if (alphaMask != 0) {
          a = MaskToByte(px, alphaMask);
        }
      } else {  // 16bpp
        const uint16_t px = ReadDibU16(row + static_cast<size_t>(x) * 2);
        r = MaskToByte(px, redMask);
        g = MaskToByte(px, greenMask);
        b = MaskToByte(px, blueMask);
        if (alphaMask != 0) {
          a = MaskToByte(px, alphaMask);
        }
      }
      if (a != 0) {
        anyAlpha = true;
      }
      dst[x * 4 + 0] = b;
      dst[x * 4 + 1] = g;
      dst[x * 4 + 2] = r;
      dst[x * 4 + 3] = a;
    }
  }
  // Many 32bpp DIBs carry an alpha mask but leave it at 0 for an opaque image;
  // treat an all-zero alpha channel as opaque.
  if (alphaMask != 0 && !anyAlpha) {
    for (size_t i = 3; i < out->bgra.size(); i += 4) {
      out->bgra[i] = 255;
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
    GfxWorkInstall(gfx);
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
    // Stop the offload first: the worker must not process (or present) a frame
    // after the channel is gone, and GfxWorkUninstall puts the wrapped callbacks
    // back, so it must not run next to a live worker.
    ctx->session->StopGfxWorker();
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

namespace {

// Upper bound on how long teardown waits for an outstanding context lease to be
// returned. A holder only ever does a short, bounded FreeRDP call, so this is
// only reached if one is stuck; exceeding it delays the free (never corrupts it)
// and is logged.
constexpr uint32_t kLeaseDrainTimeoutMs = 1500;

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

Session::Session()
    : presenter_(CreateFramePresenter()), frameHost_(std::make_unique<GdiFrameHost>()) {
  frameHost_->SetPresenter(presenter_.get());
  frameHost_->SetMeter(&meter_);
  // The live observer on the same chain the replay uses: account for a frame that
  // actually reached the surface (telemetry / input response / first frame).
  frameHost_->SetPresentedFn(
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

void Session::Emit(SessionEvent event, const std::string& data) {
  if (eventFn_) {
    eventFn_(event, data);
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

void Session::EmitMetrics() {
  rdpContext* context = instance_ != nullptr ? instance_->context : nullptr;
  if (context == nullptr) {
    return;
  }
  const uint64_t now = NowMs();

  uint64_t inBytes = 0;
  uint64_t outBytes = 0;
  freerdp_get_stats(context->rdp, &inBytes, &outBytes, nullptr, nullptr);

  const uint32_t frames = frameCount_.load();
  if (!metricsStarted_) {
    lastMetricsTick_ = now;
    lastInBytes_ = inBytes;
    lastOutBytes_ = outBytes;
    lastFrameCount_ = frames;
    meter_.Reset();
    for (uint32_t i = 0; i < 5; ++i) {
      audioLostWindow_[i] = 0;
      audioTotalWindow_[i] = 0;
    }
    audioWindowIndex_ = 0;
    metricsStarted_ = true;
    return;
  }

  const uint64_t elapsedMs = now - lastMetricsTick_;
  if (elapsedMs == 0) {
    return;
  }
  const double seconds = static_cast<double>(elapsedMs) / 1000.0;

  // Byte counters are cumulative; the difference over the interval is the
  // actual network throughput regardless of the coding path (GDI or GFX/H.264).
  const uint64_t rxPerSec = static_cast<uint64_t>(
      static_cast<double>(inBytes - lastInBytes_) / seconds);
  const uint64_t txPerSec = static_cast<uint64_t>(
      static_cast<double>(outBytes - lastOutBytes_) / seconds);
  const uint32_t fps = static_cast<uint32_t>(
      static_cast<double>(frames - lastFrameCount_) / seconds + 0.5);

  // Per-frame client work, kept per phase (see hmrdp_gfx_work.h): ZGX decompress
  // + RDPGFX parse, image decode (which carries the codec's own re-composite), the
  // gdi surface->primary compose and the present. Averaged over the frames the
  // server ended, not over presents: a frame owns this work and its size is
  // decided by the arrival cadence, so dividing by presents would make the figure
  // move with the network rate. There is no pre-summed total anywhere: the phases
  // carry different kinds of time, and each reader adds the ones it wants - the
  // toolbar sums the four work phases and leaves the blocked `sync` out
  // (doc_agent/session-and-input.md §3).
  const GfxWorkMeter::Sample work = meter_.Drain();
  const uint64_t framesDone = work.frames;
  auto perFrame = [framesDone](uint64_t value) {
    return framesDone > 0 ? value / framesDone : 0;
  };
  const uint64_t perFrameZgxUs = perFrame(work.zgxParseUs);
  const uint64_t perFrameDecodeUs = perFrame(work.decodeUs);
  const uint64_t perFrameComposeUs = perFrame(work.composeUs);
  const uint64_t perFramePresentUs = perFrame(work.presentUs);
  const uint64_t perFramePresentWaitUs = perFrame(work.presentWaitUs);
  const uint64_t perFrameSyncUs = perFrame(work.syncUs);
  const uint64_t perFrameBytes = perFrame(work.bytes);
  const uint64_t perFrameCommands = perFrame(work.commands);
  // The four work phases' per-frame sum: the same thing the toolbar shows as 本机.
  // `sync` is blocked time, not work, and is deliberately not in it.
  const uint64_t perFrameWorkUs = perFrameZgxUs + perFrameDecodeUs + perFrameComposeUs +
                                  perFramePresentUs;
  // Duty cycle: how much of this window's wall clock the client actually spent
  // working. Small next to 1000 ‰ means the client is waiting on the far side.
  const uint64_t windowUs = static_cast<uint64_t>(elapsedMs) * 1000;
  const uint32_t dutyPermille =
      windowUs > 0
          ? static_cast<uint32_t>((work.zgxParseUs + work.decodeUs + work.composeUs +
                                   work.presentUs) * 1000 / windowUs)
          : 0;
  // GPU work of the presents in this window: the dirty-rect upload, the 超分辨率
  // upscale and the clear + letterbox quad, from the presenter's timestamp queries.
  // This is *our* work on the GPU, not the GPU's total load - no public API offers
  // the latter to an app - so the toolbar shows it as ms/frame next to the CPU
  // percentage. Zero on a backend that cannot measure it (the GLES fallback).
  uint64_t gpuCopyUs = 0;
  uint64_t gpuSrUs = 0;
  uint64_t gpuBlitUs = 0;
  if (presenter_ != nullptr) {
    presenter_->TakeGpuTimings(&gpuCopyUs, &gpuSrUs, &gpuBlitUs);
  }
  const uint64_t perFrameGpuCopyUs = perFrame(gpuCopyUs);
  const uint64_t perFrameGpuSrUs = perFrame(gpuSrUs);
  const uint64_t perFrameGpuBlitUs = perFrame(gpuBlitUs);
  if (framesDone > 0) {
    // One line, split per phase so a slow one is attributable at a glance, plus
    // the normalization that makes it comparable with a replay of the same
    // stream. `sync` is printed next to the phases but is blocked, not work. The
    // frame acknowledge is not part of any phase: the ack patch writes it before
    // the EndFrame callback runs (see patch-steps/09).
    HMRDP_LOGI("perf: work %{public}llu us/frame = zgx+parse %{public}llu + decode %{public}llu"
               " + compose %{public}llu + present %{public}llu"
               " (+ sync %{public}llu + presentWait %{public}llu blocked)"
               " (frames=%{public}llu presents=%{public}u frames/s=%{public}llu"
               " cmds/frame=%{public}llu kB/frame=%{public}llu duty=%{public}u.%{public}u%%"
               " rx=%{public}lluB/s fps=%{public}u gpu copy=%{public}llu sr=%{public}llu"
               " blit=%{public}llu us/frame)",
               static_cast<unsigned long long>(perFrameWorkUs),
               static_cast<unsigned long long>(perFrameZgxUs),
               static_cast<unsigned long long>(perFrameDecodeUs),
               static_cast<unsigned long long>(perFrameComposeUs),
               static_cast<unsigned long long>(perFramePresentUs),
               static_cast<unsigned long long>(perFrameSyncUs),
               static_cast<unsigned long long>(perFramePresentWaitUs),
               static_cast<unsigned long long>(framesDone), frames - lastFrameCount_,
               static_cast<unsigned long long>(
                   static_cast<double>(framesDone) / seconds + 0.5),
               static_cast<unsigned long long>(perFrameCommands),
               static_cast<unsigned long long>(perFrameBytes / 1024),
               dutyPermille / 10, dutyPermille % 10,
               static_cast<unsigned long long>(rxPerSec), fps,
               static_cast<unsigned long long>(perFrameGpuCopyUs),
               static_cast<unsigned long long>(perFrameGpuSrUs),
               static_cast<unsigned long long>(perFrameGpuBlitUs));
  }
  // Audio glitch rate over the recent window: bytes that failed to play
  // (underrun silence + overflow drops) over all bytes the stream handled.
  uint64_t audioLost = 0;
  uint64_t audioTotal = 0;
  const int audioRate = audio_.TakeLossStats(&audioLost, &audioTotal);
  audioLostWindow_[audioWindowIndex_] = audioLost;
  audioTotalWindow_[audioWindowIndex_] = audioTotal;
  audioWindowIndex_ = (audioWindowIndex_ + 1) % 5;
  uint64_t audioLostSum = 0;
  uint64_t audioTotalSum = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    audioLostSum += audioLostWindow_[i];
    audioTotalSum += audioTotalWindow_[i];
  }
  int32_t audioRateHz = -1;
  int32_t audioLossBp = -1;
  if (audioTotalSum > 0) {
    audioRateHz = audioRate;
    audioLossBp = static_cast<int32_t>(audioLostSum * 10000 / audioTotalSum);
  }

  lastMetricsTick_ = now;
  lastInBytes_ = inBytes;
  lastOutBytes_ = outBytes;
  lastFrameCount_ = frames;

  // Network characteristics are captured from the autodetect callback (see
  // HmrdpNetworkCharacteristicsResult). Average RTT is the steady-state value;
  // base RTT is the fallback before the first average is reported. -1 means
  // "not measured yet".
  int32_t rtt = -1;
  const uint32_t averageRtt = netCharAverageRtt_.load();
  const uint32_t baseRtt = netCharBaseRtt_.load();
  const uint32_t measured = averageRtt != 0 ? averageRtt : baseRtt;
  if (measured != 0) {
    rtt = static_cast<int32_t>(measured);
  }

  // Fields 0..5 are the toolbar's contract (see RdpModels.ets); the rest are the
  // per-frame work in microseconds, phase by phase, plus the normalization that
  // keeps it readable across frame rates (commands and bytes per frame, and the
  // duty cycle in ‰), the blocked `sync`/`presentWait`, and the GPU split
  // (upload / 超分辨率 upscale / letterbox). No pre-summed total: the UI adds the
  // phases it shows and skips the blocked and the GPU ones.
  std::ostringstream payload;
  payload << rtt << "|" << rxPerSec << "|" << txPerSec << "|" << fps
          << "|" << audioRateHz << "|" << audioLossBp
          << "|" << perFrameZgxUs << "|" << perFrameDecodeUs << "|" << perFrameComposeUs
          << "|" << perFramePresentUs << "|" << perFrameBytes << "|" << dutyPermille
          << "|" << perFrameCommands << "|" << perFrameSyncUs
          << "|" << perFramePresentWaitUs
          << "|" << perFrameGpuCopyUs << "|" << perFrameGpuSrUs << "|" << perFrameGpuBlitUs;
  Emit(SessionEvent::kMetrics, payload.str());
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
  meter_.Reset();
  gfxContext_ = nullptr;
  for (uint32_t i = 0; i < 5; ++i) {
    audioLostWindow_[i] = 0;
    audioTotalWindow_[i] = 0;
  }
  audioWindowIndex_ = 0;
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
    if (presenter_ != nullptr) {
      presenter_->SetSuperResolution(true, options.srBackend, options.width, options.height);
    }
    HMRDP_LOGI("session %{public}dx%{public}d (super resolution %{public}s %{public}d%% -> "
               "%{public}dx%{public}d)",
               options.width, options.height,
               options.srBackend == SuperResolutionBackend::kFsr ? "fsr" : "xengine",
               options.srRatioPercent, sessionWidth, sessionHeight);
  } else if (presenter_ != nullptr) {
    presenter_->SetSuperResolution(false, options.srBackend, 0, 0);
  }
  AppendArg(args, "/w:" + std::to_string(sessionWidth));
  AppendArg(args, "/h:" + std::to_string(sessionHeight));
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
  // Audio is only requested when the device actually provides an audio output;
  // otherwise the channel is left off so an unsupported device degrades to a
  // silent session instead of failing.
  const bool audioSupported = AudioOutput::Supported();
  if (options.enableAudio && !audioSupported) {
    HMRDP_LOGW("audio: requested but unsupported on this device, disabling");
  }
  freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback,
                            options.enableAudio && audioSupported);
  // GFX (RDPGFX) is mandatory: without it the session does not render at all,
  // so it is always enabled and the old per-connection toggle was removed.
  freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
  // H.264 (AVC420/AVC444) support was removed: on Windows it only engages when
  // the client advertises AVC444, which is a niche, non-core path (and it still
  // needed the whole decode pipeline offloaded to the GPU to be worth it).
  // RemoteFX Progressive is the default desktop codec, so the client now always
  // uses it; the "hardware decode" setting targets a future GPU RFX decoder.
  freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
  freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
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
    MarkDestroyed();
    return false;
  }

  stopRequested_ = false;
  // The capture hook and the GFX wrappers run without a session context of their
  // own, so the meter is published here (and let go in Teardown).
  SetActiveWorkMeter(&meter_);
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
    SetActiveWorkMeter(nullptr);
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
      // due. The render loop is event-driven, so a fixed 100ms poll would just
      // burn 10 wakeups/s while idle; waiting until the sample deadline drops
      // that to ~1/s. EmitMetrics still runs on the loop after the wait.
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

      // Emit a telemetry sample once the baseline is established and roughly
      // once per second. The loop wakes at least every 100ms.
      if (!metricsStarted_ || NowMs() - lastMetricsTick_ >= 1000) {
        EmitMetrics();
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

  // Stop the GFX offload before the disconnect tears the channels down: the
  // worker parses into the rdpgfx plugin, which freerdp_disconnect frees (and the
  // frame acknowledge it sends goes through the same channel). Off the drdynvc
  // thread these are no longer serialised with that teardown, so the worker must
  // be joined first.
  StopGfxWorker();
  freerdp_disconnect(instance);
  Emit(SessionEvent::kDisconnected, EncodeError(lastErrorCode_, lastError_));
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
  // Runs on the RDP thread once its connect/loop has ended. It is the sole
  // owner of the FreeRDP context: first it stops handing out leases and waits for
  // the outstanding ones to be returned, then it frees - so no other thread can
  // be holding the context (or its cliprdr channel) when it goes away.
  //
  // The offload worker is joined first: it is the one thread that could still
  // touch the GFX context after this point (HmrdpPostDisconnect normally stops it
  // already; this covers the paths where that did not run).
  StopGfxWorker();
  if (ActiveWorkMeter() == &meter_) {
    SetActiveWorkMeter(nullptr);
  }
  freerdp* instance = nullptr;
  int leasedAfterWait = 0;
  {
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    tearingDown_ = true;
    instance = instance_;
    instance_ = nullptr;
    cliprdr_ = nullptr;
    // Every lease holder only performs a short, bounded FreeRDP call (and the
    // abort/disconnect this teardown follows makes a blocked send fail), so this
    // normally returns immediately.
    leasesCv_.wait_for(lock, std::chrono::milliseconds(kLeaseDrainTimeoutMs),
                       [this] { return activeLeases_ == 0; });
    leasedAfterWait = activeLeases_;
  }
  if (leasedAfterWait != 0) {
    // A holder is still inside the context after the wait. Freeing it now would
    // be a use-after-free, so the context is *abandoned* instead: the memory (and
    // the window, which destroySession only releases once this marks terminal)
    // leaks rather than being freed under a live user. This is the pathological
    // path - it is logged so it is never silent.
    HMRDP_LOGW("teardown: %{public}d context lease(s) still held after %{public}dms;"
               " abandoning (not freeing) the context",
               leasedAfterWait, kLeaseDrainTimeoutMs);
    return;
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
    ResetRemoteFileDownloadLocked();
    remoteFiles_.clear();
  }
  audio_.Close();
  // The desktop buffer dies with the presenter; the next connect re-attaches.
  frameHost_->DetachDesktopBuffer();
  if (presenter_ != nullptr) {
    presenter_->Reset();
  }
  // Terminal: publishes the phase and wakes WaitDestroyed().
  MarkDestroyed();
}

void Session::HandlePostConnect() {
  Emit(SessionEvent::kConnected, "");
}

void Session::HandleGfxData(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
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
    auto* gfx = static_cast<RdpgfxClientContext*>(gfxWorkerContext_);
    if (gfx == nullptr) {
      continue;
    }
    ContextGuard ctx(*this);
    if (!ctx.valid()) {
      continue;
    }
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
      // Same QoS the frame pipeline used to get on the drdynvc thread (see
      // HmrdpThreadQoSHook / patch 09): the platform then schedules the frame
      // work with less wake-up and preemption latency.
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
  frameHost_->OnFrameBegin();
}

void Session::HandleBeginPaint() {
  // gdi is about to compose this frame into the primary buffer (the mirrored
  // surface's pixels are already there). Same wait as HandleFrameBegin, and a
  // no-op when that one already drained the previous frame's copy - it is kept
  // because the compose of a *non-mirrored* surface happens here.
  frameHost_->OnBeginPaint();
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
  (void)frameHost_->OnEndPaint(gdi, &presentUs, nullptr);
}

void Session::AfterPresent() {
  // The frame's present is charged to the meter by the shared frame host itself
  // (work and blocked halves), so that the live session and the replay cannot
  // report it differently; this only does the per-frame bookkeeping.
  frameCount_.fetch_add(1);

  if (!firstFrameSent_) {
    firstFrameSent_ = true;
    Emit(SessionEvent::kFirstFrame, "");
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
  {
    // FreeRDP closes the channels right after this callback
    // (freerdp_disconnect -> freerdp_channels_close), so the channel pointer is
    // dropped under the lifecycle lock and the outstanding leases are waited out
    // here: once this returns, no lease can still point at the channel that is
    // about to be freed.
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    cliprdr_ = nullptr;
    leasesCv_.wait_for(lock, std::chrono::milliseconds(kLeaseDrainTimeoutMs),
                       [this] { return activeLeases_ == 0; });
    if (activeLeases_ != 0) {
      HMRDP_LOGW("post-disconnect: %{public}d lease(s) still held after %{public}dms;"
                 " the channel is closing regardless", activeLeases_,
                 kLeaseDrainTimeoutMs);
    }
  }
  remoteHtmlFormatId_ = 0;
  remoteRequestInFlight_ = false;
  remoteRequestKind_ = LocalClipKind::kNone;
  hasPendingRemoteRefresh_ = false;
  pendingRefreshKind_ = LocalClipKind::kNone;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    // The channel is gone: any download in flight can never finish.
    ResetRemoteFileDownloadLocked();
    remoteFiles_.clear();
  }
  // Make the capture durable before the buffers are dropped with the session.
  hmrdp::GfxDumpFlush();
}

int Session::OnAudioData(const void* data, size_t size, int sampleRate, int channels) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  return audio_.Write(data, size, sampleRate, channels);
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

void Session::SetRfxDump(bool enabled, const std::string& dir) {
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
  if (cliprdr_ == nullptr || formatList == nullptr ||
      cliprdr_->ClientFormatDataRequest == nullptr) {
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
  bool abortedDownload = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    // Anything the server advertises now supersedes any transfer in progress
    // (and an empty file list invalidates the cached descriptors).
    abortedDownload = remoteFilesActive_;
    ResetRemoteFileDownloadLocked();
    if (fileGroupId == 0) {
      remoteFiles_.clear();
    }
  }
  if (abortedDownload) {
    // Tell the UI: its progress row would otherwise wait for an event that can
    // no longer arrive.
    Emit(SessionEvent::kFileTransferFailed, "远端剪贴板已改变，传输已中止");
  }
  // Pull the richest representation, in that order. A file copy is a list, so
  // it is asked for first; the "richest single value" formats follow. Only one
  // format is in flight at a time: the channel does not tag data responses with
  // their format, so overlapping requests could not be told apart.
  UINT32 requestId = 0;
  int kind = kClipNone;
  if (fileGroupId != 0) {
    requestId = fileGroupId;
    kind = kClipFiles;
  } else if (dibId != 0) {
    requestId = dibId;
    kind = kClipImage;
  } else if (dibV5Id != 0) {
    requestId = dibV5Id;
    kind = kClipImage;
  } else if (htmlId != 0) {
    requestId = htmlId;
    kind = kClipHtml;
  } else if (rtfId != 0) {
    // Rich Text Format is the only structured format many Windows apps offer
    // (Word/WPS often omit HTML); convert it to HTML for the local clipboard.
    requestId = rtfId;
    kind = kClipRtf;
  } else if (textId != 0) {
    requestId = textId;
    kind = kClipText;
  }
  remoteHtmlFormatId_ = htmlId;
  if (requestId == 0) {
    HMRDP_LOGI("cliprdr server format list: no supported format");
    return CHANNEL_RC_OK;
  }
  if (remoteRequestInFlight_) {
    // Keep only the newest change; it is fetched once the current response
    // arrives (never overlap requests: responses are untagged).
    hasPendingRemoteRefresh_ = true;
    pendingRefreshKind_ = static_cast<LocalClipKind>(kind);
    pendingRefreshFormatId_ = requestId;
    return CHANNEL_RC_OK;
  }
  return SendRemoteDataRequest(requestId, static_cast<LocalClipKind>(kind));
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
  std::vector<std::string> advertisedFiles;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    const UINT32 id = request->requestedFormatId;
    // Both ids we advertise for a file clipboard carry the descriptor list:
    // servers echo the id from our FORMAT_LIST, but there is no data format
    // behind FileContents (byte ranges use FILECONTENTS_REQUEST), so accept
    // either instead of depending on which one the peer picked.
    if (localClipKind_ == LocalClipKind::kFiles &&
        (id == kFileGroupDescriptorId || id == kFileContentsId)) {
      advertisedFiles = localClipboardFilePaths_;
    } else if (id == kHtmlFormatId && localClipKind_ == LocalClipKind::kHtml) {
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
  if (!advertisedFiles.empty()) {
    const std::string descriptor = BuildFileGroupDescriptor(advertisedFiles);
    payload.assign(descriptor.begin(), descriptor.end());
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
  if (!advertisedFiles.empty()) {
    uint64_t total = 0;
    size_t fileCount = 0;
    {
      std::lock_guard<std::mutex> lock(clipboardMutex_);
      total = localTransferTotal_;
      fileCount = localClipboardFilePaths_.size();
    }
    if (total == 0) {
      // All advertised files are empty, so there is nothing left to stream.
      std::ostringstream donePayload;
      donePayload << fileCount;
      Emit(SessionEvent::kFileTransferDone, donePayload.str());
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
    // FILEGROUPDESCRIPTORW: u32 count + count fixed-size descriptors. The list
    // itself is small; the file bytes are streamed later, on demand.
    constexpr size_t kHeaderBytes = sizeof(uint32_t);
    uint32_t count = 0;
    if (size >= kHeaderBytes + sizeof(uint32_t)) {
      memcpy(&count, data, sizeof(count));
    }
    const size_t needed = kHeaderBytes + static_cast<size_t>(count) * sizeof(FileDescriptorW);
    if (count == 0 || needed > size) {
      HMRDP_LOGW("cliprdr remote file list malformed: count=%{public}u size=%{public}u",
                 count, static_cast<unsigned>(size));
    } else {
      std::vector<ClipboardFileInfo> files;
      files.reserve(count);
      uint64_t totalBytes = 0;
      uint32_t directoryCount = 0;
      for (uint32_t i = 0; i < count; ++i) {
        FileDescriptorW descriptor = {};
        memcpy(&descriptor, data + kHeaderBytes + i * sizeof(FileDescriptorW),
               sizeof(descriptor));
        ClipboardFileInfo info;
        info.name = SanitizeClipboardFileName(
            FileDescriptorNameToUtf8(descriptor.fileName, kFileDescriptorNameChars));
        if ((descriptor.flags & kFdAttributes) != 0 &&
            (descriptor.fileAttributes & kFileAttributeDirectory) != 0) {
          info.isDirectory = true;
        }
        if ((descriptor.flags & kFdFileSize) != 0) {
          info.size = (static_cast<uint64_t>(descriptor.sizeHigh) << 32) |
                      descriptor.sizeLow;
          info.sizeKnown = true;
        }
        if (info.isDirectory) {
          ++directoryCount;
        }
        totalBytes += info.size;
        files.push_back(std::move(info));
      }
      {
        std::lock_guard<std::mutex> lock(clipboardMutex_);
        // Keep the whole list, directories included: a FILECONTENTS_REQUEST's
        // listIndex is the position in what the server advertised, so filtering
        // here would shift every later index.
        remoteFiles_ = std::move(files);
      }
      std::ostringstream payload;
      payload << (count - directoryCount) << '|' << totalBytes << '|' << directoryCount;
      HMRDP_LOGI("cliprdr remote file list: files=%{public}u dirs=%{public}u total=%{public}llu",
                 count - directoryCount, directoryCount,
                 static_cast<unsigned long long>(totalBytes));
      Emit(SessionEvent::kClipboardFileList, payload.str());
    }
  } else if (kind == LocalClipKind::kImage) {
    DibImage image;
    if (ParseDibToBgra(data, size, &image)) {
      std::ostringstream payload;
      payload << image.width << ',' << image.height << '|'
              << Base64Encode(image.bgra.data(), image.bgra.size());
      Emit(SessionEvent::kClipboardImage, payload.str());
    } else {
      const uint32_t dibHeader = size >= 4 ? ReadDibU32(data) : 0;
      const uint16_t dibBpp = size >= 16 ? ReadDibU16(data + 14) : 0;
      const uint32_t dibCompression = size >= 20 ? ReadDibU32(data + 16) : 0;
      HMRDP_LOGW(
          "cliprdr remote image: unsupported DIB size=%{public}u header=%{public}u "
          "bpp=%{public}u compression=%{public}u",
          static_cast<unsigned>(size), dibHeader, static_cast<unsigned>(dibBpp),
          dibCompression);
    }
  } else if (kind == LocalClipKind::kHtml) {
    const std::string html = ParseCfHtml(reinterpret_cast<const char*>(data), size);
    if (!html.empty()) {
      Emit(SessionEvent::kClipboardHtml, html);
    }
  } else if (kind == LocalClipKind::kRtf) {
    const std::string html = RtfToHtml(data, size);
    if (!html.empty()) {
      Emit(SessionEvent::kClipboardHtml, html);
    }
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

  // A newer clipboard change may have arrived while this request was in flight.
  if (hasPendingRemoteRefresh_) {
    hasPendingRemoteRefresh_ = false;
    return SendRemoteDataRequest(pendingRefreshFormatId_, pendingRefreshKind_);
  }
  return CHANNEL_RC_OK;
}

// Notifies the server of the local clipboard content after it changed.
void Session::AdvertiseLocalClipboard() {
  CliprdrClientContext* cliprdr = cliprdrLocked();
  if (cliprdr == nullptr || !clipboardReady_.load()) {
    return;
  }
  SendLocalFormatList(cliprdr, static_cast<int>(localClipKind_));
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
    localClipKind_ = LocalClipKind::kText;
  }
  free(wide);
  HMRDP_LOGI("set local clipboard text: %{public}u bytes",
             static_cast<unsigned>(utf8.size()));
  AdvertiseLocalClipboard();
}

void Session::SetLocalClipboardHtml(const std::string& html) {
  ContextGuard ctx(*this);
  if (html.empty()) {
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
  ContextGuard ctx(*this);
  if (pixels == nullptr || width == 0 || height == 0) {
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

void Session::SetLocalClipboardFiles(const std::vector<std::string>& paths) {
  ContextGuard ctx(*this);
  if (paths.empty()) {
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
    localClipboardUtf16_.clear();
    localClipboardValid_ = false;
    localClipboardHtml_.clear();
    localClipboardUtf16FromHtml_.clear();
    localClipboardDib_.clear();
    localClipKind_ = LocalClipKind::kFiles;
    localTransferDone_ = 0;
    localTransferTotal_ = total;
  }
  HMRDP_LOGI("set local clipboard files: count=%{public}u total=%{public}llu",
             static_cast<unsigned>(paths.size()),
             static_cast<unsigned long long>(total));
  AdvertiseLocalClipboard();
  // Tell the UI the size of the upload so it can show a progress bar before the
  // server starts pulling.
  Emit(SessionEvent::kFileTransferProgress,
       BuildTransferProgress(1, 0, total, 0, static_cast<uint32_t>(paths.size())));
}

void Session::ResetRemoteFileDownloadLocked() {
  if (remoteFileHandle_ != nullptr) {
    fclose(remoteFileHandle_);
    remoteFileHandle_ = nullptr;
  }
  if (remoteFilesActive_) {
    // Drop the partial copies: a half-downloaded file is worse than none, and a
    // retry starts from scratch anyway.
    for (const std::string& path : remoteFilesPaths_) {
      remove(path.c_str());
    }
  }
  remoteFilesActive_ = false;
  remoteFilesDestDir_.clear();
  remoteFilesPaths_.clear();
  remoteFileIndex_ = 0;
  remoteFileOffset_ = 0;
  remoteFileSize_ = 0;
  remoteFileSizeKnown_ = false;
  remoteFileSizePending_ = false;
  remoteTransferDone_ = 0;
  remoteTransferTotal_ = 0;
}

void Session::CancelFileTransfer() {
  bool pulled = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    ResetRemoteFileDownloadLocked();
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
    localClipKind_ = LocalClipKind::kText;
    withdrawn = true;
  }
  if (withdrawn) {
    HMRDP_LOGI("cliprdr file list withdrawn (empty text advertised)");
    AdvertiseLocalClipboard();
  }
}

void Session::PullRemoteFiles(const std::string& destDir) {
  // Keep the cliprdr channel alive for the whole pull: the step below sends the
  // first file-contents request on this (UI) thread.
  ContextGuard ctx(*this);
  CliprdrClientContext* cliprdr = cliprdrLocked();
  if (cliprdr == nullptr || cliprdr->ClientFileContentsRequest == nullptr ||
      destDir.empty()) {
    Emit(SessionEvent::kFileTransferFailed, "剪贴板通道不可用");
    return;
  }
  size_t fileCount = 0;
  uint64_t total = 0;
  bool nothingToPull = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (remoteFilesActive_) {
      // Already running: the UI serialises transfers, so this is a stray call.
      return;
    }
    nothingToPull = remoteFiles_.empty();
    if (!nothingToPull) {
      remoteFilesDestDir_ = destDir;
      remoteFilesPaths_.clear();
      remoteFilesPaths_.reserve(remoteFiles_.size());
      remoteTransferTotal_ = 0;
      remoteTransferDone_ = 0;
      for (const ClipboardFileInfo& file : remoteFiles_) {
        if (file.isDirectory) {
          // Keep the vector aligned with the server's listIndex, but leave an
          // empty entry so it is neither downloaded nor reported.
          remoteFilesPaths_.push_back(std::string());
          continue;
        }
        remoteFilesPaths_.push_back(
            JoinSandboxPath(destDir, SanitizeClipboardFileName(file.name)));
        remoteTransferTotal_ += file.size;
      }
      remoteFileIndex_ = 0;
      remoteFileOffset_ = 0;
      remoteFileSize_ = 0;
      remoteFileSizeKnown_ = false;
      remoteFileSizePending_ = false;
      remoteFilesActive_ = true;
      total = remoteTransferTotal_;
      for (const ClipboardFileInfo& file : remoteFiles_) {
        if (!file.isDirectory) {
          ++fileCount;
        }
      }
    }
  }
  if (nothingToPull) {
    // The list was cleared (e.g. the remote clipboard changed) between the UI
    // reading the count and pressing the button; clear its progress row.
    Emit(SessionEvent::kFileTransferFailed, "没有可下载的文件");
    return;
  }
  HMRDP_LOGI("cliprdr download start: %{public}u file(s)", static_cast<unsigned>(fileCount));
  Emit(SessionEvent::kFileTransferProgress,
       BuildTransferProgress(0, 0, total, 0, static_cast<uint32_t>(fileCount)));
  StartNextRemoteFileStep();
}

void Session::FinishRemoteFileTransfer() {
  std::string payload;
  size_t fileCount = 0;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (remoteFileHandle_ != nullptr) {
      fclose(remoteFileHandle_);
      remoteFileHandle_ = nullptr;
    }
    for (size_t i = 0; i < remoteFilesPaths_.size(); ++i) {
      if (i > 0) {
        payload.push_back('\n');
      }
      payload += remoteFilesPaths_[i];
    }
    fileCount = remoteFilesPaths_.size();
    remoteFilesActive_ = false;
    remoteFileIndex_ = 0;
    remoteFileOffset_ = 0;
    remoteFileSize_ = 0;
    remoteFileSizeKnown_ = false;
    remoteFileSizePending_ = false;
  }
  HMRDP_LOGI("cliprdr download complete: %{public}u file(s)",
             static_cast<unsigned>(fileCount));
  Emit(SessionEvent::kClipboardFilesReady, payload);
}

void Session::FailRemoteFileTransfer(const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    ResetRemoteFileDownloadLocked();
  }
  HMRDP_LOGW("cliprdr file transfer failed: %{public}s", reason.c_str());
  Emit(SessionEvent::kFileTransferFailed, reason);
}

UINT Session::StartNextRemoteFileStep() {
  // Snapshot under the lock; when this runs from PullRemoteFiles (UI thread) the
  // caller's lease is what keeps the channel alive until the request is sent.
  CliprdrClientContext* cliprdr = cliprdrLocked();
  if (cliprdr == nullptr || cliprdr->ClientFileContentsRequest == nullptr) {
    FailRemoteFileTransfer("剪贴板通道不可用");
    return CHANNEL_RC_OK;
  }
  for (;;) {
    CLIPRDR_FILE_CONTENTS_REQUEST request = {};
    std::string path;
    std::string openError;
    bool sendRequest = false;
    bool fileListDone = false;
    {
      std::lock_guard<std::mutex> lock(clipboardMutex_);
      if (!remoteFilesActive_) {
        return CHANNEL_RC_OK;
      }
      if (remoteFileIndex_ >= remoteFiles_.size()) {
        fileListDone = true;
      } else {
        ClipboardFileInfo& info = remoteFiles_[remoteFileIndex_];
        path = remoteFilesPaths_[remoteFileIndex_];
        if (info.isDirectory || path.empty()) {
          // Directories cannot be streamed over the file clipboard: skip them
          // (the UI tells the user they are unsupported).
          remoteFileIndex_ += 1;
          remoteFileOffset_ = 0;
          remoteFileSize_ = 0;
          remoteFileSizeKnown_ = false;
          remoteFileSizePending_ = false;
          continue;
        }
        if (!remoteFileSizeKnown_ && info.sizeKnown) {
          remoteFileSize_ = info.size;
          remoteFileSizeKnown_ = true;
        }
        if (!remoteFileSizeKnown_) {
          if (remoteFileSizePending_) {
            // A size request is already in flight; wait for its response.
            return CHANNEL_RC_OK;
          }
          remoteFileSizePending_ = true;
          request.dwFlags = FILECONTENTS_SIZE;
          request.cbRequested = static_cast<UINT32>(sizeof(uint64_t));
          request.nPositionLow = 0;
          request.nPositionHigh = 0;
          sendRequest = true;
        } else {
          if (remoteFileHandle_ == nullptr) {
            remoteFileHandle_ = fopen(path.c_str(), "wb");
            if (remoteFileHandle_ == nullptr) {
              openError = "无法创建文件";
            } else {
              remoteFileOffset_ = 0;
            }
          }
          if (openError.empty()) {
            if (remoteFileOffset_ >= remoteFileSize_) {
              // Current file complete (this also covers zero-byte files).
              if (remoteFileHandle_ != nullptr) {
                fclose(remoteFileHandle_);
                remoteFileHandle_ = nullptr;
              }
              remoteFileIndex_ += 1;
              remoteFileOffset_ = 0;
              remoteFileSize_ = 0;
              remoteFileSizeKnown_ = false;
              continue;
            }
            const uint64_t remaining = remoteFileSize_ - remoteFileOffset_;
            const uint32_t chunk = remaining < kFileChunkBytes
                                       ? static_cast<uint32_t>(remaining)
                                       : kFileChunkBytes;
            request.dwFlags = FILECONTENTS_RANGE;
            request.nPositionLow =
                static_cast<UINT32>(remoteFileOffset_ & 0xFFFFFFFFull);
            request.nPositionHigh = static_cast<UINT32>(remoteFileOffset_ >> 32);
            request.cbRequested = chunk;
            sendRequest = true;
          }
        }
        if (sendRequest) {
          request.common.msgType = CB_FILECONTENTS_REQUEST;
          request.streamId = static_cast<UINT32>(remoteFileIndex_ + 1);
          request.listIndex = static_cast<UINT32>(remoteFileIndex_);
          request.haveClipDataId = FALSE;
        }
      }
    }
    if (!openError.empty()) {
      FailRemoteFileTransfer(openError);
      return CHANNEL_RC_OK;
    }
    if (sendRequest) {
      // One line per file (SIZE / first range), not per chunk.
      if (request.dwFlags == FILECONTENTS_SIZE ||
          (request.nPositionLow == 0 && request.nPositionHigh == 0)) {
        HMRDP_LOGI("cliprdr request file content: index=%{public}u flags=0x%{public}x cb=%{public}u",
                   request.listIndex, request.dwFlags, request.cbRequested);
      }
      return cliprdr->ClientFileContentsRequest(cliprdr, &request);
    }
    if (fileListDone) {
      FinishRemoteFileTransfer();
      return CHANNEL_RC_OK;
    }
    return CHANNEL_RC_OK;
  }
}

UINT Session::HandleRemoteFileContentsResponse(
    const CLIPRDR_FILE_CONTENTS_RESPONSE* response) {
  if (response == nullptr) {
    return CHANNEL_RC_OK;
  }
  const bool failed = (response->common.msgFlags & CB_RESPONSE_FAIL) != 0;
  std::string failReason;
  uint64_t done = 0;
  uint64_t total = 0;
  uint32_t fileCount = 0;
  uint32_t currentIndex = 0;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (!remoteFilesActive_) {
      return CHANNEL_RC_OK;
    }
    if (failed) {
      failReason = "远端拒绝提供文件内容";
    } else if (remoteFileSizePending_) {
      remoteFileSizePending_ = false;
      if (response->requestedData == nullptr ||
          response->cbRequested != sizeof(uint64_t)) {
        failReason = "文件大小响应无效";
      } else {
        uint64_t size = 0;
        memcpy(&size, response->requestedData, sizeof(size));
        if (remoteFileIndex_ < remoteFiles_.size()) {
          ClipboardFileInfo& info = remoteFiles_[remoteFileIndex_];
          remoteTransferTotal_ = remoteTransferTotal_ - info.size + size;
          info.size = size;
          info.sizeKnown = true;
        }
        remoteFileSize_ = size;
        remoteFileSizeKnown_ = true;
      }
    } else {
      if (response->requestedData == nullptr || response->cbRequested == 0) {
        // An empty range means the server has nothing more for this file.
        remoteFileOffset_ = remoteFileSize_;
      } else if (remoteFileHandle_ == nullptr) {
        failReason = "文件未打开";
      } else {
        const size_t written =
            fwrite(response->requestedData, 1, response->cbRequested, remoteFileHandle_);
        if (written != response->cbRequested) {
          failReason = "写入文件失败";
        } else {
          remoteFileOffset_ += written;
          remoteTransferDone_ += written;
        }
      }
    }
    done = remoteTransferDone_;
    total = remoteTransferTotal_;
    for (const ClipboardFileInfo& info : remoteFiles_) {
      if (!info.isDirectory) {
        ++fileCount;
      }
    }
    if (remoteFileIndex_ < remoteFiles_.size() && !remoteFiles_[remoteFileIndex_].isDirectory) {
      currentIndex = static_cast<uint32_t>(remoteFileIndex_) + 1;
    }
  }
  if (!failReason.empty()) {
    FailRemoteFileTransfer(failReason);
    return CHANNEL_RC_OK;
  }
  Emit(SessionEvent::kFileTransferProgress,
       BuildTransferProgress(0, done, total, currentIndex, fileCount));
  return StartNextRemoteFileStep();
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
  Emit(SessionEvent::kFileTransferProgress,
       BuildTransferProgress(1, done, total, request->listIndex + 1, fileCount));
  if (uploadDone) {
    std::ostringstream donePayload;
    donePayload << fileCount;
    Emit(SessionEvent::kFileTransferDone, donePayload.str());
    // Every advertised byte is served, so the list has no reason to stay on the
    // clipboard: withdrawing it keeps a second paste on the remote side from
    // pulling the same files again (with no progress row to cancel).
    WithdrawLocalFileClipboard();
  }
  return rc;
}

}  // namespace hmrdp
