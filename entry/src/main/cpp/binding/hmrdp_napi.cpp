#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <napi/native_api.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "binding/handle_registry.h"
#include "hmrdp_log.h"
#include "hmrdp_bitmap.h"
#include "hmrdp_napi_util.h"
#include "hmrdp_replay.h"
#include "hmrdp_session.h"
#include "hmrdp_vk_context.h"

using namespace hmrdp::binding;

namespace {

using hmrdp::RdpOptions;
using hmrdp::Session;
using hmrdp::SessionEvent;
using hmrdp::binding::HandleRegistry;

HandleRegistry<Session> g_sessions;
std::mutex g_windowMutex;
std::map<int64_t, OHNativeWindow*> g_windows;
constexpr uint32_t kTeardownTimeoutMs = 2000;

napi_threadsafe_function g_eventTsfn = nullptr;

HandleRegistry<hmrdp::GfxReplay> g_replays;

struct NativeBuffer {
  std::vector<uint8_t> bytes;
};
HandleRegistry<NativeBuffer> g_buffers;

struct EventPayload {
  int64_t handle;
  int event;
  Session::EventData data;
  std::vector<uint8_t> bytes;
};

napi_value MakeEventPayload(napi_env env, int event, const Session::EventData& payload,
                            const std::vector<uint8_t>& bytes) {
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  auto setInt = [env, obj](const char* name, int64_t value) {
    napi_value v = nullptr;
    napi_create_int64(env, value, &v);
    napi_set_named_property(env, obj, name, v);
  };
  auto setStr = [env, obj](const char* name, const std::string& value) {
    napi_value v = nullptr;
    napi_create_string_utf8(env, value.c_str(), value.size(), &v);
    napi_set_named_property(env, obj, name, v);
  };
  auto setBool = [env, obj](const char* name, bool value) {
    napi_value v = nullptr;
    napi_get_boolean(env, value, &v);
    napi_set_named_property(env, obj, name, v);
  };
  auto value = [&payload](int index) {
    return index >= 0 && index < static_cast<int>(Session::EventData::kMaxValues)
               ? payload.values[index]
               : static_cast<int64_t>(0);
  };
  auto setBuffer = [&](const char* name) {
    if (!bytes.empty()) {
      auto buffer = std::make_shared<NativeBuffer>();
      buffer->bytes = bytes;
      setInt(name, g_buffers.Add(buffer));
    }
  };

  switch (event) {
    case static_cast<int>(SessionEvent::kError):
    case static_cast<int>(SessionEvent::kDisconnected):
      setInt("code", value(0));
      setStr("message", payload.text);
      break;
    case static_cast<int>(SessionEvent::kResize):
      setInt("width", value(0));
      setInt("height", value(1));
      break;
    case static_cast<int>(SessionEvent::kClipboardText):
      setStr("text", payload.text);
      break;
    case static_cast<int>(SessionEvent::kClipboardHtml):
    case static_cast<int>(SessionEvent::kClipboardRtf):
    case static_cast<int>(SessionEvent::kRemoteFileListRaw):
      setBuffer("dataHandle");
      break;
    case static_cast<int>(SessionEvent::kRemoteFileChunk):
      setInt("listIndex", value(0));
      setBool("isSize", value(1) != 0);
      setInt("sizeValue", value(2));
      setBool("failed", value(3) != 0);
      setBuffer("dataHandle");
      break;
    case static_cast<int>(SessionEvent::kCursorShape):
      setInt("width", value(0));
      setInt("height", value(1));
      setInt("hotX", value(2));
      setInt("hotY", value(3));
      setInt("magnificationMilli", value(4));
      setInt("shapeHash", value(5));
      setBuffer("imageHandle");
      break;
    case static_cast<int>(SessionEvent::kClipboardImage):
    case static_cast<int>(SessionEvent::kClipboardImagePng):
      setBuffer("imageHandle");
      break;
    case static_cast<int>(SessionEvent::kFileTransferProgress):
      setInt("direction", value(0));
      setInt("doneBytes", value(1));
      setInt("totalBytes", value(2));
      setInt("fileIndex", value(3));
      setInt("fileCount", value(4));
      break;
    case static_cast<int>(SessionEvent::kFileTransferFailed):
      setStr("reason", payload.text);
      break;
    case static_cast<int>(SessionEvent::kFileTransferDone):
      setInt("fileCount", value(0));
      break;
    case static_cast<int>(SessionEvent::kKeyIntercept):
      setInt("keyCode", value(0));
      setBool("down", value(1) != 0);
      break;
    case static_cast<int>(SessionEvent::kAudioFormat):
      setInt("audioRate", value(0));
      setInt("audioChannels", value(1));
      break;
    case static_cast<int>(SessionEvent::kRemoteFormatList):
      setStr("formatCandidates", payload.text);
      break;
    case static_cast<int>(SessionEvent::kRemoteFormatResponse):
      setInt("clipKind", value(0));
      setBool("failed", value(1) != 0);
      break;
    case static_cast<int>(SessionEvent::kRemoteLockState):
      setInt("lockFlags", value(0));
      break;
    default:
      break;
  }
  return obj;
}

void CallJsEvent(napi_env env, napi_value jsCallback, void*, void* data) {
  std::unique_ptr<EventPayload> payload(static_cast<EventPayload*>(data));
  if (env == nullptr || jsCallback == nullptr || payload == nullptr) {
    return;
  }
  napi_value handleValue = nullptr;
  napi_value eventValue = nullptr;
  napi_create_int64(env, payload->handle, &handleValue);
  napi_create_int32(env, payload->event, &eventValue);
  napi_value payloadValue = MakeEventPayload(env, payload->event, payload->data, payload->bytes);
  napi_value argv[3] = {handleValue, eventValue, payloadValue};
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_call_function(env, global, jsCallback, 3, argv, nullptr);
}

void OnSessionEvent(int64_t handle, SessionEvent event, const Session::EventData& data,
                    const std::vector<uint8_t>& bytes) {
  if (g_eventTsfn == nullptr) {
    return;
  }
  EventPayload* payload = new EventPayload{handle, static_cast<int>(event), data, bytes};
  const napi_status status =
      napi_call_threadsafe_function(g_eventTsfn, payload, napi_tsfn_nonblocking);
  if (status != napi_ok) {
    delete payload;
  }
}

std::shared_ptr<Session> FindSession(int64_t handle) {
  return g_sessions.Find(handle);
}

void DestroyWindow(int64_t handle) {
  std::lock_guard<std::mutex> lock(g_windowMutex);
  auto it = g_windows.find(handle);
  if (it == g_windows.end()) {
    return;
  }
  if (it->second != nullptr) {
    OH_NativeWindow_DestroyNativeWindow(it->second);
  }
  g_windows.erase(it);
}

void SecureErase(std::string& value) {
  if (!value.empty()) {
    volatile char* data = &value[0];
    for (size_t i = 0; i < value.size(); ++i) {
      data[i] = '\0';
    }
  }
  value.clear();
}

// The window buffer is CPU-accessible by default, but CPU reads cost energy, and this
// client never reads it, so the read bits are dropped; CPU_WRITE stays for the GLES fallback.
void PreferGpuWindowBuffer(OHNativeWindow* window) {
  if (window == nullptr) {
    return;
  }
  uint64_t usage = 0;
  if (OH_NativeWindow_NativeWindowHandleOpt(window, GET_USAGE, &usage) != 0) {
    return;
  }
  const uint64_t reduced =
      usage & ~(static_cast<uint64_t>(NATIVEBUFFER_USAGE_CPU_READ) |
                static_cast<uint64_t>(NATIVEBUFFER_USAGE_CPU_READ_OFTEN));
  if (reduced == usage) {
    return;
  }
  if (OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, reduced) != 0) {
    HMRDP_LOGW("window usage: SET_USAGE failed, keeping 0x%{public}llx",
               static_cast<unsigned long long>(usage));
    return;
  }
  HMRDP_LOGI("window usage: 0x%{public}llx -> 0x%{public}llx (cpu read off)",
             static_cast<unsigned long long>(usage),
             static_cast<unsigned long long>(reduced));
}

