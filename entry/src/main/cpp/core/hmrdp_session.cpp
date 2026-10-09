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
#include "hmrdp_bitmap.h"
#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_parallel.h"

// The rdpsnd backend is patched on OHOS to hand decoded PCM to a HmrdpSetAudioSink
// instead of opening an OpenSL ES device; it returns the render latency in ms.
using HmrdpAudioSink = int (*)(void* context, const void* data, size_t size,
                               int sampleRate, int channels);

extern "C" void HmrdpSetAudioSink(HmrdpAudioSink sink);

// Defined by the patched rdpei client; weak, so libhmrdp links against stock FreeRDP.
extern "C" void HmrdpSetTouchFrameInterval(UINT32 intervalMs) __attribute__((weak));

// Touch pacing for the patched rdpei client: the interval is a floor on the flush
// period (rate ceiling), and neither value is zero so the network is not the only bound.
static constexpr UINT32 kTouchFrameIntervalDefaultMs = 20;
static constexpr UINT32 kTouchFrameIntervalHighRateMs = 8;

// Raw (still ZGX-compressed) GFX chunk sink, installed only with the HmRdp GFX
// capture patch; the write happens first so its disk I/O never lands in the measured phases.
extern "C" void HmrdpGfxRawCapture(const BYTE* data, UINT32 size) {
  hmrdp::GfxDumpRaw(data, static_cast<uint32_t>(size));
}

// Defined by the patched rdpgfx client; weak so stock FreeRDP links unchanged.
extern "C" void HmrdpSetGfxRawCapture(void (*fn)(const BYTE* data, UINT32 size))
    __attribute__((weak));

// Defined by the patched winpr, called once per pool/drdynvc thread; weak, so a
// stock FreeRDP links and QoS marking is simply absent.
extern "C" void HmrdpSetThreadQoSApplier(void (*fn)(void)) __attribute__((weak));

