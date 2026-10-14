#include <multimodalinput/oh_input_manager.h>
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
#include "hmrdp_moonlight.h"
#include "hmrdp_moonlight_audio.h"
#include "hmrdp_moonlight_session.h"
#include "hmrdp_replay.h"
#include "hmrdp_session.h"
#include "hmrdp_vk_context.h"

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

// 快捷键穿透: one process-global key interceptor, delivered only while this app is
// focused. Ownership follows the focused session; the token-keyed registry keeps it generic.
using InterceptSink = std::function<void(int32_t keyCode, bool down)>;
std::mutex g_interceptMutex;
std::unordered_map<int64_t, InterceptSink> g_interceptSinks;
int64_t g_interceptOwner = 0;
bool g_interceptActive = false;

void OnInterceptedKeyEvent(const Input_KeyEvent* keyEvent) {
  if (keyEvent == nullptr) {
    return;
  }
  const int32_t action = OH_Input_GetKeyEventAction(keyEvent);
  if (action != KEY_ACTION_DOWN && action != KEY_ACTION_UP) {
    return;
  }
  const int32_t keyCode = OH_Input_GetKeyEventKeyCode(keyEvent);
  InterceptSink sink;
  {
    std::lock_guard<std::mutex> lock(g_interceptMutex);
    const auto it = g_interceptSinks.find(g_interceptOwner);
    if (it != g_interceptSinks.end()) {
      sink = it->second;
    }
  }
  if (sink) {
    sink(keyCode, action == KEY_ACTION_DOWN);
  }
}

void RegisterInterceptSink(int64_t token, InterceptSink sink) {
  std::lock_guard<std::mutex> lock(g_interceptMutex);
  g_interceptSinks[token] = std::move(sink);
}

// Releasing a sink that owns the interceptor must also release the OS registration.
void UnregisterInterceptSink(int64_t token) {
  bool removeInterceptor = false;
  {
    std::lock_guard<std::mutex> lock(g_interceptMutex);
    g_interceptSinks.erase(token);
    if (g_interceptOwner == token) {
      g_interceptOwner = 0;
      removeInterceptor = g_interceptActive;
    }
  }
  if (removeInterceptor) {
    OH_Input_RemoveKeyEventInterceptor();
    std::lock_guard<std::mutex> lock(g_interceptMutex);
    g_interceptActive = false;
  }
}