napi_value CreateSession(napi_env env, napi_callback_info) {
  auto session = std::make_shared<Session>();
  const int64_t id = g_sessions.Add(session);
  session->SetEventFn([id](SessionEvent event, const Session::EventData& data,
                           const std::vector<uint8_t>& bytes) {
    OnSessionEvent(id, event, data, bytes);
  });
  // Delivered only while this session is the focused interceptor owner.
  RegisterInterceptSink(id, [id](int32_t keyCode, bool down) {
    Session::EventData data;
    data.values[0] = keyCode;
    data.values[1] = down ? 1 : 0;
    OnSessionEvent(id, SessionEvent::kKeyIntercept, data, {});
  });
  return CreateInt(env, id);
}

napi_value DestroySession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  UnregisterInterceptSink(handle);
  std::shared_ptr<Session> session = g_sessions.Take(handle);
  if (session == nullptr) {
    return CreateUndefined(env);
  }
  bool destroyed = true;
  if (session) {
    session->Disconnect();
    destroyed = session->WaitDestroyed(kTeardownTimeoutMs);
    if (!destroyed) {
      HMRDP_LOGW("destroySession %{public}d: teardown still running after %{public}dms",
                 static_cast<int>(handle), kTeardownTimeoutMs);
    }
  }
  if (destroyed) {
    DestroyWindow(handle);
  }
  return CreateUndefined(env);
}