namespace hmrdp {

// HarmonyOS QoS (Kernel_Enhance_Kit): a thread marks its own importance to lower
// wake-up/preemption latency. libqos.so is dlopen()ed, so a device without it runs unmarked.
using SetThreadQoSFn = int (*)(int level);
// QoS_Level 3 (USER_INITIATED), deliberately below the UI level so the session
// window's own drawing is never starved by decoding.
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
using fileclip::FileSizeOfPath;
using fileclip::kFileContentsId;
using fileclip::kFileContentsName;
using fileclip::kFileGroupDescriptorId;
using fileclip::kFileGroupDescriptorName;

namespace {

// Whether remote cursor updates drive the HarmonyOS system cursor; global because
// it is a user setting, read when a session connects.
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

// Must match Session::LocalClipKind.
constexpr int kClipText = 1;
constexpr int kClipHtml = 2;
constexpr int kClipImage = 3;
constexpr int kClipRtf = 4;
constexpr int kClipFiles = 5;

// Registered ids start at 0xC000 (WinPR convention); CB_USE_LONG_FORMAT_NAMES maps by name.
constexpr UINT32 kHtmlFormatId = 0xC000;
const char kHtmlFormatName[] = "HTML Format";

// Windows offers images as CF_DIB plus, when advertised, this compressed form.
constexpr UINT32 kPngFormatId = 0xC003;
const char kPngFormatName[] = "PNG";

// Hard cap on the source cursor size; larger is treated as corrupt and dropped.
constexpr uint32_t kMaxCursorSourceSide = 1024;

typedef struct {
  rdpClientContext common;
  Session* session;
} HmrdpContext;

// FreeRDP only forwards the Network Characteristics Result PDU, so libhmrdp
// registers its own handler to capture the RTT.
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

// Custom pointer class: `Free` must not release the struct, FreeRDP's pointer
// cache owns it (libfreerdp/cache/pointer.c).
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

BOOL HmrdpPointerSetPosition(rdpContext*, UINT32, UINT32) {
  return TRUE;
}

BOOL HmrdpPlaySound(rdpContext*, const PLAY_SOUND_UPDATE*) {
  return TRUE;
}

// The default handler sends this PDU straight back (it exists for proxy
// forwarding), so it must be replaced to consume it here.
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
    // The matching FileContents pseudo-format is announced alongside the
    // descriptor list, as the Windows client does.
    formats[count].formatId = kFileGroupDescriptorId;
    formats[count].formatName = const_cast<char*>(kFileGroupDescriptorName);
    ++count;
    formats[count].formatId = kFileContentsId;
    formats[count].formatName = const_cast<char*>(kFileContentsName);
    ++count;
  } else if (kind == kClipImage) {
    // Prefer PNG; keep CF_DIB as a fallback for servers without PNG support.
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

// Runs the default handler first (it initialises the GFX pipeline), then captures
// the cliprdr channel interface.
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
    // The wrappers feed the frame work meter and are shared with the offline
    // replay so both report the same "本机" (hmrdp_gfx_work.h).
    RdpgfxClientContext* gfx = reinterpret_cast<RdpgfxClientContext*>(e->pInterface);
    GfxWorkInstall(gfx, ctx->session->workMeter());
    // START_FRAME is still *before* the frame's writes, so the previous frame's GPU
    // copy out of the zero-copy desktop buffer must drain here, not in BeginPaint.
    Session* session = ctx->session;
    GfxWorkSetFrameBeginHook(gfx, [session]() { session->HandleFrameBegin(); });
    // Offload only once fully connected: a failure during the connect sequence tears
    // the channels down inside freerdp_connect and would race the worker.
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

  // No-op unless the user enabled the capture; harmless without the HmRdp patch.
  if (HmrdpSetGfxRawCapture != nullptr) {
    HmrdpSetGfxRawCapture(&HmrdpGfxRawCapture);
  }
  if (HmrdpSetThreadQoSApplier != nullptr) {
    HmrdpSetThreadQoSApplier(&HmrdpThreadQoSHook);
    HMRDP_LOGI("qos: applier registered, libqos.so %{public}s",
               hmrdp::ResolveQoS() != nullptr ? "available" : "unavailable");
  }

  // Without these subscriptions the GFX surface commands are never decoded.
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
  // gdi composes into the presenter's desktop buffer when the surface is already up;
  // otherwise it owns its buffer and HandleEndPaint moves it over.
  if (!InitGdiWithPresenter(instance, pre->session != nullptr ? pre->session->presenter() : nullptr,
                            desktopWidth, desktopHeight)) {
    return FALSE;
  }
  context->update->BeginPaint = HmrdpBeginPaint;
  context->update->EndPaint = HmrdpEndPaint;
  context->update->DesktopResize = HmrdpDesktopResize;
  context->update->PlaySound = HmrdpPlaySound;
  // Consume the server's lock-key indicator updates instead of echoing them back.
  context->update->SetKeyboardIndicators = HmrdpSetKeyboardIndicators;

  // Take over cursor handling so the remote pointer shape drives the HarmonyOS
  // system cursor; without it the server's pointer updates are dropped.
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
    // Move the frame pipeline off the drdynvc dispatch thread so audio and input
    // queued on it are no longer held behind a frame.
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
    // The GFX worker was joined before the disconnect, so the wrapped callbacks
    // can be put back here before FreeRDP frees the channel interface.
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
  // Two session windows can connect at once, so serialise the shared g_entryPoints.
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

// 超分: output size / upscale ratio, rounded to an even pixel count (some servers
// render odd desktop sizes badly). Mirrors SettingsStore.srSessionSize.
int SuperResolutionSessionSize(int output, int ratioPercent) {
  if (output <= 0 || ratioPercent <= 100) {
    return output;
  }
  const int scaled = (output * 100 + ratioPercent / 2) / ratioPercent;
  const int even = scaled - (scaled % 2);
  return even > 0 ? even : 2;
}

}  // namespace

namespace {

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
  // A lease is only handed out for a live instance; once teardown has started or
  // the instance is gone, callers get an invalid lease and must do nothing.
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
  // Account for a frame that actually reached the surface (telemetry / first frame).
  pipeline_.host()->SetPresentedFn(
      [this](bool presented, uint64_t, const PresentUploadInfo&) {
        if (presented) {
          AfterPresent();
        }
      });
  EnsureEntryPoints();
}

Session::~Session() {
  // Teardown() owns the context, so a live instance here means it never ran.
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
    if (index >= EventData::kMaxValues) {
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

  // Average RTT is steady-state; base RTT is the fallback before it is reported.
  const uint32_t averageRtt = netCharAverageRtt_.load();
  const uint32_t baseRtt = netCharBaseRtt_.load();
  const uint32_t measured = averageRtt != 0 ? averageRtt : baseRtt;
  sample.rttMs = measured != 0 ? static_cast<int32_t>(measured) : -1;

  // Drained on the RDP thread so the window is exactly one tick and never races
  // the pipeline's teardown.
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
  // One Session per attempt, never reused: kIdle -> kStarting is the only entry.
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
  // The client does not store network characteristics internally, so capture them
  // via the public autodetect callback.
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
  // 超分: the server renders output / upscale ratio and the presenter upscales back;
  // without it the session desktop *is* the output.
  int sessionWidth = options.width;
  int sessionHeight = options.height;
  desktopMagnification_ = 1.0;
  if (options.srEnabled && options.srRatioPercent > 100) {
    sessionWidth = SuperResolutionSessionSize(options.width, options.srRatioPercent);
    sessionHeight = SuperResolutionSessionSize(options.height, options.srRatioPercent);
    if (sessionWidth > 0) {
      // The pointer bitmap event carries this so ArkTS magnifies the cursor by the
      // same factor (HandlePointerShape).
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
  // Credentials go through the settings API, never the command line, so they cannot
  // leak via the parser's failure logging.
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
  // Clipboard redirection is always on, triggered manually from the toolbar.
  freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
  // Without the channel no PCM ever arrives, so an unsupported device stays silent
  // rather than failing.
  freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, options.enableAudio);
  // GFX (RDPGFX) is mandatory: without it the session does not render at all.
  freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
  // RemoteFX Progressive is the desktop codec; H.264/AVC444 stay off
  // (see doc_agent/gfx-engine.md §3).
  freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
  freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
  freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE);
  freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE);
  freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, TRUE);
  // RDPEI (touch/pen) channel, so ArkUI touch events forward as native remote touch.
  freerdp_settings_set_bool(settings, FreeRDP_MultiTouchInput, TRUE);
  // The UI only offers the fixed Windows presets, so the value passes through as-is.
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
  // The RDP thread holds a shared reference and performs the teardown itself, so
  // neither the context nor this object can be freed from under it.
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
    // Unreachable in practice, but finish the lifecycle so WaitDestroyed() cannot time out.
    MarkDestroyed();
    return;
  }
  // Mark this thread like the frame pipeline (HarmonyOS QoS).
  HmrdpThreadQoSHook();
  const BOOL ok = freerdp_connect(instance);
  if (!ok) {
    // freerdp_connect() already ran freerdp_disconnect() on failure, so it is
    // deliberately not called again; the attempt's terminal event is this single kError.
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
      // Wake only for transport work or the next telemetry sample; the loop is
      // event-driven, so fixed-period polling would wake it needlessly while idle.
      DWORD timeoutMs = 0;
      if (!metricsStarted_) {
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

      // Runs on this (RDP) thread so draining the work meter / audio / GPU sources
      // cannot race the pipeline's teardown; ArkTS pulls the stored sample.
      if (!metricsStarted_ || NowMs() - lastMetricsTick_ >= 1000) {
        SampleTelemetry();
      }
    }
  }

  // The server's ErrorInfo PDU is more specific than the client-side last error, so
  // it wins; a user-requested stop is reported as an empty payload.
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

  // Join the GFX worker before the disconnect: it parses into the rdpgfx plugin,
  // which freerdp_disconnect frees (doc_agent/architecture.md §4.3).
  StopGfxWorker();
  freerdp_disconnect(instance);
  EmitError(SessionEvent::kDisconnected, lastErrorCode_, lastError_);
  Teardown();
}

