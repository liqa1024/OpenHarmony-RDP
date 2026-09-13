/*
 * HmRdp - HarmonyOS RDP client
 * Node-API bridge between ArkTS and the FreeRDP session wrapper.
 *
 * Rendering surfaces are bound through the ArkUI XComponent surface id:
 * ArkTS forwards onSurfaceCreated/Changed/Destroyed to this module, which
 * turns the surface id into an OHNativeWindow consumed by the EGL renderer.
 */
#include <native_window/external_window.h>
#include <napi/native_api.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "hmrdp_log.h"
#include "hmrdp_replay.h"
#include "hmrdp_rfx.h"
#include "hmrdp_session.h"
#include "hmrdp_vk_context.h"
#include "hmrdp_vk_renderer.h"
#include "hmrdp_vk_selftest.h"

namespace {

using hmrdp::RdpOptions;
using hmrdp::AudioOutput;
using hmrdp::Session;
using hmrdp::SessionEvent;

std::mutex g_mutex;
std::map<int64_t, std::unique_ptr<Session>> g_sessions;
// Each session owns exactly one OHNativeWindow, keyed by session handle, so
// multiple concurrent sessions never share surface state.
std::map<int64_t, OHNativeWindow*> g_windows;
int64_t g_nextId = 1;

// Dev-only Vulkan bring-up harness (VULKAN-TODO §5 V0). Kept separate from the
// session so the platform path can be proven before the engine work lands.
std::mutex g_vkMutex;
std::unique_ptr<hmrdp::VkRenderer> g_vkRenderer;
OHNativeWindow* g_vkWindow = nullptr;

napi_threadsafe_function g_eventTsfn = nullptr;

struct EventPayload {
  int64_t handle;
  int event;
  std::string data;
};

void CallJsEvent(napi_env env, napi_value jsCallback, void*, void* data) {
  std::unique_ptr<EventPayload> payload(static_cast<EventPayload*>(data));
  if (env == nullptr || jsCallback == nullptr || payload == nullptr) {
    return;
  }
  napi_value handleValue = nullptr;
  napi_value eventValue = nullptr;
  napi_value dataValue = nullptr;
  napi_create_int64(env, payload->handle, &handleValue);
  napi_create_int32(env, payload->event, &eventValue);
  napi_create_string_utf8(env, payload->data.c_str(), NAPI_AUTO_LENGTH, &dataValue);
  napi_value argv[3] = {handleValue, eventValue, dataValue};
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_call_function(env, global, jsCallback, 3, argv, nullptr);
}

void OnSessionEvent(int64_t handle, SessionEvent event, const std::string& data) {
  if (g_eventTsfn == nullptr) {
    return;
  }
  EventPayload* payload = new EventPayload{handle, static_cast<int>(event), data};
  const napi_status status =
      napi_call_threadsafe_function(g_eventTsfn, payload, napi_tsfn_nonblocking);
  if (status != napi_ok) {
    delete payload;
  }
}

Session* FindSession(int64_t handle) {
  auto it = g_sessions.find(handle);
  return it == g_sessions.end() ? nullptr : it->second.get();
}

// Must be called with g_mutex held.
void DestroyWindowLocked(int64_t handle) {
  auto it = g_windows.find(handle);
  if (it == g_windows.end()) {
    return;
  }
  if (it->second != nullptr) {
    OH_NativeWindow_DestroyNativeWindow(it->second);
  }
  g_windows.erase(it);
}

// Best-effort scrub of a transient credential buffer.
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

napi_value CreateUndefined(napi_env env) {
  napi_value result = nullptr;
  napi_get_undefined(env, &result);
  return result;
}

// --- NAPI methods ---------------------------------------------------------

napi_value CreateSession(napi_env env, napi_callback_info) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const int64_t id = g_nextId++;
  auto session = std::make_unique<Session>();
  // Bind the handle into the callback so events can be routed to the owning
  // session window when several sessions run at once.
  session->SetEventFn([id](SessionEvent event, const std::string& data) {
    OnSessionEvent(id, event, data);
  });
  g_sessions[id] = std::move(session);
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
  std::unique_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sessions.find(handle);
    if (it == g_sessions.end()) {
      return CreateUndefined(env);
    }
    session = std::move(it->second);
    g_sessions.erase(it);
    DestroyWindowLocked(handle);
  }
  session.reset();
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
  options.colorDepth = GetIntProperty(env, args[1], "colorDepth", 32);
  options.ignoreCertificate = GetBoolProperty(env, args[1], "ignoreCertificate", true);
  options.enableClipboard = GetBoolProperty(env, args[1], "enableClipboard", true);
  options.enableAudio = GetBoolProperty(env, args[1], "enableAudio", true);
  options.enableGfx = GetBoolProperty(env, args[1], "enableGfx", true);
  options.enableH264 = GetBoolProperty(env, args[1], "enableH264", true);
  options.enableRemoteFx = GetBoolProperty(env, args[1], "enableRemoteFx", true);
  options.performanceFlags = GetIntProperty(env, args[1], "performanceFlags", 0);
  options.gatewayHost = GetStringProperty(env, args[1], "gatewayHost");
  options.gatewayPort = GetIntProperty(env, args[1], "gatewayPort", 443);
  options.gatewayUsername = GetStringProperty(env, args[1], "gatewayUsername");
  options.gatewayPassword = GetStringProperty(env, args[1], "gatewayPassword");
  options.gatewayDomain = GetStringProperty(env, args[1], "gatewayDomain");

  if (options.host.empty()) {
    return CreateBool(env, false);
  }

  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
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
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
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
  std::lock_guard<std::mutex> lock(g_mutex);
  Session* session = FindSession(handle);
  if (session == nullptr) {
    OH_NativeWindow_DestroyNativeWindow(window);
    return CreateUndefined(env);
  }
  DestroyWindowLocked(handle);
  g_windows[handle] = window;
  session->renderer()->SetSurface(window, width, height);
  session->renderer()->Prepare();
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
  std::lock_guard<std::mutex> lock(g_mutex);
  Session* session = FindSession(handle);
  if (session != nullptr) {
    session->renderer()->ResizeSurface(width, height);
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
  std::lock_guard<std::mutex> lock(g_mutex);
  Session* session = FindSession(handle);
  if (session != nullptr) {
    session->renderer()->DestroySurface();
  }
  DestroyWindowLocked(handle);
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
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  const uint16_t ux = static_cast<uint16_t>(x < 0 ? 0 : x);
  const uint16_t uy = static_cast<uint16_t>(y < 0 ? 0 : y);
  return CreateBool(env,
                    session->SendMouse(static_cast<uint16_t>(flags), ux, uy));
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
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
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
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
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
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  return CreateBool(env,
                    session->SendUnicode(static_cast<uint16_t>(codepoint), down));
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
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  session->SetLocalClipboardText(text);
  return CreateBool(env, true);
}