napi_value Connect(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 2 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateBool(env, false);
  }
  RdpOptions options;
  options.host = GetStringProperty(env, args[1], "host");
  options.port = GetIntProperty(env, args[1], "port", 3389);
  options.username = GetStringProperty(env, args[1], "username");
  options.password = GetStringProperty(env, args[1], "password");
  options.domain = GetStringProperty(env, args[1], "domain");
  options.width = GetIntProperty(env, args[1], "width", 1920);
  options.height = GetIntProperty(env, args[1], "height", 1080);
  options.streamWidth = GetIntProperty(env, args[1], "streamWidth", 0);
  options.streamHeight = GetIntProperty(env, args[1], "streamHeight", 0);
  options.scalePercent = GetIntProperty(env, args[1], "scalePercent", 0);
  options.ignoreCertificate = GetBoolProperty(env, args[1], "ignoreCertificate", true);
  options.enableAudio = GetBoolProperty(env, args[1], "enableAudio", true);
  options.srBackend = GetIntProperty(env, args[1], "srBackend", 0) == 1
                          ? hmrdp::SuperResolutionBackend::kFsr
                          : hmrdp::SuperResolutionBackend::kXengine;
  options.gatewayHost = GetStringProperty(env, args[1], "gatewayHost");
  options.gatewayPort = GetIntProperty(env, args[1], "gatewayPort", 443);
  options.gatewayUsername = GetStringProperty(env, args[1], "gatewayUsername");
  options.gatewayPassword = GetStringProperty(env, args[1], "gatewayPassword");
  options.gatewayDomain = GetStringProperty(env, args[1], "gatewayDomain");

  if (options.host.empty()) {
    return CreateBool(env, false);
  }

  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    SecureErase(options.password);
    SecureErase(options.gatewayPassword);
    return CreateBool(env, false);
  }
  const bool started = session->Connect(options);
  SecureErase(options.password);
  SecureErase(options.gatewayPassword);
  return CreateBool(env, started);
}

napi_value Disconnect(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session != nullptr) {
    session->Disconnect();
  }
  return CreateUndefined(env);
}

napi_value SetSurface(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 4 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  size_t length = 0;
  napi_get_value_string_utf8(env, args[1], nullptr, 0, &length);
  std::string surfaceId(length, '\0');
  napi_get_value_string_utf8(env, args[1], surfaceId.data(), length + 1, &length);
  surfaceId.resize(length);
  int32_t width = 0;
  int32_t height = 0;
  napi_get_value_int32(env, args[2], &width);
  napi_get_value_int32(env, args[3], &height);

  const uint64_t sid = static_cast<uint64_t>(strtoull(surfaceId.c_str(), nullptr, 10));
  OHNativeWindow* window = nullptr;
  const int32_t err = OH_NativeWindow_CreateNativeWindowFromSurfaceId(sid, &window);
  if (err != 0 || window == nullptr) {
    HMRDP_LOGE("CreateNativeWindowFromSurfaceId failed: %{public}d", err);
    return CreateUndefined(env);
  }
  PreferGpuWindowBuffer(window);
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    OH_NativeWindow_DestroyNativeWindow(window);
    return CreateUndefined(env);
  }
  DestroyWindow(handle);
  {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_windows[handle] = window;
  }
  session->presenter()->SetSurface(window, width, height);
  session->presenter()->Prepare();
  HMRDP_LOGI("session %{public}d surface bound %{public}dx%{public}d",
             static_cast<int>(handle), width, height);
  return CreateUndefined(env);
}

napi_value UpdateSurface(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t width = 0;
  int32_t height = 0;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  napi_get_value_int32(env, args[1], &width);
  napi_get_value_int32(env, args[2], &height);
  std::shared_ptr<Session> session = FindSession(handle);
  if (session != nullptr) {
    session->presenter()->ResizeSurface(width, height);
  }
  return CreateUndefined(env);
}

napi_value ClearSurface(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session != nullptr) {
    session->presenter()->DestroySurface();
  }
  DestroyWindow(handle);
  return CreateUndefined(env);
}

napi_value SendMouse(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t flags = 0;
  int32_t x = 0;
  int32_t y = 0;
  if (argc < 4 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int32(env, args[1], &flags) != napi_ok ||
      napi_get_value_int32(env, args[2], &x) != napi_ok ||
      napi_get_value_int32(env, args[3], &y) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  const uint16_t ux = static_cast<uint16_t>(x < 0 ? 0 : x);
  const uint16_t uy = static_cast<uint16_t>(y < 0 ? 0 : y);
  return CreateBool(env,
                    session->SendMouse(static_cast<uint16_t>(flags), ux, uy));
}

napi_value SetSuppressed(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  bool suppressed = false;
  if (argc < 2 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_bool(env, args[1], &suppressed) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env, session->SetSuppressed(suppressed));
}

napi_value SendTouch(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value args[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t flags = 0;
  int32_t finger = 0;
  int32_t pressure = 0;
  int32_t x = 0;
  int32_t y = 0;
  if (argc < 6 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int32(env, args[1], &flags) != napi_ok ||
      napi_get_value_int32(env, args[2], &finger) != napi_ok ||
      napi_get_value_int32(env, args[3], &pressure) != napi_ok ||
      napi_get_value_int32(env, args[4], &x) != napi_ok ||
      napi_get_value_int32(env, args[5], &y) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env, session->SendTouch(static_cast<uint32_t>(flags), finger,
                                            static_cast<uint32_t>(pressure), x, y));
}

napi_value SendKey(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t scancode = 0;
  bool down = false;
  bool extended = false;
  if (argc < 4 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int32(env, args[1], &scancode) != napi_ok ||
      napi_get_value_bool(env, args[2], &down) != napi_ok ||
      napi_get_value_bool(env, args[3], &extended) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env, session->SendKey(static_cast<uint8_t>(scancode), down, extended));
}

napi_value SendUnicode(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t codepoint = 0;
  bool down = false;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int32(env, args[1], &codepoint) != napi_ok ||
      napi_get_value_bool(env, args[2], &down) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env,
                    session->SendUnicode(static_cast<uint16_t>(codepoint), down));
}

napi_value SendLockState(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t flags = 0;
  if (argc < 2 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int32(env, args[1], &flags) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env, session->SendLockState(static_cast<uint32_t>(flags)));
}

// 快捷键穿透: route intercepted keys to `handle` (0/enabled=false releases). Returns
// whether the state is in effect; a registration failure (e.g. missing ACL) returns false.
napi_value SetShortcutPassthrough(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  bool enabled = false;
  if (argc < 2 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_bool(env, args[1], &enabled) != napi_ok) {
    return CreateBool(env, false);
  }
  return CreateBool(env, SetInterceptOwner(handle, enabled));
}

napi_value SetClipboardText(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 2 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateBool(env, false);
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, args[1], nullptr, 0, &length) != napi_ok) {
    return CreateBool(env, false);
  }
  std::string text(length, '\0');
  napi_get_value_string_utf8(env, args[1], text.data(), length + 1, &length);
  text.resize(length);
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  session->SetLocalClipboardText(text);
  return CreateBool(env, true);
}

