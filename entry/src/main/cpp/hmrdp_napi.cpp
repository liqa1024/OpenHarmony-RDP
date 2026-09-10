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
#include <multimodalinput/oh_input_manager.h>
#include <window_manager/oh_window_event_filter.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "hmrdp_log.h"
#include "hmrdp_session.h"

namespace {

using hmrdp::RdpOptions;
using hmrdp::Session;
using hmrdp::SessionEvent;

std::mutex g_mutex;
std::map<int64_t, std::unique_ptr<Session>> g_sessions;
// Each session owns exactly one OHNativeWindow, keyed by session handle, so
// multiple concurrent sessions never share surface state.
std::map<int64_t, OHNativeWindow*> g_windows;
int64_t g_nextId = 1;

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

// --- Keyboard shortcut passthrough ---------------------------------------
// Window-level key filter only, no system_basic permission. Controlled solely
// by the "keyboardShortcutPassthrough" setting: while it is on and a session
// window is focused, every key event the filter receives is mapped and
// forwarded to the remote — nothing is deliberately held back. Keys the shell
// consumes before they ever reach the window (typically Meta/Win) simply cannot
// be captured this way; for those, winKeySubstitute lets one ordinary key be
// sent as Win instead.

std::mutex g_captureMutex;
bool g_captureActive = false;
int64_t g_captureHandle = 0;
int32_t g_captureWindowId = 0;
// KeyCode remapped to Win/Meta for the remote (0 = disabled).
int32_t g_substituteKeyCode = 0;
// Keys currently held down, encoded by PackKey(). Released when capture stops
// so a key-up swallowed by a focus change cannot leave a modifier stuck remote.
std::set<uint16_t> g_pressedKeys;

// Maps a HarmonyOS multimodal KeyCode to a PS/2 set-1 scancode. Mirrors
// entry/src/main/ets/utils/KeyMapper.ets; keep both tables in sync.
bool MapKeyCode(int32_t keyCode, uint8_t& scancode, bool& extended) {
  static const uint8_t kDigitScan[10] = {0x0B, 0x02, 0x03, 0x04, 0x05,
                                         0x06, 0x07, 0x08, 0x09, 0x0A};
  static const uint8_t kLetterScan[26] = {
      0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
      0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
  if (keyCode >= 2000 && keyCode <= 2009) {
    scancode = kDigitScan[keyCode - 2000];
    extended = false;
    return true;
  }
  if (keyCode >= 2017 && keyCode <= 2042) {
    scancode = kLetterScan[keyCode - 2017];
    extended = false;
    return true;
  }
  uint8_t scan = 0;
  bool ext = false;
  switch (keyCode) {
    case 2012: scan = 0x48; ext = true; break;   // DPAD_UP
    case 2013: scan = 0x50; ext = true; break;   // DPAD_DOWN
    case 2014: scan = 0x4B; ext = true; break;   // DPAD_LEFT
    case 2015: scan = 0x4D; ext = true; break;   // DPAD_RIGHT
    case 2043: scan = 0x33; break;               // comma
    case 2044: scan = 0x34; break;               // period
    case 2045: scan = 0x38; break;               // alt left
    case 2046: scan = 0x38; ext = true; break;   // alt right
    case 2047: scan = 0x2A; break;               // shift left
    case 2048: scan = 0x36; break;               // shift right
    case 2049: scan = 0x0F; break;               // tab
    case 2050: scan = 0x39; break;               // space
    case 2054: scan = 0x1C; break;               // enter
    case 2055: scan = 0x0E; break;               // backspace
    case 2056: scan = 0x29; break;               // grave
    case 2057: scan = 0x0C; break;               // minus
    case 2058: scan = 0x0D; break;               // equals
    case 2059: scan = 0x1A; break;               // left bracket
    case 2060: scan = 0x1B; break;               // right bracket
    case 2061: scan = 0x2B; break;               // backslash
    case 2062: scan = 0x27; break;               // semicolon
    case 2063: scan = 0x28; break;               // apostrophe
    case 2064: scan = 0x35; break;               // slash
    case 2067: scan = 0x5D; ext = true; break;   // menu
    case 2068: scan = 0x49; ext = true; break;   // page up
    case 2069: scan = 0x51; ext = true; break;   // page down
    case 2070: scan = 0x01; break;               // escape
    case 2071: scan = 0x53; ext = true; break;   // forward delete
    case 2072: scan = 0x1D; break;               // ctrl left
    case 2073: scan = 0x1D; ext = true; break;   // ctrl right
    case 2074: scan = 0x3A; break;               // caps lock
    case 2075: scan = 0x46; break;               // scroll lock
    case 2076: scan = 0x5B; ext = true; break;   // meta left
    case 2077: scan = 0x5C; ext = true; break;   // meta right
    case 2081: scan = 0x47; ext = true; break;   // home
    case 2082: scan = 0x4F; ext = true; break;   // end
    case 2083: scan = 0x52; ext = true; break;   // insert
    case 2090: scan = 0x3B; break;               // F1
    case 2091: scan = 0x3C; break;
    case 2092: scan = 0x3D; break;
    case 2093: scan = 0x3E; break;
    case 2094: scan = 0x3F; break;
    case 2095: scan = 0x40; break;
    case 2096: scan = 0x41; break;
    case 2097: scan = 0x42; break;
    case 2098: scan = 0x43; break;
    case 2099: scan = 0x44; break;
    case 2100: scan = 0x57; break;               // F11
    case 2101: scan = 0x58; break;               // F12
    default: return false;
  }
  scancode = scan;
  extended = ext;
  return true;
}

// Encodes a scancode + extended flag so it can be tracked in a set.
uint16_t PackKey(uint8_t scancode, bool extended) {
  return static_cast<uint16_t>(scancode) | (extended ? 0x100u : 0u);
}

// Sends a key-release for a tracked pressed key to the owning session.
void ReleaseKey(int64_t handle, uint16_t packed) {
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
  if (session != nullptr) {
    session->SendKey(static_cast<uint8_t>(packed & 0xFFu), false, (packed & 0x100u) != 0);
  }
}

// Forwards one filtered key event to the owning session. Returns true when the
// event was handled, so the window filter consumes it instead of letting ArkUI
// dispatch it (keys not in the map fall through to the ArkTS handler).
bool HandleCapturedKey(const Input_KeyEvent* event) {
  int64_t handle = 0;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    handle = g_captureHandle;
  }
  if (handle == 0) {
    return false;
  }
  const int32_t action = OH_Input_GetKeyEventAction(event);
  // A cancelled key sequence has no matching up events: release everything.
  if (action == KEY_ACTION_CANCEL) {
    std::set<uint16_t> pressed;
    {
      std::lock_guard<std::mutex> lock(g_captureMutex);
      pressed.swap(g_pressedKeys);
    }
    for (const uint16_t packed : pressed) {
      ReleaseKey(handle, packed);
    }
    return true;
  }
  const int32_t keyCode = OH_Input_GetKeyEventKeyCode(event);
  uint8_t scancode = 0;
  bool extended = false;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    // The user can remap one ordinary key (e.g. right Alt) to Win so that Win
    // combinations still work even though the shell keeps the real Win key.
    if (g_substituteKeyCode != 0 && keyCode == g_substituteKeyCode) {
      scancode = 0x5B;
      extended = true;
    }
  }
  if (scancode == 0 && !MapKeyCode(keyCode, scancode, extended)) {
    return false;
  }
  const bool down = action == KEY_ACTION_DOWN;
  const uint16_t packed = PackKey(scancode, extended);
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (down) {
      g_pressedKeys.insert(packed);
    } else {
      g_pressedKeys.erase(packed);
    }
  }
  Session* session = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    session = FindSession(handle);
  }
  if (session != nullptr) {
    session->SendKey(scancode, down, extended);
  }
  return true;
}