// Only the focused session becomes the owner; releasing another token's ownership
// is a no-op, so a focus handoff is order-independent.
bool SetInterceptOwner(int64_t token, bool enabled) {
  bool callAdd = false;
  bool callRemove = false;
  {
    std::lock_guard<std::mutex> lock(g_interceptMutex);
    if (enabled && token != 0) {
      g_interceptOwner = token;
      callAdd = !g_interceptActive;
    } else if (g_interceptOwner == token) {
      g_interceptOwner = 0;
      callRemove = g_interceptActive;
    }
  }

  if (callAdd) {
    const Input_Result ret = OH_Input_AddKeyEventInterceptor(OnInterceptedKeyEvent, nullptr);
    const bool ok = ret == INPUT_SUCCESS || ret == INPUT_REPEAT_INTERCEPTOR;
    std::lock_guard<std::mutex> lock(g_interceptMutex);
    g_interceptActive = ok;
    if (!ok) {
      g_interceptOwner = 0;
      HMRDP_LOGW("key interceptor add failed: %{public}d", static_cast<int32_t>(ret));
    }
    return ok;
  }
  if (callRemove) {
    OH_Input_RemoveKeyEventInterceptor();
    std::lock_guard<std::mutex> lock(g_interceptMutex);
    g_interceptActive = false;
    return true;
  }
  std::lock_guard<std::mutex> lock(g_interceptMutex);
  return g_interceptActive;
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

std::string GetStringProperty(napi_env env, napi_value object, const char* name) {
  napi_value value = nullptr;
  if (napi_get_named_property(env, object, name, &value) != napi_ok || value == nullptr) {
    return std::string();
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_string) {
    return std::string();
  }
  size_t length = 0;
  napi_get_value_string_utf8(env, value, nullptr, 0, &length);
  std::string result(length, '\0');
  napi_get_value_string_utf8(env, value, result.data(), length + 1, &length);
  result.resize(length);
  return result;
}

std::string GetStringArgument(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  if (value == nullptr || napi_typeof(env, value, &type) != napi_ok || type != napi_string) {
    return std::string();
  }
  size_t length = 0;
  napi_get_value_string_utf8(env, value, nullptr, 0, &length);
  std::string result(length, '\0');
  napi_get_value_string_utf8(env, value, result.data(), length + 1, &length);
  result.resize(length);
  return result;
}

int32_t GetIntProperty(napi_env env, napi_value object, const char* name, int32_t fallback) {
  napi_value value = nullptr;
  if (napi_get_named_property(env, object, name, &value) != napi_ok || value == nullptr) {
    return fallback;
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_number) {
    return fallback;
  }
  int32_t result = fallback;
  napi_get_value_int32(env, value, &result);
  return result;
}

bool GetBoolProperty(napi_env env, napi_value object, const char* name, bool fallback) {
  napi_value value = nullptr;
  if (napi_get_named_property(env, object, name, &value) != napi_ok || value == nullptr) {
    return fallback;
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_boolean) {
    return fallback;
  }
  bool result = fallback;
  napi_get_value_bool(env, value, &result);
  return result;
}

napi_value CreateInt(napi_env env, int64_t value) {
  napi_value result = nullptr;
  napi_create_int64(env, value, &result);
  return result;
}

napi_value CreateBool(napi_env env, bool value) {
  napi_value result = nullptr;
  napi_get_boolean(env, value, &result);
  return result;
}

napi_value CreateStringUtf8(napi_env env, const std::string& value) {
  napi_value result = nullptr;
  napi_create_string_utf8(env, value.c_str(), value.size(), &result);
  return result;
}

napi_value CreateUndefined(napi_env env) {
  napi_value result = nullptr;
  napi_get_undefined(env, &result);
  return result;
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
  options.scalePercent = GetIntProperty(env, args[1], "scalePercent", 0);
  options.ignoreCertificate = GetBoolProperty(env, args[1], "ignoreCertificate", true);
  options.enableAudio = GetBoolProperty(env, args[1], "enableAudio", true);
  options.srEnabled = GetBoolProperty(env, args[1], "srEnabled", false);
  options.srBackend = GetIntProperty(env, args[1], "srBackend", 0) == 1
                          ? hmrdp::SuperResolutionBackend::kFsr
                          : hmrdp::SuperResolutionBackend::kXengine;
  options.srRatioPercent = GetIntProperty(env, args[1], "srRatioPercent", 0);
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
  return obj;
}

napi_value MoonlightProbe(napi_env env, napi_callback_info) {
  const std::string out = hmrdp::MoonlightProbe();
  napi_value result = nullptr;
  napi_create_string_utf8(env, out.c_str(), out.size(), &result);
  return result;
}

napi_value MoonlightVideoFormats(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t choice = 0;
  if (argc >= 1) {
    napi_get_value_int32(env, args[0], &choice);
  }
  int32_t codecModes = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, args[1], &codecModes);
  }
  const hmrdp::MoonlightVideoCodecChoice value =
      static_cast<hmrdp::MoonlightVideoCodecChoice>(choice);
  return CreateInt(env, hmrdp::MoonlightVideoFormats(value, codecModes));
}

napi_value MoonlightVideoCodecSupport(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t codec = 0;
  if (argc >= 1) {
    napi_get_value_int32(env, args[0], &codec);
  }
  return CreateInt(env, hmrdp::MoonlightVideoCodecSupport(codec));
}

napi_value MoonlightVideoCodec(napi_env env, napi_callback_info) {
  const std::string out = hmrdp::MoonlightVideoCodec();
  napi_value result = nullptr;
  napi_create_string_utf8(env, out.c_str(), out.size(), &result);
  return result;
}

napi_value MoonlightVideoWarning(napi_env env, napi_callback_info) {
  const std::string out = hmrdp::MoonlightVideoWarning();
  napi_value result = nullptr;
  napi_create_string_utf8(env, out.c_str(), out.size(), &result);
  return result;
}