napi_value SetClipboardHtmlRaw(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateBool(env, false);
  }
  void* cfHtmlData = nullptr;
  size_t byteCount = 0;
  if (napi_get_arraybuffer_info(env, args[1], &cfHtmlData, &byteCount) != napi_ok ||
      cfHtmlData == nullptr || byteCount == 0) {
    return CreateBool(env, false);
  }
  const std::string plainText = GetStringArgument(env, args[2]);
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  session->SetLocalClipboardHtmlRaw(static_cast<const uint8_t*>(cfHtmlData), byteCount,
                                    plainText);
  return CreateBool(env, true);
}

napi_value SetClipboardImage(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateBool(env, false);
  }
  void* dibData = nullptr;
  size_t dibLen = 0;
  if (napi_get_arraybuffer_info(env, args[1], &dibData, &dibLen) != napi_ok) {
    return CreateBool(env, false);
  }
  void* pngData = nullptr;
  size_t pngLen = 0;
  if (napi_get_arraybuffer_info(env, args[2], &pngData, &pngLen) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  session->SetLocalClipboardImage(static_cast<const uint8_t*>(dibData), dibLen,
                                  static_cast<const uint8_t*>(pngData), pngLen);
  return CreateBool(env, true);
}

bool ReadStringArg(napi_env env, napi_value value, std::string* out) {
  if (out == nullptr) {
    return false;
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    return false;
  }
  out->resize(length);
  napi_get_value_string_utf8(env, value, out->data(), length + 1, &length);
  out->resize(length);
  return true;
}

napi_value SetClipboardFileOffer(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateBool(env, false);
  }
  void* descriptorData = nullptr;
  size_t descriptorLen = 0;
  if (napi_get_arraybuffer_info(env, args[1], &descriptorData, &descriptorLen) != napi_ok ||
      descriptorData == nullptr || descriptorLen == 0) {
    return CreateBool(env, false);
  }
  std::string text;
  if (!ReadStringArg(env, args[2], &text)) {
    return CreateBool(env, false);
  }
  std::vector<std::string> paths;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    const size_t stop = end == std::string::npos ? text.size() : end;
    if (stop > start) {
      paths.push_back(text.substr(start, stop - start));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr || paths.empty()) {
    return CreateBool(env, false);
  }
  session->SetLocalFileOffer(static_cast<const uint8_t*>(descriptorData), descriptorLen,
                             paths);
  return CreateBool(env, true);
}

napi_value RequestRemoteFileContents(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value args[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int64_t listIndex = 0;
  bool sizeOnly = false;
  double offset = 0;
  int64_t length = 0;
  if (argc < 5 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int64(env, args[1], &listIndex) != napi_ok ||
      napi_get_value_bool(env, args[2], &sizeOnly) != napi_ok ||
      napi_get_value_double(env, args[3], &offset) != napi_ok ||
      napi_get_value_int64(env, args[4], &length) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr || listIndex < 0 || offset < 0 || length < 0) {
    return CreateBool(env, false);
  }
  const bool ok = session->RequestRemoteFileContents(
      static_cast<uint32_t>(listIndex), sizeOnly, static_cast<uint64_t>(offset),
      static_cast<uint32_t>(length));
  return CreateBool(env, ok);
}

napi_value CancelFileTransfer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateBool(env, false);
  }
  std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  session->CancelFileTransfer();
  return CreateBool(env, true);
}