napi_value SetClipboardHtml(napi_env env, napi_callback_info info) {
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
  std::string html(length, '\0');
  napi_get_value_string_utf8(env, args[1], html.data(), length + 1, &length);
  html.resize(length);
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  session->SetLocalClipboardHtml(html);
  return CreateBool(env, true);
}

napi_value SetClipboardImage(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value args[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t pixelFormat = 0;
  if (argc < 5 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int32(env, args[1], &width) != napi_ok ||
      napi_get_value_int32(env, args[2], &height) != napi_ok ||
      napi_get_value_int32(env, args[3], &pixelFormat) != napi_ok || width <= 0 ||
      height <= 0) {
    return CreateBool(env, false);
  }
  void* pixelData = nullptr;
  size_t byteCount = 0;
  if (napi_get_arraybuffer_info(env, args[4], &pixelData, &byteCount) != napi_ok ||
      pixelData == nullptr) {
    return CreateBool(env, false);
  }
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
  if (session == nullptr) {
    return CreateBool(env, false);
  }
  session->SetLocalClipboardImage(static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height), pixelFormat,
                                  static_cast<const uint8_t*>(pixelData), byteCount);
  return CreateBool(env, true);
}

napi_value IsAudioSupported(napi_env env, napi_callback_info) {
  return CreateBool(env, AudioOutput::Supported());
}

napi_value SetTouchHighRate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  bool enabled = false;
  if (argc < 1 || napi_get_value_bool(env, args[0], &enabled) != napi_ok) {
    return CreateBool(env, false);
  }
  Session::SetTouchHighRate(enabled);
  return CreateBool(env, true);
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

napi_value SetHardwareDecode(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  bool enabled = true;
  if (argc < 1 || napi_get_value_bool(env, args[0], &enabled) != napi_ok) {
    return CreateBool(env, false);
  }
  Session::SetHardwareDecode(enabled);
  return CreateBool(env, true);
}