void Session::Disconnect() {
  // Runs on the UI thread and only *signals*: the RDP thread owns the teardown, so
  // the context is never freed while that thread may still be using it.
  stopRequested_ = true;
  {
    const SessionPhase phase = phase_.load();
    if (phase == SessionPhase::kStarting || phase == SessionPhase::kConnected) {
      SessionPhase expected = phase;
      phase_.compare_exchange_strong(expected, SessionPhase::kStopping);
    }
  }
  std::lock_guard<std::mutex> lock(lifecycleMutex_);
  if (instance_ != nullptr && instance_->context != nullptr) {
    // The abort event wakes both a running connect and an established loop;
    // signalled under the lock so Teardown() cannot free the context in between.
    freerdp_abort_connect_context(instance_->context);
  }
}

void Session::Teardown() {
  // Sole owner of the context: stops handing out leases, waits for the outstanding
  // ones, then frees, so no other thread holds it when it goes away
  // (doc_agent/architecture.md §4.3).
  freerdp* instance = nullptr;
  {
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    tearingDown_ = true;
    instance = instance_;
    instance_ = nullptr;
    cliprdr_ = nullptr;
    // Lease holders only make short, bounded calls, but the wait is unbounded: the
    // context must never be freed under a live lease.
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
  clipboardReady_ = false;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    // The channel is gone, so a pending download request can never be answered.
    fileContentsPending_ = false;
  }
  audio_.Reset();
  pipeline_.host()->DetachDesktopBuffer();
  if (pipeline_.presenter() != nullptr) {
    pipeline_.presenter()->Reset();
  }
  MarkDestroyed();
}

