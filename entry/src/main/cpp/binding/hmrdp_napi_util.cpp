#include "hmrdp_napi_util.h"

#include <multimodalinput/oh_input_manager.h>

#include <mutex>
#include <unordered_map>
#include <utility>

#include "hmrdp_log.h"

namespace hmrdp {
namespace binding {

// 快捷键穿透: one process-global key interceptor, delivered only while this app is
// focused. Ownership follows the focused session; the token-keyed registry keeps it generic.
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

}  // namespace binding
}  // namespace hmrdp