napi_value SetRfxDump(napi_env env, napi_callback_info info) {
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
  Session::SetRfxDump(enabled, dir);
  return CreateBool(env, true);
}

// Dev/test helper: returns a one-line description of the device GLES compute
// capability (see GpuComputeInfo::Describe).
napi_value GpuComputeInfo(napi_env env, napi_callback_info) {
  const std::string desc = hmrdp::GetGpuComputeInfo().Describe();
  napi_value out = nullptr;
  napi_create_string_utf8(env, desc.c_str(), desc.size(), &out);
  return out;
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

// Dev-only: replay a recorded hmrdp_gfx.bin capture straight to the screen.
napi_value StartGfxReplayTest(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value args[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  std::string out;
  if (argc < 4) {
    out = "failed: need (surfaceId, surfaceW, surfaceH, gfxPath[, route])";
  } else {
    const std::string surfaceId = GetStringArg(env, args[0]);
    int32_t surfaceW = 0;
    int32_t surfaceH = 0;
    napi_get_value_int32(env, args[1], &surfaceW);
    napi_get_value_int32(env, args[2], &surfaceH);
    const std::string gfxPath = GetStringArg(env, args[3]);
    int32_t route = 0;
    if (argc >= 5) {
      napi_get_value_int32(env, args[4], &route);
    }
    // 0 = GPU only (perf), 1 = CPU(gdi) only (perf reference),
    // 2 = both fed the same stream + per-frame pixel comparison (correctness).
    hmrdp::GfxReplayRoute replayRoute = hmrdp::GfxReplayRoute::kGpu;
    if (route == 1) {
      replayRoute = hmrdp::GfxReplayRoute::kCpu;
    } else if (route == 2) {
      replayRoute = hmrdp::GfxReplayRoute::kCompare;
    }
    const uint64_t sid = static_cast<uint64_t>(strtoull(surfaceId.c_str(), nullptr, 10));
    OHNativeWindow* window = nullptr;
    const int32_t err = OH_NativeWindow_CreateNativeWindowFromSurfaceId(sid, &window);
    if (err != 0 || window == nullptr) {
      out = "failed: native window";
    } else if (hmrdp::GfxReplay::Instance().Start(window, surfaceW, surfaceH, gfxPath,
                                                   replayRoute)) {
      out = "started " + hmrdp::GfxReplay::Instance().Stats();
    } else {
      out = "failed: " + hmrdp::GfxReplay::Instance().Stats();
    }
  }
  HMRDP_LOGI("gfx replay: %{public}s", out.c_str());
  napi_value result = nullptr;
  napi_create_string_utf8(env, out.c_str(), out.size(), &result);
  return result;
}

napi_value StopGfxReplayTest(napi_env env, napi_callback_info info) {
  (void)info;
  hmrdp::GfxReplay::Instance().Stop();
  return CreateUndefined(env);
}

napi_value ResizeGfxReplayTest(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, args[0], &width);
    napi_get_value_int32(env, args[1], &height);
  }
  hmrdp::GfxReplay::Instance().Resize(width, height);
  return CreateUndefined(env);
}

napi_value GfxReplayTestStats(napi_env env, napi_callback_info info) {
  (void)info;
  // Multi-line form for the on-device performance panel; the log keeps the
  // single-line variant so one record carries the whole summary.
  const std::string lines = hmrdp::GfxReplay::Instance().StatsLines();
  HMRDP_LOGI("gfx replay: %{public}s", hmrdp::GfxReplay::Instance().Stats().c_str());
  napi_value result = nullptr;
  napi_create_string_utf8(env, lines.c_str(), lines.size(), &result);
  return result;
}

// Dev-only: ClearCodec batch granularity for the GPU replay (union-rectangle
// pixel cap; 0 = one flush per command). Lets the sync-count vs mapped-bytes
// trade-off be measured on a real device.
napi_value SetGfxReplayBatchArea(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t pixels = 1 << 20;
  if (argc >= 1) {
    napi_get_value_int32(env, args[0], &pixels);
  }
  hmrdp::GfxReplay::Instance().SetClearBatchArea(pixels);
  HMRDP_LOGI("gfx replay: clear batch area = %{public}d px", pixels);
  return CreateUndefined(env);
}

