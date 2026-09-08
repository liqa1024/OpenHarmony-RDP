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
#include <freerdp/client/cmdline.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <freerdp/utils/signal.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/wlog.h>

#include "hmrdp_log.h"

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

BOOL HmrdpPlaySound(rdpContext* context, const PLAY_SOUND_UPDATE* playSound) {
  return TRUE;
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

  // The graphics pipeline (RDPGFX) callbacks are registered by the client
  // library when the rdpgfx channel connects. Without these subscriptions the
  // GFX surface commands are never decoded and the screen stays black.
  PubSub_SubscribeChannelConnected(instance->context->pubSub,
                                   freerdp_client_OnChannelConnectedEventHandler);
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
  PubSub_UnsubscribeChannelConnected(instance->context->pubSub,
                                     freerdp_client_OnChannelConnectedEventHandler);
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

void HmrdpClientFree(freerdp* instance, rdpContext* context) {}

int HmrdpClientStart(rdpContext* context) { return 0; }
int HmrdpClientStop(rdpContext* context) { return 0; }

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
}

void AppendArg(std::vector<std::string>& args, const std::string& value) {
  args.push_back(value);
}

}  // namespace

Session::Session(int64_t id) : id_(id) {
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
  lastError_ = error;
  Emit(SessionEvent::kError, error);
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
    const char* error = freerdp_get_last_error_string(
        static_cast<UINT32>(freerdp_get_last_error(instance->context)));
    SetError(error != nullptr ? error : "connection failed");
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
          const char* error = freerdp_get_last_error_string(
              static_cast<UINT32>(freerdp_get_last_error(instance->context)));
          if (error != nullptr) {
            lastError_ = error;
          }
        }
        break;
      }
    }
  }

  freerdp_disconnect(instance);
  connected_ = false;
  Emit(SessionEvent::kDisconnected, lastError_);
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
  running_ = false;
  connected_ = false;
  renderer_.Reset();
}

void Session::HandlePostConnect() {
  rdpSettings* settings = instance_->context->settings;
  const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  renderer_.SetDesktopSize(static_cast<int>(width), static_cast<int>(height));
  connected_ = true;
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
  connected_ = false;
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

bool Session::SendExtendedMouse(uint16_t flags, uint16_t x, uint16_t y) {
  if (instance_ == nullptr || instance_->context == nullptr ||
      instance_->context->input == nullptr) {
    return false;
  }
  return freerdp_input_send_extended_mouse_event(instance_->context->input, flags, x, y);
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

bool Session::SendSynchronize() {
  if (instance_ == nullptr || instance_->context == nullptr ||
      instance_->context->input == nullptr) {
    return false;
  }
  return freerdp_input_send_synchronize_event(instance_->context->input, 0);
}

void Session::RequestResize(int width, int height) {
  if (instance_ == nullptr || instance_->context == nullptr) {
    return;
  }
  rdpSettings* settings = instance_->context->settings;
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
                              static_cast<uint32_t>(width));
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                              static_cast<uint32_t>(height));
  renderer_.SetDesktopSize(width, height);
}

void Session::SetClipboardEnabled(bool enabled) {
  clipboardEnabled_ = enabled;
}

}  // namespace hmrdp