napi_value GetSessionTelemetry(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  napi_value result = nullptr;
  napi_create_object(env, &result);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return result;
  }
  std::shared_ptr<Session> session = FindSession(handle);
  const hmrdp::SessionTelemetry telemetry =
      session != nullptr ? session->ReadTelemetry() : hmrdp::SessionTelemetry();
  auto setInt = [env, result](const char* name, int64_t value) {
    napi_value v = nullptr;
    napi_create_int64(env, value, &v);
    napi_set_named_property(env, result, name, v);
  };
  setInt("sampleSeq", static_cast<int64_t>(telemetry.sampleSeq));
  setInt("windowElapsedMs", static_cast<int64_t>(telemetry.windowElapsedMs));
  setInt("rxBytesWindow", static_cast<int64_t>(telemetry.rxBytesWindow));
  setInt("txBytesWindow", static_cast<int64_t>(telemetry.txBytesWindow));
  setInt("framesWindow", static_cast<int64_t>(telemetry.framesWindow));
  setInt("rttMs", telemetry.rttMs);
  setInt("windowFrames", static_cast<int64_t>(telemetry.windowFrames));
  setInt("zgxParseUs", static_cast<int64_t>(telemetry.zgxParseUs));
  setInt("decodeUs", static_cast<int64_t>(telemetry.decodeUs));
  setInt("composeUs", static_cast<int64_t>(telemetry.composeUs));
  setInt("presentUs", static_cast<int64_t>(telemetry.presentUs));
  setInt("presentWaitUs", static_cast<int64_t>(telemetry.presentWaitUs));
  setInt("syncUs", static_cast<int64_t>(telemetry.syncUs));
  setInt("windowBytes", static_cast<int64_t>(telemetry.windowBytes));
  setInt("windowCommands", static_cast<int64_t>(telemetry.windowCommands));
  setInt("audioRateHz", telemetry.audioRateHz);
  setInt("audioLostBytes", static_cast<int64_t>(telemetry.audioLostBytes));
  setInt("audioTotalBytes", static_cast<int64_t>(telemetry.audioTotalBytes));
  setInt("gpuCopyUs", static_cast<int64_t>(telemetry.gpuCopyUs));
  setInt("gpuSrUs", static_cast<int64_t>(telemetry.gpuSrUs));
  setInt("gpuBlitUs", static_cast<int64_t>(telemetry.gpuBlitUs));
  return result;
}

napi_value BufferSize(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateInt(env, 0);
  }
  const std::shared_ptr<NativeBuffer> buffer = g_buffers.Find(handle);
  return CreateInt(env, buffer != nullptr ? static_cast<int64_t>(buffer->bytes.size()) : 0);
}

napi_value BufferRead(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  const std::shared_ptr<NativeBuffer> buffer = g_buffers.Find(handle);
  if (buffer == nullptr) {
    return CreateUndefined(env);
  }
  void* out = nullptr;
  napi_value result = nullptr;
  if (napi_create_arraybuffer(env, buffer->bytes.size(), &out, &result) != napi_ok ||
      out == nullptr) {
    return CreateUndefined(env);
  }
  if (!buffer->bytes.empty()) {
    memcpy(out, buffer->bytes.data(), buffer->bytes.size());
  }
  return result;
}

napi_value BufferRelease(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc >= 1 && napi_get_value_int64(env, args[0], &handle) == napi_ok) {
    g_buffers.Take(handle);
  }
  return CreateUndefined(env);
}

napi_value ScaleBgraBitmap(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value args[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int64_t srcW = 0;
  int64_t srcH = 0;
  int64_t dstW = 0;
  int64_t dstH = 0;
  double sharpen = 0.0;
  if (argc < 6 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int64(env, args[1], &srcW) != napi_ok ||
      napi_get_value_int64(env, args[2], &srcH) != napi_ok ||
      napi_get_value_int64(env, args[3], &dstW) != napi_ok ||
      napi_get_value_int64(env, args[4], &dstH) != napi_ok ||
      napi_get_value_double(env, args[5], &sharpen) != napi_ok) {
    return CreateInt(env, 0);
  }
  const std::shared_ptr<NativeBuffer> src = g_buffers.Take(handle);
  if (src == nullptr || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 ||
      src->bytes.size() != static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 4) {
    return CreateInt(env, 0);
  }
  auto dst = std::make_shared<NativeBuffer>();
  const uint32_t srcSide = static_cast<uint32_t>(srcW > srcH ? srcW : srcH);
  const uint32_t dstSide = static_cast<uint32_t>(dstW > dstH ? dstW : dstH);
  if (dstSide <= srcSide) {
    hmrdp::DownscaleBgra(src->bytes, static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
                         static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH), &dst->bytes);
  } else {
    hmrdp::UpscaleBgra(src->bytes, static_cast<uint32_t>(srcW), static_cast<uint32_t>(srcH),
                       static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
                       static_cast<float>(sharpen), &dst->bytes);
  }
  return CreateInt(env, g_buffers.Add(dst));
}

napi_value AudioRead(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 2 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateInt(env, 0);
  }
  void* data = nullptr;
  size_t length = 0;
  if (napi_get_arraybuffer_info(env, args[1], &data, &length) != napi_ok || data == nullptr) {
    return CreateInt(env, 0);
  }
  const std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    std::memset(data, 0, length);
    return CreateInt(env, 0);
  }
  return CreateInt(env, session->ReadAudio(static_cast<uint8_t*>(data), length));
}

napi_value AudioFormat(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int rate = 0;
  int channels = 0;
  if (argc >= 1 && napi_get_value_int64(env, args[0], &handle) == napi_ok) {
    const std::shared_ptr<Session> session = FindSession(handle);
    if (session != nullptr) {
      session->AudioFormat(&rate, &channels);
    }
  }
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value rateValue = nullptr;
  napi_create_int32(env, rate, &rateValue);
  napi_set_named_property(env, obj, "rate", rateValue);
  napi_value channelsValue = nullptr;
  napi_create_int32(env, channels, &channelsValue);
  napi_set_named_property(env, obj, "channels", channelsValue);
  return obj;
}