napi_threadsafe_function g_moonlightTsfn = nullptr;
OHNativeWindow* g_moonlightWindow = nullptr;

// Negative, so it never collides with the positive RDP session handles used as tokens.
constexpr int64_t kMoonlightInterceptToken = -1;

struct MoonlightEventPayload {
  int event = 0;
  int value = 0;
  std::string text;
  int width = 0;
  int height = 0;
  int hotX = 0;
  int hotY = 0;
  int64_t shapeId = 0;
  bool visible = false;
  bool hasShape = false;
  // Bytes carried as a clipboard frame handle instead of an image handle.
  bool isClipboard = false;
  std::vector<uint8_t> bytes;
};

void CallJsMoonlightEvent(napi_env env, napi_value jsCallback, void*, void* data) {
  std::unique_ptr<MoonlightEventPayload> payload(static_cast<MoonlightEventPayload*>(data));
  if (env == nullptr || jsCallback == nullptr || payload == nullptr) {
    return;
  }
  napi_value eventValue = nullptr;
  napi_value valueValue = nullptr;
  napi_value textValue = nullptr;
  napi_create_int32(env, payload->event, &eventValue);
  napi_create_int32(env, payload->value, &valueValue);
  napi_create_string_utf8(env, payload->text.c_str(), payload->text.size(), &textValue);
  napi_value extra = nullptr;
  napi_create_object(env, &extra);
  auto setInt = [env, extra](const char* name, int64_t v) {
    napi_value value = nullptr;
    napi_create_int64(env, v, &value);
    napi_set_named_property(env, extra, name, value);
  };
  auto setBool = [env, extra](const char* name, bool v) {
    napi_value value = nullptr;
    napi_get_boolean(env, v, &value);
    napi_set_named_property(env, extra, name, value);
  };
  setInt("width", payload->width);
  setInt("height", payload->height);
  setInt("hotX", payload->hotX);
  setInt("hotY", payload->hotY);
  setInt("shapeId", payload->shapeId);
  setBool("visible", payload->visible);
  setBool("hasShape", payload->hasShape);
  if (!payload->bytes.empty()) {
    auto buffer = std::make_shared<NativeBuffer>();
    buffer->bytes = payload->bytes;
    setInt(payload->isClipboard ? "dataHandle" : "imageHandle", g_buffers.Add(buffer));
  }
  napi_value argv[4] = {eventValue, valueValue, textValue, extra};
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_call_function(env, global, jsCallback, 4, argv, nullptr);
}

void OnMoonlightCursor(const hmrdp::MoonlightCursorShape& shape) {
  if (g_moonlightTsfn == nullptr) {
    return;
  }
  MoonlightEventPayload* payload = new MoonlightEventPayload();
  payload->event = static_cast<int>(hmrdp::MoonlightSessionEvent::kCursorShape);
  payload->visible = shape.visible;
  payload->hasShape = shape.hasShape;
  payload->shapeId = shape.shapeId;
  payload->width = shape.width;
  payload->height = shape.height;
  payload->hotX = shape.hotspotX;
  payload->hotY = shape.hotspotY;
  if (shape.hasShape && shape.bgra != nullptr && shape.size > 0) {
    payload->bytes.assign(shape.bgra, shape.bgra + shape.size);
  }
  const napi_status status =
      napi_call_threadsafe_function(g_moonlightTsfn, payload, napi_tsfn_nonblocking);
  if (status != napi_ok) {
    delete payload;
  }
}

void OnMoonlightClipboard(const uint8_t* data, int length) {
  if (g_moonlightTsfn == nullptr) {
    return;
  }
  MoonlightEventPayload* payload = new MoonlightEventPayload();
  payload->event = static_cast<int>(hmrdp::MoonlightSessionEvent::kClipboardData);
  payload->isClipboard = true;
  if (data != nullptr && length > 0) {
    payload->bytes.assign(data, data + length);
  }
  const napi_status status =
      napi_call_threadsafe_function(g_moonlightTsfn, payload, napi_tsfn_nonblocking);
  if (status != napi_ok) {
    delete payload;
  }
}