// Dev/test (VULKAN-TODO §3.2): the Phase 0 device report - loader/device
// versions, the migration-relevant extensions, memory types and queue families.
napi_value VulkanInfo(napi_env env, napi_callback_info) {
  const hmrdp::VulkanCapabilities& caps = hmrdp::GetVulkanCapabilities();
  HMRDP_LOGI("vulkan caps: %{public}s", caps.Describe().c_str());
  const std::string lines = caps.DescribeLines();
  napi_value result = nullptr;
  napi_create_string_utf8(env, lines.c_str(), lines.size(), &result);
  return result;
}

// Dev/test (VULKAN-TODO §5 V0): bind this XComponent's surface to the Vulkan
// presenter and present the first solid frame. Does not touch any RDP session.
napi_value StartVulkanTest(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  std::string out;
  if (argc < 3) {
    out = "failed: need (surfaceId, surfaceW, surfaceH)";
  } else {
    const std::string surfaceId = GetStringArg(env, args[0]);
    int32_t surfaceW = 0;
    int32_t surfaceH = 0;
    napi_get_value_int32(env, args[1], &surfaceW);
    napi_get_value_int32(env, args[2], &surfaceH);
    const uint64_t sid = static_cast<uint64_t>(strtoull(surfaceId.c_str(), nullptr, 10));
    OHNativeWindow* window = nullptr;
    const int32_t err = OH_NativeWindow_CreateNativeWindowFromSurfaceId(sid, &window);
    if (err != 0 || window == nullptr) {
      out = "failed: native window";
    } else {
      std::lock_guard<std::mutex> lock(g_vkMutex);
      if (g_vkRenderer == nullptr) {
        g_vkRenderer = std::make_unique<hmrdp::VkRenderer>();
      }
      if (g_vkWindow != nullptr && g_vkWindow != window) {
        OH_NativeWindow_DestroyNativeWindow(g_vkWindow);
      }
      g_vkWindow = window;
      g_vkRenderer->SetSurface(window, surfaceW, surfaceH);
      out = g_vkRenderer->Prepare() ? ("ok " + g_vkRenderer->Describe())
                                    : ("failed: " + g_vkRenderer->lastError());
    }
  }
  HMRDP_LOGI("vulkan test start: %{public}s", out.c_str());
  napi_value result = nullptr;
  napi_create_string_utf8(env, out.c_str(), out.size(), &result);
  return result;
}

// Presents one more solid frame; the dev page drives this so repeated
// acquire/present and surface re-creation are exercised on device.
napi_value PresentVulkanTest(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t red = 16;
  int32_t green = 96;
  int32_t blue = 176;
  if (argc >= 3) {
    napi_get_value_int32(env, args[0], &red);
    napi_get_value_int32(env, args[1], &green);
    napi_get_value_int32(env, args[2], &blue);
  }
  const auto clamp = [](int32_t value) -> uint8_t {
    if (value < 0) {
      return 0;
    }
    return value > 255 ? 255 : static_cast<uint8_t>(value);
  };
  std::lock_guard<std::mutex> lock(g_vkMutex);
  if (g_vkRenderer == nullptr || !g_vkRenderer->ready()) {
    return CreateBool(env, false);
  }
  return CreateBool(env, g_vkRenderer->PresentClear(clamp(red), clamp(green), clamp(blue)));
}

napi_value ResizeVulkanTest(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, args[0], &width);
    napi_get_value_int32(env, args[1], &height);
  }
  std::lock_guard<std::mutex> lock(g_vkMutex);
  if (g_vkRenderer != nullptr) {
    g_vkRenderer->ResizeSurface(width, height);
  }
  return CreateUndefined(env);
}

napi_value StopVulkanTest(napi_env env, napi_callback_info) {
  std::lock_guard<std::mutex> lock(g_vkMutex);
  if (g_vkRenderer != nullptr) {
    g_vkRenderer->Reset();
    g_vkRenderer.reset();
  }
  if (g_vkWindow != nullptr) {
    OH_NativeWindow_DestroyNativeWindow(g_vkWindow);
    g_vkWindow = nullptr;
  }
  return CreateUndefined(env);
}