napi_value RequestRemoteFormat(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int64_t formatId = 0;
  int64_t kind = 0;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int64(env, args[1], &formatId) != napi_ok ||
      napi_get_value_int64(env, args[2], &kind) != napi_ok) {
    return CreateBool(env, false);
  }
  const std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env, session->RequestRemoteFormat(static_cast<uint32_t>(formatId),
                                                      static_cast<uint32_t>(kind)));
}

napi_value SendSynchronize(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int64_t flags = 0;
  if (argc < 2 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int64(env, args[1], &flags) != napi_ok) {
    return CreateBool(env, false);
  }
  const std::shared_ptr<Session> session = FindSession(handle);
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env, session->ReassertLockState(static_cast<uint32_t>(flags)));
}

napi_value SetRdpCursor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  bool enabled = true;
  if (argc < 1 || napi_get_value_bool(env, args[0], &enabled) != napi_ok) {
    return CreateBool(env, false);
  }
  Session::SetRdpCursor(enabled);
  return CreateBool(env, true);
}

napi_value SetHardwareAccel(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  bool enabled = true;
  if (argc < 1 || napi_get_value_bool(env, args[0], &enabled) != napi_ok) {
    return CreateBool(env, false);
  }
  Session::SetHardwareAccel(enabled);
  return CreateBool(env, true);
}

napi_value SetGfxCapture(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  bool enabled = false;
  if (argc < 1 || napi_get_value_bool(env, args[0], &enabled) != napi_ok) {
    return CreateBool(env, false);
  }
  std::string dir;
  if (argc >= 2) {
    size_t length = 0;
    if (napi_get_value_string_utf8(env, args[1], nullptr, 0, &length) == napi_ok && length > 0) {
      dir.resize(length);
      napi_get_value_string_utf8(env, args[1], dir.data(), length + 1, &length);
      dir.resize(length);
    }
  }
  Session::SetGfxCapture(enabled, dir);
  return CreateBool(env, true);
}

std::string GetStringArg(napi_env env, napi_value v) {
  size_t length = 0;
  if (napi_get_value_string_utf8(env, v, nullptr, 0, &length) != napi_ok || length == 0) {
    return std::string();
  }
  std::string s(length, '\0');
  napi_get_value_string_utf8(env, v, s.data(), length + 1, &length);
  s.resize(length);
  return s;
}