void OnMoonlightSessionEvent(hmrdp::MoonlightSessionEvent event, int value, const char* text) {
  if (g_moonlightTsfn == nullptr) {
    return;
  }
  MoonlightEventPayload* payload = new MoonlightEventPayload();
  payload->event = static_cast<int>(event);
  payload->value = value;
  payload->text = text != nullptr ? text : "";
  const napi_status status =
      napi_call_threadsafe_function(g_moonlightTsfn, payload, napi_tsfn_nonblocking);
  if (status != napi_ok) {
    delete payload;
  }
}

bool HexToBytes(const std::string& hex, std::string* out) {
  if (hex.size() % 2 != 0) {
    return false;
  }
  out->clear();
  out->reserve(hex.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out->push_back(static_cast<char>((hi << 4) | lo));
  }
  return true;
}

napi_value MoonlightSetSessionListener(napi_env env, napi_callback_info info) {
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
  if (g_moonlightTsfn != nullptr) {
    UnregisterInterceptSink(kMoonlightInterceptToken);
    napi_release_threadsafe_function(g_moonlightTsfn, napi_tsfn_release);
    g_moonlightTsfn = nullptr;
  }
  napi_value resourceName = nullptr;
  napi_create_string_utf8(env, "HmRdpMoonlightEvent", NAPI_AUTO_LENGTH, &resourceName);
  napi_create_threadsafe_function(env, args[0], nullptr, resourceName, 0, 1, nullptr, nullptr,
                                  nullptr, CallJsMoonlightEvent, &g_moonlightTsfn);
  hmrdp::MoonlightSessionSetCursorCallback(&OnMoonlightCursor);
  hmrdp::MoonlightSessionSetClipboardCallback(&OnMoonlightClipboard);
  // Delivered only while Moonlight is the focused interceptor owner.
  RegisterInterceptSink(kMoonlightInterceptToken, [](int32_t keyCode, bool down) {
    OnMoonlightSessionEvent(down ? hmrdp::MoonlightSessionEvent::kKeyInterceptDown
                                 : hmrdp::MoonlightSessionEvent::kKeyInterceptUp,
                            keyCode, "");
  });
  return CreateUndefined(env);
}

napi_value MoonlightSendClipboard(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return CreateInt(env, -1);
  }
  void* data = nullptr;
  size_t length = 0;
  if (napi_get_arraybuffer_info(env, args[0], &data, &length) != napi_ok) {
    // Also accept a TypedArray view (e.g. Uint8Array) over an ArrayBuffer.
    bool isTypedArray = false;
    napi_is_typedarray(env, args[0], &isTypedArray);
    if (!isTypedArray) {
      return CreateInt(env, -1);
    }
    napi_typedarray_type type;
    size_t offset = 0;
    size_t count = 0;
    napi_value buffer = nullptr;
    if (napi_get_typedarray_info(env, args[0], &type, &count, &data, &buffer, &offset) != napi_ok ||
        data == nullptr) {
      return CreateInt(env, -1);
    }
    length = count;
  }
  if (data == nullptr || length == 0) {
    return CreateInt(env, -1);
  }
  return CreateInt(env, hmrdp::MoonlightSessionSendClipboard(data, static_cast<int>(length)));
}

// 快捷键穿透 for a Moonlight session; returns whether the requested state is in effect.
napi_value MoonlightSetShortcutPassthrough(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  bool enabled = false;
  if (argc < 1 || napi_get_value_bool(env, args[0], &enabled) != napi_ok) {
    return CreateBool(env, false);
  }
  return CreateBool(env, SetInterceptOwner(kMoonlightInterceptToken, enabled));
}

