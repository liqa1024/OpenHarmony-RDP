/*
 * HmRdp - HarmonyOS RDP client
 * FreeRDP client session implementation.
 */
#include "hmrdp_session.h"

#include <cstring>
#include <sstream>
#include <vector>

#include <freerdp/channels/channels.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/gdi/gdi.h>
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

namespace hmrdp {
namespace {

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

// Advertises the local clipboard as CF_UNICODETEXT to the server. The channel
// owns the ClientFormatList sender, so this only builds the format list.
UINT SendCliprdrFormatList(CliprdrClientContext* cliprdr) {
  if (cliprdr == nullptr || cliprdr->ClientFormatList == nullptr) {
    return CHANNEL_RC_OK;
  }
  HMRDP_LOGI("cliprdr sending local format list (CF_UNICODETEXT)");
  CLIPRDR_FORMAT format = {};
  format.formatId = CF_UNICODETEXT;
  format.formatName = nullptr;
  CLIPRDR_FORMAT_LIST formatList = {};
  formatList.common.msgType = CB_FORMAT_LIST;
  formatList.common.msgFlags = 0;
  formatList.numFormats = 1;
  formatList.formats = &format;
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
  freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, options.enableAudio);
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

void Session::HandlePostDisconnect() {
  clipboardReady_ = false;
  cliprdr_ = nullptr;
}

void Session::OnAudioData(const void* data, size_t size, int sampleRate, int channels) {
  if (audioFn_ && data != nullptr && size > 0) {
    audioFn_(data, size, sampleRate, channels);
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
  return freerdp_client_handle_touch(client, flags, finger, pressure, x, y) ? true : false;
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
  // the local clipboard is empty: the server needs to know the client accepts
  // CF_UNICODETEXT before it will offer its own clipboard content.
  SendCliprdrFormatList(cliprdr_);
  return CHANNEL_RC_OK;
}

UINT Session::OnCliprdrServerFormatList(const CLIPRDR_FORMAT_LIST* formatList) {
  if (cliprdr_ == nullptr || formatList == nullptr ||
      cliprdr_->ClientFormatDataRequest == nullptr) {
    return CHANNEL_RC_OK;
  }
  HMRDP_LOGI("cliprdr server format list: %{public}u formats", formatList->numFormats);
  for (UINT32 i = 0; i < formatList->numFormats; i++) {
    HMRDP_LOGI("  format[%{public}u] id=%{public}u", i, formatList->formats[i].formatId);
    if (formatList->formats[i].formatId != CF_UNICODETEXT) {
      continue;
    }
    CLIPRDR_FORMAT_DATA_REQUEST request = {};
    request.common.msgType = CB_FORMAT_DATA_REQUEST;
    request.common.msgFlags = 0;
    request.requestedFormatId = CF_UNICODETEXT;
    HMRDP_LOGI("cliprdr requesting CF_UNICODETEXT");
    return cliprdr_->ClientFormatDataRequest(cliprdr_, &request);
  }
  return CHANNEL_RC_OK;
}

UINT Session::OnCliprdrServerFormatDataRequest(
    const CLIPRDR_FORMAT_DATA_REQUEST* request) {
  if (cliprdr_ == nullptr || request == nullptr ||
      cliprdr_->ClientFormatDataResponse == nullptr) {
    return CHANNEL_RC_OK;
  }
  HMRDP_LOGI("cliprdr server data request: format=%{public}u", request->requestedFormatId);
  // Copy the local text out under the lock, then send without holding it.
  std::vector<BYTE> payload;
  if (request->requestedFormatId == CF_UNICODETEXT) {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    if (localClipboardValid_ && !localClipboardUtf16_.empty()) {
      payload.assign(localClipboardUtf16_.begin(), localClipboardUtf16_.end());
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
  if (response == nullptr || (response->common.msgFlags & CB_RESPONSE_FAIL) != 0) {
    HMRDP_LOGW("cliprdr data response failed or null");
    return CHANNEL_RC_OK;
  }
  const BYTE* data = response->requestedFormatData;
  const UINT32 size = response->common.dataLen;
  HMRDP_LOGI("cliprdr data response: size=%{public}u", size);
  if (data == nullptr || size < sizeof(WCHAR)) {
    return CHANNEL_RC_OK;
  }
  // CF_UNICODETEXT is a NUL-terminated UTF-16LE string; make sure the buffer we
  // hand to the converter is terminated even if the server omitted the NUL.
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

void Session::SetLocalClipboardText(const std::string& utf8) {
  if (!clipboardEnabled_.load()) {
    return;
  }
  HMRDP_LOGI("set local clipboard: %{public}u bytes, cliprdr=%{public}d ready=%{public}d",
             static_cast<unsigned>(utf8.size()), cliprdr_ != nullptr ? 1 : 0, clipboardReady_.load() ? 1 : 0);
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
  }
  free(wide);
  if (cliprdr_ != nullptr && clipboardReady_.load()) {
    SendCliprdrFormatList(cliprdr_);
  }
}

}  // namespace hmrdp