// Dev/test (VULKAN-TODO §5 V1): replay a capture through the Vulkan surface
// engine while FreeRDP's own gdi pipeline consumes the same bytes, and compare
// the two composed screens pixel by pixel. Frames carrying progressive/clear
// commands are excluded (V2/V3), so a clean run proves "fill + copy +
// uncompressed is pixel-exact". `present` (0/1) also blits to the XComponent.
napi_value StartVulkanEngineTest(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value args[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  std::string out;
  if (argc < 5) {
    out = "failed: need (gfxPath, surfaceId, surfaceW, surfaceH, present)";
  } else {
    const std::string gfxPath = GetStringArg(env, args[0]);
    const std::string surfaceId = GetStringArg(env, args[1]);
    int32_t surfaceW = 0;
    int32_t surfaceH = 0;
    int32_t present = 0;
    napi_get_value_int32(env, args[2], &surfaceW);
    napi_get_value_int32(env, args[3], &surfaceH);
    if (argc >= 5) {
      napi_get_value_int32(env, args[4], &present);
    }
    OHNativeWindow* window = nullptr;
    if (surfaceId.length() > 0) {
      const uint64_t sid = static_cast<uint64_t>(strtoull(surfaceId.c_str(), nullptr, 10));
      const int32_t err = OH_NativeWindow_CreateNativeWindowFromSurfaceId(sid, &window);
      if (err != 0 || window == nullptr) {
        window = nullptr;
      }
    }
    if (hmrdp::GfxVkSelfTest::Instance().Start(gfxPath, window, surfaceW, surfaceH,
                                               present != 0)) {
      out = "started " + hmrdp::GfxVkSelfTest::Instance().Stats();
    } else {
      out = "failed: " + hmrdp::GfxVkSelfTest::Instance().StatsLines();
    }
  }
  HMRDP_LOGI("vk selftest: %{public}s", out.c_str());
  napi_value result = nullptr;
  napi_create_string_utf8(env, out.c_str(), out.size(), &result);
  return result;
}

napi_value StopVulkanEngineTest(napi_env env, napi_callback_info) {
  hmrdp::GfxVkSelfTest::Instance().Stop();
  return CreateUndefined(env);
}

napi_value ResizeVulkanEngineTest(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, args[0], &width);
    napi_get_value_int32(env, args[1], &height);
  }
  hmrdp::GfxVkSelfTest::Instance().Resize(width, height);
  return CreateUndefined(env);
}

napi_value VulkanEngineTestStats(napi_env env, napi_callback_info) {
  const std::string lines = hmrdp::GfxVkSelfTest::Instance().StatsLines();
  HMRDP_LOGI("vk selftest: %{public}s", hmrdp::GfxVkSelfTest::Instance().Stats().c_str());
  napi_value result = nullptr;
  napi_create_string_utf8(env, lines.c_str(), lines.size(), &result);
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
      {"sendTouch", nullptr, SendTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sendKey", nullptr, SendKey, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sendUnicode", nullptr, SendUnicode, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setClipboardText", nullptr, SetClipboardText, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setClipboardHtml", nullptr, SetClipboardHtml, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setClipboardImage", nullptr, SetClipboardImage, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"onEvent", nullptr, OnEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"isAudioSupported", nullptr, IsAudioSupported, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setTouchHighRate", nullptr, SetTouchHighRate, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setRdpCursor", nullptr, SetRdpCursor, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setHardwareDecode", nullptr, SetHardwareDecode, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setRfxDump", nullptr, SetRfxDump, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"gpuComputeInfo", nullptr, GpuComputeInfo, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"startGfxReplayTest", nullptr, StartGfxReplayTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"stopGfxReplayTest", nullptr, StopGfxReplayTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"resizeGfxReplayTest", nullptr, ResizeGfxReplayTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"gfxReplayTestStats", nullptr, GfxReplayTestStats, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setGfxReplayBatchArea", nullptr, SetGfxReplayBatchArea, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"vulkanInfo", nullptr, VulkanInfo, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"startVulkanTest", nullptr, StartVulkanTest, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"presentVulkanTest", nullptr, PresentVulkanTest, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"resizeVulkanTest", nullptr, ResizeVulkanTest, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"stopVulkanTest", nullptr, StopVulkanTest, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"startVulkanEngineTest", nullptr, StartVulkanEngineTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"stopVulkanEngineTest", nullptr, StopVulkanEngineTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"resizeVulkanEngineTest", nullptr, ResizeVulkanEngineTest, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"vulkanEngineTestStats", nullptr, VulkanEngineTestStats, nullptr, nullptr, nullptr,
       napi_default, nullptr},
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