napi_value MoonlightSessionSetSurface(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return CreateBool(env, false);
  }
  const std::string surfaceId = GetStringArgument(env, args[0]);
  const uint64_t sid = static_cast<uint64_t>(strtoull(surfaceId.c_str(), nullptr, 10));
  OHNativeWindow* window = nullptr;
  if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(sid, &window) != 0 || window == nullptr) {
    HMRDP_LOGE("moonlight: CreateNativeWindowFromSurfaceId failed");
    return CreateBool(env, false);
  }
  // Default window scaling stretches the picture; SCALE_FIT keeps the video aspect.
  if (OH_NativeWindow_NativeWindowSetScalingModeV2(window, OH_SCALING_MODE_SCALE_FIT_V2) != 0) {
    HMRDP_LOGW("moonlight: set scaling mode failed");
  }
  if (g_moonlightWindow != nullptr) {
    if (hmrdp::MoonlightSessionActive()) {
      OH_NativeWindow_DestroyNativeWindow(window);
      return CreateBool(env, false);
    }
    OH_NativeWindow_DestroyNativeWindow(g_moonlightWindow);
    g_moonlightWindow = nullptr;
  }
  g_moonlightWindow = window;
  hmrdp::MoonlightSessionSetWindow(window);
  return CreateBool(env, true);
}

napi_value MoonlightSessionClearSurface(napi_env env, napi_callback_info) {
  hmrdp::MoonlightSessionSetWindow(nullptr);
  if (g_moonlightWindow != nullptr && !hmrdp::MoonlightSessionActive()) {
    OH_NativeWindow_DestroyNativeWindow(g_moonlightWindow);
    g_moonlightWindow = nullptr;
  }
  return CreateUndefined(env);
}

napi_value MoonlightSessionStart(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return CreateStringUtf8(env, "config");
  }
  hmrdp::MoonlightStreamConfig config;
  config.host = GetStringProperty(env, args[0], "host");
  config.serverInfoAppVersion = GetStringProperty(env, args[0], "serverInfoAppVersion");
  config.serverInfoGfeVersion = GetStringProperty(env, args[0], "serverInfoGfeVersion");
  config.sessionUrl = GetStringProperty(env, args[0], "sessionUrl");
  config.serverCodecModeSupport = GetIntProperty(env, args[0], "serverCodecModeSupport", 0);
  config.width = GetIntProperty(env, args[0], "width", 1920);
  config.height = GetIntProperty(env, args[0], "height", 1080);
  config.fps = GetIntProperty(env, args[0], "fps", 60);
  config.bitrateKbps = GetIntProperty(env, args[0], "bitrateKbps", 20000);
  config.packetSize = GetIntProperty(env, args[0], "packetSize", 1024);
    config.streamingRemotely = GetIntProperty(env, args[0], "streamingRemotely", 2);
  config.audioConfiguration = GetIntProperty(env, args[0], "audioConfiguration", 0);
  config.supportedVideoFormats = GetIntProperty(env, args[0], "supportedVideoFormats", 0);
  config.clientRefreshRateX100 = GetIntProperty(env, args[0], "clientRefreshRateX100", 0);
  config.colorSpace = GetIntProperty(env, args[0], "colorSpace", 0);
  config.colorRange = GetIntProperty(env, args[0], "colorRange", 0);
  config.encryptionFlags = GetIntProperty(env, args[0], "encryptionFlags", 0);
  config.localCursorMode = GetBoolProperty(env, args[0], "localCursorMode", false);
  config.enableAudio = GetBoolProperty(env, args[0], "enableAudio", true);
  config.lowLatencyAudio = GetBoolProperty(env, args[0], "lowLatencyAudio", true);
  HexToBytes(GetStringProperty(env, args[0], "remoteInputAesKeyHex"), &config.remoteInputAesKey);
  HexToBytes(GetStringProperty(env, args[0], "remoteInputAesIvHex"), &config.remoteInputAesIv);
  return CreateStringUtf8(env, hmrdp::MoonlightSessionStart(config, &OnMoonlightSessionEvent));
}

napi_value MoonlightSessionStop(napi_env env, napi_callback_info) {
  hmrdp::MoonlightSessionStop();
  return CreateUndefined(env);
}

napi_value MoonlightSessionActive(napi_env env, napi_callback_info) {
  return CreateBool(env, hmrdp::MoonlightSessionActive());
}

napi_value MoonlightAudioRead(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  void* data = nullptr;
  size_t length = 0;
  if (argc < 1 || napi_get_arraybuffer_info(env, args[0], &data, &length) != napi_ok ||
      data == nullptr) {
    return CreateInt(env, 0);
  }
  return CreateInt(env, hmrdp::MoonlightAudioRead(static_cast<uint8_t*>(data), length));
}