void Session::HandlePostConnect() {
  Emit(SessionEvent::kConnected);
}

// Defensive high-water mark: the queue cannot drop chunks (every one must reach
// the parser), so crossing this only warns.
constexpr size_t kGfxQueueHighWaterBytes = 64 * 1024 * 1024;

void Session::HandleGfxData(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  // Stamp the raw (still ZGX-compressed) chunk on this session's meter, since the
  // capture hook has no context to find it.
  pipeline_.meter()->OnChunk(static_cast<uint32_t>(size));
  // The drdynvc dispatch thread must never be blocked: it carries every dynamic
  // channel, so waiting here would hold audio and input behind the frame pipeline.
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
  // Worker not running (before start or after stop): process inline so the chunk is
  // not lost; the lease refuses once teardown has begun.
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
  // The whole frame pipeline runs here (ZGX decompress, parse, decode, composite,
  // present), so the drdynvc dispatch thread is never held behind a frame.
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
  if (gfx != nullptr) {
    GfxWorkSetDataSinkHook(static_cast<RdpgfxClientContext*>(gfx), nullptr);
  }
  HMRDP_LOGI("gfx worker: stopped");
}

void Session::HandleFrameBegin() {
  // The zero-copy wait and its `sync` accounting live in the shared frame host, so
  // the live path and the offline replay share one implementation.
  pipeline_.host()->OnFrameBegin();
}

void Session::HandleBeginPaint() {
  // Kept even though HandleFrameBegin usually drains first: the compose of a
  // *non-mirrored* surface happens here.
  pipeline_.host()->OnBeginPaint();
}

void Session::HandleEndPaint() {
  // Taken on a FreeRDP channel thread, so the lease keeps the context (and gdi)
  // alive while this frame is presented.
  ContextGuard ctx(*this);
  if (!ctx.valid() || ctx.instance()->context == nullptr) {
    return;
  }
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
  // release it and present the dirty region; shared with the offline CPU replay.
  uint64_t presentUs = 0;
  (void)pipeline_.host()->OnEndPaint(gdi, &presentUs, nullptr);
}

void Session::AfterPresent() {
  // The shared frame host charges the meter (work and blocked halves), so the live
  // session and the replay cannot report it differently.
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
  // ArkTS owns target size, hotspot scaling and dedup; native ships the bitmap as
  // decoded plus the magnification and a content hash so identical resends are dropped.
  const uint32_t hash = HashBgra(bgra.data(), bgra.size());
  const double magnification = desktopMagnification_ > 1.0 ? desktopMagnification_ : 1.0;
  const int64_t magMilli = static_cast<int64_t>(magnification * 1000.0 + 0.5);
  EmitValues(SessionEvent::kCursorShape,
             {width, height, hotX, hotY, magMilli, static_cast<int64_t>(hash)}, bgra);
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
    // FreeRDP closes the channels right after this callback, so drop the pointer
    // under the lock and wait out the leases before the channel is freed.
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
  // Diagnostic: without the RDPEI channel FreeRDP silently degrades touch to mouse
  // emulation, visible as stray clicks while dragging.
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
    // Lower value = higher rate; the high-rate option uses 8ms (125Hz) rather than
    // 0, which would let the network alone bound a drag.
    HmrdpSetTouchFrameInterval(enabled ? kTouchFrameIntervalHighRateMs
                                       : kTouchFrameIntervalDefaultMs);
  }
}

void Session::SetRdpCursor(bool enabled) {
  g_useRdpCursor.store(enabled);
}

void Session::SetHardwareAccel(bool enabled) {
  // The presenter backend reads this (hmrdp_presenter.h) when the next presenter
  // is created.
  hmrdp::SetHardwareAccelEnabled(enabled);
}

void Session::SetGfxCapture(bool enabled, const std::string& dir) {
  const bool wantEnabled = enabled && !dir.empty();
  // GfxDumpConfigure is idempotent, so re-applying the same value does not
  // truncate a running capture.
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
// The Client Synchronize PDU also carries a Kana bit, which nothing here can produce.
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
  // The input path calls this on every key event, so dedup the values already sent.
  if (localLocksKnown_.load() && localLocks_.load() == locks) {
    return true;
  }
  localLocks_.store(locks);
  localLocksKnown_.store(true);
  return SendSynchronize(locks);
}