napi_value StartReplay(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value args[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = -1;
  if (argc >= 4) {
    const std::string surfaceId = GetStringArg(env, args[0]);
    int32_t surfaceW = 0;
    int32_t surfaceH = 0;
    napi_get_value_int32(env, args[1], &surfaceW);
    napi_get_value_int32(env, args[2], &surfaceH);
    const std::string gfxPath = GetStringArg(env, args[3]);
    int32_t realtime = 0;
    if (argc >= 5) {
      napi_get_value_int32(env, args[4], &realtime);
    }
    int32_t refMode = 0;
    if (argc >= 6) {
      napi_get_value_int32(env, args[5], &refMode);
    }
    hmrdp::GfxReplayRefMode replayRef = hmrdp::GfxReplayRefMode::kOff;
    if (refMode == 1) {
      replayRef = hmrdp::GfxReplayRefMode::kExport;
    } else if (refMode == 2) {
      replayRef = hmrdp::GfxReplayRefMode::kCompare;
    }
    const uint64_t sid = static_cast<uint64_t>(strtoull(surfaceId.c_str(), nullptr, 10));
    OHNativeWindow* window = nullptr;
    const int32_t err = OH_NativeWindow_CreateNativeWindowFromSurfaceId(sid, &window);
    if (err == 0 && window != nullptr) {
      PreferGpuWindowBuffer(window);
      auto replay = std::make_shared<hmrdp::GfxReplay>();
      if (replay->Start(window, surfaceW, surfaceH, gfxPath, realtime != 0, replayRef)) {
        handle = g_replays.Add(replay);
        HMRDP_LOGI("replay %{public}d: %{public}s", static_cast<int>(handle),
                   replay->Stats().c_str());
      } else {
        HMRDP_LOGW("replay: start failed (%{public}s)", replay->Stats().c_str());
      }
    } else {
      HMRDP_LOGE("CreateNativeWindowFromSurfaceId failed: %{public}d", err);
    }
  }
  return CreateInt(env, handle);
}

napi_value StopReplay(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  std::shared_ptr<hmrdp::GfxReplay> replay = g_replays.Take(handle);
  if (replay == nullptr) {
    return CreateUndefined(env);
  }
  replay->Stop();
  HMRDP_LOGI("replay %{public}d: %{public}s", static_cast<int>(handle),
             replay->Stats().c_str());
  return CreateUndefined(env);
}

napi_value StopAllReplays(napi_env env, napi_callback_info info) {
  (void)info;
  std::vector<std::shared_ptr<hmrdp::GfxReplay>> replays = g_replays.TakeAll();
  for (const auto& replay : replays) {
    replay->Stop();
  }
  HMRDP_LOGI("replay: stopped all (%{public}zu)", replays.size());
  return CreateUndefined(env);
}

napi_value ResizeReplay(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t width = 0;
  int32_t height = 0;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  napi_get_value_int32(env, args[1], &width);
  napi_get_value_int32(env, args[2], &height);
  std::shared_ptr<hmrdp::GfxReplay> replay = g_replays.Find(handle);
  if (replay != nullptr) {
    replay->Resize(width, height);
  }
  return CreateUndefined(env);
}

napi_value ReplayStats(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  std::shared_ptr<hmrdp::GfxReplay> replay = g_replays.Find(handle);
  const std::string lines =
      replay != nullptr ? replay->StatsLines() : std::string("no such replay");
  HMRDP_LOGI("replay %{public}d: %{public}s", static_cast<int>(handle),
             replay != nullptr ? replay->Stats().c_str() : "gone");
  napi_value result = nullptr;
  napi_create_string_utf8(env, lines.c_str(), lines.size(), &result);
  return result;
}

napi_value ListReplays(napi_env env, napi_callback_info info) {
  (void)info;
  std::vector<std::pair<int64_t, std::shared_ptr<hmrdp::GfxReplay>>> replays =
      g_replays.Snapshot();
  std::string out = "count=" + std::to_string(replays.size());
  for (const auto& entry : replays) {
    out += "\nhandle=" + std::to_string(entry.first) + " " + entry.second->Stats();
  }
  HMRDP_LOGI("replay list: %{public}s", out.c_str());
  napi_value result = nullptr;
  napi_create_string_utf8(env, out.c_str(), out.size(), &result);
  return result;
}

napi_value VulkanInfo(napi_env env, napi_callback_info) {
  const hmrdp::VulkanCapabilities& caps = hmrdp::GetVulkanCapabilities();
  HMRDP_LOGI("vulkan caps: %{public}s", caps.Describe().c_str());
  const std::string lines = caps.DescribeLines();
  napi_value result = nullptr;
  napi_create_string_utf8(env, lines.c_str(), lines.size(), &result);
  return result;
}

napi_value MakeVerdict(napi_env env, bool supported, const std::string& code) {
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value supportedValue = nullptr;
  napi_get_boolean(env, supported, &supportedValue);
  napi_set_named_property(env, obj, "supported", supportedValue);
  napi_value codeValue = nullptr;
  napi_create_string_utf8(env, code.c_str(), code.size(), &codeValue);
  napi_set_named_property(env, obj, "code", codeValue);
  return obj;
}

napi_value VulkanAccelSupport(napi_env env, napi_callback_info) {
  const hmrdp::VulkanCapabilities& caps = hmrdp::GetVulkanCapabilities();
  HMRDP_LOGI("vulkan accel support: supported=%{public}d code=%{public}s",
             caps.presenterSupported ? 1 : 0, caps.presenterUnsupportedCode.c_str());
  return MakeVerdict(env, caps.presenterSupported, caps.presenterUnsupportedCode);
}

napi_value SuperResolutionSupport(napi_env env, napi_callback_info) {
  const hmrdp::VulkanCapabilities& caps = hmrdp::GetVulkanCapabilities();
  HMRDP_LOGI("super resolution support: supported=%{public}d code=%{public}s",
             caps.srSupported ? 1 : 0, caps.srUnsupportedCode.c_str());
  return MakeVerdict(env, caps.srSupported, caps.srUnsupportedCode);
}

napi_value SuperResolutionBackends(napi_env env, napi_callback_info) {
  const hmrdp::VulkanCapabilities& caps = hmrdp::GetVulkanCapabilities();
  HMRDP_LOGI("super resolution backends: xengine=%{public}d fsr=%{public}d",
             caps.srXengineSupported ? 1 : 0, caps.srFsrSupported ? 1 : 0);
  napi_value obj = nullptr;
  napi_create_object(env, &obj);
  napi_value xengine = nullptr;
  napi_get_boolean(env, caps.srXengineSupported, &xengine);
  napi_set_named_property(env, obj, "xengine", xengine);
  napi_value fsr = nullptr;
  napi_get_boolean(env, caps.srFsrSupported, &fsr);
  napi_set_named_property(env, obj, "fsr", fsr);
  napi_value videoImport = nullptr;
  napi_get_boolean(env, caps.srVideoImportSupported, &videoImport);
  napi_set_named_property(env, obj, "videoImport", videoImport);
  return obj;
}

napi_value OnEvent(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return CreateUndefined(env);
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_function) {
    return CreateUndefined(env);
  }
  if (g_eventTsfn != nullptr) {
    napi_release_threadsafe_function(g_eventTsfn, napi_tsfn_release);
    g_eventTsfn = nullptr;
  }
  napi_value resourceName = nullptr;
  napi_create_string_utf8(env, "HmRdpEvent", NAPI_AUTO_LENGTH, &resourceName);
  napi_create_threadsafe_function(env, args[0], nullptr, resourceName, 0, 1, nullptr,
                                  nullptr, nullptr, CallJsEvent, &g_eventTsfn);
  return CreateUndefined(env);
}

}  // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor desc[] = {
      {"createSession", nullptr, CreateSession, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"destroySession", nullptr, DestroySession, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"connect", nullptr, Connect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"disconnect", nullptr, Disconnect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setSurface", nullptr, SetSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"updateSurface", nullptr, UpdateSurface, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"clearSurface", nullptr, ClearSurface, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"sendMouse", nullptr, SendMouse, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setSuppressed", nullptr, SetSuppressed, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"sendTouch", nullptr, SendTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sendKey", nullptr, SendKey, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sendUnicode", nullptr, SendUnicode, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"sendLockState", nullptr, SendLockState, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"sendSynchronize", nullptr, SendSynchronize, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"requestRemoteFormat", nullptr, RequestRemoteFormat, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setShortcutPassthrough", nullptr, SetShortcutPassthrough, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setClipboardText", nullptr, SetClipboardText, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setClipboardHtmlRaw", nullptr, SetClipboardHtmlRaw, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setClipboardImage", nullptr, SetClipboardImage, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setClipboardFileOffer", nullptr, SetClipboardFileOffer, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"requestRemoteFileContents", nullptr, RequestRemoteFileContents, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"cancelFileTransfer", nullptr, CancelFileTransfer, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"onEvent", nullptr, OnEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sessionTelemetry", nullptr, GetSessionTelemetry, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"bufferSize", nullptr, BufferSize, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bufferRead", nullptr, BufferRead, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bufferRelease", nullptr, BufferRelease, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"scaleBgraBitmap", nullptr, ScaleBgraBitmap, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"audioRead", nullptr, AudioRead, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"audioFormat", nullptr, AudioFormat, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setRdpCursor", nullptr, SetRdpCursor, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setHardwareAccel", nullptr, SetHardwareAccel, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setGfxCapture", nullptr, SetGfxCapture, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"startReplay", nullptr, StartReplay, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopReplay", nullptr, StopReplay, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopAllReplays", nullptr, StopAllReplays, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"resizeReplay", nullptr, ResizeReplay, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"replayStats", nullptr, ReplayStats, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"listReplays", nullptr, ListReplays, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"vulkanInfo", nullptr, VulkanInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"vulkanAccelSupport", nullptr, VulkanAccelSupport, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"superResolutionSupport", nullptr, SuperResolutionSupport, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"superResolutionBackends", nullptr, SuperResolutionBackends, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightProbe", nullptr, MoonlightProbe, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"moonlightVideoFormats", nullptr, MoonlightVideoFormats, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightVideoCodec", nullptr, MoonlightVideoCodec, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightVideoCodecSupport", nullptr, MoonlightVideoCodecSupport, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightVideoMaxFps", nullptr, MoonlightVideoMaxFps, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightVideoWarning", nullptr, MoonlightVideoWarning, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightSetSessionListener", nullptr, MoonlightSetSessionListener, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightSessionSetSurface", nullptr, MoonlightSessionSetSurface, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightSessionClearSurface", nullptr, MoonlightSessionClearSurface, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightSessionResizeSurface", nullptr, MoonlightSessionResizeSurface, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightSessionStart", nullptr, MoonlightSessionStart, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightSessionStop", nullptr, MoonlightSessionStop, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightSessionActive", nullptr, MoonlightSessionActive, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightAudioRead", nullptr, MoonlightAudioRead, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightAudioFormat", nullptr, MoonlightAudioFormat, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightLaunchQueryParameters", nullptr, MoonlightLaunchQueryParameters, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightMouseMove", nullptr, MoonlightSessionMouseMove, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightMousePosition", nullptr, MoonlightSessionMousePosition, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightMouseButton", nullptr, MoonlightSessionMouseButton, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightKey", nullptr, MoonlightSessionKey, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"moonlightScroll", nullptr, MoonlightSessionScroll, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightTouch", nullptr, MoonlightSessionTouch, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightTouchpad", nullptr, MoonlightSessionTouchpadFrame, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightHighResScroll", nullptr, MoonlightSessionHighResScroll, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightHighResHScroll", nullptr, MoonlightSessionHighResHScroll, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightController", nullptr, MoonlightSessionController, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightControllerArrival", nullptr, MoonlightSessionControllerArrival, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightText", nullptr, MoonlightSessionText, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightSendClipboardData", nullptr, MoonlightSendClipboard, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightHostFeatures", nullptr, MoonlightSessionHostFeatures, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightTelemetry", nullptr, MoonlightGetTelemetry, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightSetShortcutPassthrough", nullptr, MoonlightSetShortcutPassthrough, nullptr,
       nullptr, nullptr, napi_default, nullptr},
      {"gamepadKitSetListener", nullptr, GamepadKitSetListener, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"gamepadKitStop", nullptr, GamepadKitStopListener, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"gamepadKitDevices", nullptr, GamepadKitDevices, nullptr, nullptr, nullptr, napi_default,
       nullptr},
  };

  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
  return exports;
}
EXTERN_C_END

namespace hmrdp {
namespace binding {

int64_t AddBufferHandle(const std::vector<uint8_t>& bytes) {
  auto buffer = std::make_shared<NativeBuffer>();
  buffer->bytes = bytes;
  return g_buffers.Add(buffer);
}

}  // namespace binding
}  // namespace hmrdp

static napi_module g_hmrdpModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "hmrdp",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterHmRdpModule() {
  napi_module_register(&g_hmrdpModule);
}