napi_value MoonlightAudioFormat(napi_env env, napi_callback_info info) {
  (void)info;
  int rate = 0;
  int channels = 0;
  hmrdp::MoonlightAudioFormat(&rate, &channels);
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

napi_value MoonlightLaunchQueryParameters(napi_env env, napi_callback_info) {
  return CreateStringUtf8(env, hmrdp::MoonlightLaunchQueryParameters());
}

napi_value MoonlightSessionMouseMove(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t dx = 0;
  int32_t dy = 0;
  napi_get_value_int32(env, args[0], &dx);
  napi_get_value_int32(env, args[1], &dy);
  hmrdp::MoonlightSessionMouseMove(dx, dy);
  return CreateUndefined(env);
}

napi_value MoonlightSessionMousePosition(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value args[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t x = 0;
  int32_t y = 0;
  int32_t refW = 0;
  int32_t refH = 0;
  napi_get_value_int32(env, args[0], &x);
  napi_get_value_int32(env, args[1], &y);
  napi_get_value_int32(env, args[2], &refW);
  napi_get_value_int32(env, args[3], &refH);
  hmrdp::MoonlightSessionMousePosition(x, y, refW, refH);
  return CreateUndefined(env);
}

napi_value MoonlightSessionMouseButton(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t action = 0;
  int32_t button = 0;
  napi_get_value_int32(env, args[0], &action);
  napi_get_value_int32(env, args[1], &button);
  hmrdp::MoonlightSessionMouseButton(action, button);
  return CreateUndefined(env);
}

napi_value MoonlightSessionKey(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t keyCode = 0;
  int32_t action = 0;
  int32_t modifiers = 0;
  napi_get_value_int32(env, args[0], &keyCode);
  napi_get_value_int32(env, args[1], &action);
  napi_get_value_int32(env, args[2], &modifiers);
  hmrdp::MoonlightSessionKey(keyCode, action, modifiers);
  return CreateUndefined(env);
}

napi_value MoonlightSessionScroll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t clicks = 0;
  napi_get_value_int32(env, args[0], &clicks);
  hmrdp::MoonlightSessionScroll(clicks);
  return CreateUndefined(env);
}

napi_value MoonlightSessionTouch(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value args[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t eventType = 0;
  int32_t pointerId = 0;
  double x = 0;
  double y = 0;
  double pressure = 0;
  napi_get_value_int32(env, args[0], &eventType);
  napi_get_value_int32(env, args[1], &pointerId);
  napi_get_value_double(env, args[2], &x);
  napi_get_value_double(env, args[3], &y);
  napi_get_value_double(env, args[4], &pressure);
  hmrdp::MoonlightSessionTouch(eventType, pointerId, static_cast<float>(x),
                               static_cast<float>(y), static_cast<float>(pressure));
  return CreateUndefined(env);
}

// eventTypes / pointerIds / x / y / pressure are parallel arrays; width/height are
// millimetres and buttonState a LI_TOUCHPAD_BUTTON_* mask.
napi_value MoonlightSessionTouchpadFrame(napi_env env, napi_callback_info info) {
  constexpr uint32_t kMaxContacts = 5;
  size_t argc = 8;
  napi_value args[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 8) {
    return CreateUndefined(env);
  }
  int32_t widthMm = 0;
  int32_t heightMm = 0;
  int32_t buttonState = 0;
  napi_get_value_int32(env, args[5], &widthMm);
  napi_get_value_int32(env, args[6], &heightMm);
  napi_get_value_int32(env, args[7], &buttonState);

  uint32_t count = 0;
  if (napi_get_array_length(env, args[0], &count) != napi_ok) {
    return CreateUndefined(env);
  }
  if (count > kMaxContacts) {
    count = kMaxContacts;
  }
  uint8_t eventTypes[kMaxContacts] = {0};
  uint32_t pointerIds[kMaxContacts] = {0};
  float x[kMaxContacts] = {0};
  float y[kMaxContacts] = {0};
  float pressure[kMaxContacts] = {0};
  for (uint32_t i = 0; i < count; i++) {
    napi_value element = nullptr;
    int32_t type = 0;
    uint32_t id = 0;
    double px = 0;
    double py = 0;
    double p = 0;
    napi_get_element(env, args[0], i, &element);
    napi_get_value_int32(env, element, &type);
    napi_get_element(env, args[1], i, &element);
    napi_get_value_uint32(env, element, &id);
    napi_get_element(env, args[2], i, &element);
    napi_get_value_double(env, element, &px);
    napi_get_element(env, args[3], i, &element);
    napi_get_value_double(env, element, &py);
    napi_get_element(env, args[4], i, &element);
    napi_get_value_double(env, element, &p);
    eventTypes[i] = static_cast<uint8_t>(type);
    pointerIds[i] = id;
    x[i] = static_cast<float>(px);
    y[i] = static_cast<float>(py);
    pressure[i] = static_cast<float>(p);
  }
  hmrdp::MoonlightSessionTouchpadFrame(static_cast<int>(count), eventTypes, pointerIds, x, y,
                                       pressure, widthMm, heightMm, buttonState);
  return CreateUndefined(env);
}

napi_value MoonlightSessionHighResScroll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t amount = 0;
  napi_get_value_int32(env, args[0], &amount);
  hmrdp::MoonlightSessionHighResScroll(amount);
  return CreateUndefined(env);
}

napi_value MoonlightSessionHighResHScroll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t amount = 0;
  napi_get_value_int32(env, args[0], &amount);
  hmrdp::MoonlightSessionHighResHScroll(amount);
  return CreateUndefined(env);
}