bool OnKeyFilter(Input_KeyEvent* event) {
  return HandleCapturedKey(event);
}

// Enables the per-window key filter. Returns false when it cannot be
// registered, so ArkTS keeps handling keys itself.
bool EnableKeyCapture(int64_t handle, int32_t windowId, int32_t substituteKeyCode) {
  if (windowId <= 0) {
    return false;
  }
  int64_t previousOwner = 0;
  std::set<uint16_t> previousPressed;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (g_captureActive && g_captureWindowId != windowId) {
      OH_NativeWindowManager_UnregisterKeyEventFilter(g_captureWindowId);
      g_captureActive = false;
    }
    if (!g_captureActive) {
      if (OH_NativeWindowManager_RegisterKeyEventFilter(windowId, OnKeyFilter) != OK) {
        HMRDP_LOGW("key capture: register window filter failed id=%{public}d", windowId);
        return false;
      }
      g_captureActive = true;
      HMRDP_LOGI("key capture: window filter active id=%{public}d", windowId);
    }
    if (g_captureHandle != 0 && g_captureHandle != handle) {
      previousOwner = g_captureHandle;
      previousPressed.swap(g_pressedKeys);
    }
    g_substituteKeyCode = substituteKeyCode;
    g_captureHandle = handle;
    g_captureWindowId = windowId;
  }
  // Release keys still held by a previous owner before handing over.
  for (const uint16_t packed : previousPressed) {
    ReleaseKey(previousOwner, packed);
  }
  return true;
}

// Disables capture only when the given session is the active owner, so a
// window that loses focus after another gained it cannot clear the new owner.
void DisableKeyCapture(int64_t handle) {
  int64_t owner = 0;
  std::set<uint16_t> pressed;
  {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (g_captureHandle != handle) {
      return;
    }
    owner = g_captureHandle;
    pressed.swap(g_pressedKeys);
    if (g_captureActive && g_captureWindowId > 0) {
      OH_NativeWindowManager_UnregisterKeyEventFilter(g_captureWindowId);
    }
    g_captureActive = false;
    g_captureHandle = 0;
    g_captureWindowId = 0;
    g_substituteKeyCode = 0;
  }
  if (!pressed.empty()) {
    HMRDP_LOGI("key capture: releasing %{public}d held key(s)",
               static_cast<int>(pressed.size()));
  }
  for (const uint16_t packed : pressed) {
    ReleaseKey(owner, packed);
  }
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
  DisableKeyCapture(handle);
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
  DisableKeyCapture(handle);
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

napi_value EnableKeyCaptureNapi(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  int32_t windowId = 0;
  int32_t substituteKeyCode = 0;
  if (argc < 3 || napi_get_value_int64(env, args[0], &handle) != napi_ok ||
      napi_get_value_int32(env, args[1], &windowId) != napi_ok ||
      napi_get_value_int32(env, args[2], &substituteKeyCode) != napi_ok) {
    return CreateBool(env, false);
  }
  return CreateBool(env, EnableKeyCapture(handle, windowId, substituteKeyCode));
}

napi_value DisableKeyCaptureNapi(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int64_t handle = 0;
  if (argc < 1 || napi_get_value_int64(env, args[0], &handle) != napi_ok) {
    return CreateUndefined(env);
  }
  DisableKeyCapture(handle);
  return CreateUndefined(env);
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
      {"enableKeyCapture", nullptr, EnableKeyCaptureNapi, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"disableKeyCapture", nullptr, DisableKeyCaptureNapi, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setClipboardText", nullptr, SetClipboardText, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"onEvent", nullptr, OnEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
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