bool Session::ReassertLockState(uint32_t flags) {
  // Unconditional: the SendLockState() dedup is deliberately bypassed here.
  return SendSynchronize(flags & kLockMask);
}

void Session::HandleRemoteLockState(uint32_t ledFlags) {
  // Native only relays the server's report; whether to correct the local keyboard
  // is ArkTS's decision (see kRemoteLockState / ReassertLockState).
  EmitValues(SessionEvent::kRemoteLockState, {ledFlags & kLockMask});
}

void Session::HandleCliprdrConnected(CliprdrClientContext* cliprdr) {
  if (cliprdr == nullptr) {
    return;
  }
  {
    // Refuse to publish if teardown has already begun.
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
  // Deliberately NOT asking for CB_CAN_LOCK_CLIPDATA: once locking is negotiated
  // every file-contents request must echo the server's clipDataId or fail; FreeRDP's
  // own file-transfer client leaves the same bit unset.
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
  // Advertise even when the local clipboard is empty: the server needs to know what
  // the client accepts before offering its own content.
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
  // Any change supersedes a download in progress; ArkTS owns that state machine and
  // aborts its own transfer.
  Emit(SessionEvent::kRemoteClipboardChanged);
  // Candidates are "kind:id,kind:id"; ArkTS picks (pull priority, single-in-flight);
  // native only prefers DIB over DIBV5 in the wire form.
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
  // Copy the representation out under the lock, then send without holding it.
  std::vector<BYTE> payload;
  bool servedFiles = false;
  size_t servedFileCount = 0;
  {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    const UINT32 id = request->requestedFormatId;
    // Both advertised file-clipboard ids carry the descriptor list, and FileContents
    // has no data format behind it, so accept either id the peer picked.
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
      EmitValues(SessionEvent::kFileTransferDone, {static_cast<int64_t>(servedFileCount)});
      WithdrawLocalFileClipboard();
    }
  }
  return rc;
}

UINT Session::OnCliprdrServerFormatDataResponse(
    const CLIPRDR_FORMAT_DATA_RESPONSE* response) {
  if (!remoteRequestInFlight_) {
    // Ignore stray/duplicate responses instead of mis-decoding them as text.
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
  } else if (kind == LocalClipKind::kFiles) {
    std::vector<uint8_t> descriptor(data, data + size);
    EmitBytes(SessionEvent::kRemoteFileListRaw, descriptor);
  } else if (kind == LocalClipKind::kImage) {
    std::vector<uint8_t> dib(data, data + size);
    EmitBytes(SessionEvent::kClipboardImage, dib);
  } else if (kind == LocalClipKind::kHtml) {
    std::vector<uint8_t> html(data, data + size);
    EmitBytes(SessionEvent::kClipboardHtml, html);
  } else if (kind == LocalClipKind::kRtf) {
    std::vector<uint8_t> rtf(data, data + size);
    EmitBytes(SessionEvent::kClipboardRtf, rtf);
  } else if (kind == LocalClipKind::kText && size >= sizeof(WCHAR)) {
    // CF_UNICODETEXT is NUL-terminated UTF-16LE; terminate even if the server omitted it.
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

  // Always settle, so ArkTS can issue the request it deferred while this one was in flight.
  EmitValues(SessionEvent::kRemoteFormatResponse,
             {static_cast<int64_t>(kind), failed ? 1 : 0});
  return CHANNEL_RC_OK;
}

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
  // Keep the cliprdr channel alive through the advertise below.
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
  // Tell the UI the upload size so it can show a progress bar before the pull starts.
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
      // Mid-transfer abort: the list cannot be withdrawn while the server reads it,
      // so stop serving and let the remote paste report a protocol error.
      localClipboardFilePaths_.clear();
      localTransferDone_ = 0;
      localTransferTotal_ = 0;
      localClipKind_ = LocalClipKind::kNone;
    }
  }
  if (!pulled) {
    // Nothing pulled yet (or the transfer already finished), so withdraw cleanly.
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
    // Replace the file list with an empty text clipboard: an empty FORMAT_LIST would
    // be ignored by the channel, so a later paste finds nothing instead of erroring.
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
    // Only range responses move the upload forward; a size response must not be
    // counted, or `done` would overshoot `total`.
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
    // Withdraw once every advertised byte is served, so a second remote paste does
    // not pull the same files again.
    WithdrawLocalFileClipboard();
  }
  return rc;
}

}  // namespace hmrdp