napi_value MoonlightSessionText(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  size_t length = 0;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &length) != napi_ok) {
    return CreateUndefined(env);
  }
  std::string text;
  text.resize(length + 1);
  napi_get_value_string_utf8(env, args[0], text.data(), length + 1, &length);
  hmrdp::MoonlightSessionText(text.c_str(), static_cast<int>(length));
  return CreateUndefined(env);
}

napi_value MoonlightSessionHostFeatures(napi_env env, napi_callback_info) {
  return CreateInt(env, hmrdp::MoonlightSessionHostFeatureFlags());
}

napi_value MoonlightGetTelemetry(napi_env env, napi_callback_info) {
  const hmrdp::MoonlightTelemetry telemetry = hmrdp::MoonlightReadTelemetry();
  napi_value result = nullptr;
  napi_create_object(env, &result);
  auto setInt = [env, result](const char* name, int64_t value) {
    napi_value v = nullptr;
    napi_create_int64(env, value, &v);
    napi_set_named_property(env, result, name, v);
  };
  setInt("frames", static_cast<int64_t>(telemetry.frames));
  setInt("bytes", static_cast<int64_t>(telemetry.bytes));
  setInt("hostLatencyTenthMs", static_cast<int64_t>(telemetry.hostLatencyTenthMs));
  setInt("hostLatencyFrames", static_cast<int64_t>(telemetry.hostLatencyFrames));
  setInt("decodeRoundTripUs", static_cast<int64_t>(telemetry.decodeRoundTripUs));
  setInt("decodeRoundTripFrames", static_cast<int64_t>(telemetry.decodeRoundTripFrames));
  setInt("pendingVideoFrames", telemetry.pendingVideoFrames);
  setInt("rttMs", telemetry.rttMs);
  setInt("rttVarianceMs", telemetry.rttVarianceMs);
  setInt("packetVideo", telemetry.packetVideo);
  setInt("packetFec", telemetry.packetFec);
  setInt("packetFecRecovered", telemetry.packetFecRecovered);
  setInt("packetFecFailed", telemetry.packetFecFailed);
  setInt("packetOos", telemetry.packetOos);
  setInt("packetInvalid", telemetry.packetInvalid);
  return result;
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
      {"moonlightVideoWarning", nullptr, MoonlightVideoWarning, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"moonlightSetSessionListener", nullptr, MoonlightSetSessionListener, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightSessionSetSurface", nullptr, MoonlightSessionSetSurface, nullptr, nullptr,
       nullptr, napi_default, nullptr},
      {"moonlightSessionClearSurface", nullptr, MoonlightSessionClearSurface, nullptr, nullptr,
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
  };

  napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
  return exports;
}
EXTERN_C_END

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
