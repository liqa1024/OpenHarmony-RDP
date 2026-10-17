#include "hmrdp_napi_util.h"

#include <napi/native_api.h>

#include <memory>
#include <string>
#include <vector>

#include "hmrdp_gamepad_kit.h"

namespace hmrdp {
namespace binding {

// The Game Controller Kit labels an axis event with the control it came from, so the client never
// has to work out which pair of axes carries the right stick or the triggers. Its device identity is
// an opaque string, hence the key/name the JS side correlates with the multimodal input device.
struct GamepadKitPayload {
  int event = 0;
  int source = 0;
  std::string key;
  std::string name;
  float value1 = 0;
  float value2 = 0;
};

napi_threadsafe_function g_gamepadKitTsfn = nullptr;

void CallJsGamepadKitEvent(napi_env env, napi_value jsCallback, void*, void* data) {
  std::unique_ptr<GamepadKitPayload> payload(static_cast<GamepadKitPayload*>(data));
  if (env == nullptr || jsCallback == nullptr || payload == nullptr) {
    return;
  }
  napi_value eventValue = nullptr;
  napi_value sourceValue = nullptr;
  napi_create_int32(env, payload->event, &eventValue);
  napi_create_int32(env, payload->source, &sourceValue);
  napi_value extra = nullptr;
  napi_create_object(env, &extra);
  napi_value key = nullptr;
  napi_create_string_utf8(env, payload->key.c_str(), payload->key.size(), &key);
  napi_set_named_property(env, extra, "deviceKey", key);
  napi_value name = nullptr;
  napi_create_string_utf8(env, payload->name.c_str(), payload->name.size(), &name);
  napi_set_named_property(env, extra, "name", name);
  napi_value first = nullptr;
  napi_create_double(env, payload->value1, &first);
  napi_set_named_property(env, extra, "value1", first);
  napi_value second = nullptr;
  napi_create_double(env, payload->value2, &second);
  napi_set_named_property(env, extra, "value2", second);
  napi_value argv[3] = {eventValue, sourceValue, extra};
  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_call_function(env, global, jsCallback, 3, argv, nullptr);
}

void OnGamepadKitEvent(hmrdp::GamepadKitEvent event, hmrdp::GamepadAxisSource source,
                       const hmrdp::GamepadKitDevice& device, float value1, float value2) {
  if (g_gamepadKitTsfn == nullptr) {
    return;
  }
  GamepadKitPayload* payload = new GamepadKitPayload();
  payload->event = static_cast<int>(event);
  payload->source = static_cast<int>(source);
  payload->key = device.key;
  payload->name = device.name;
  payload->value1 = value1;
  payload->value2 = value2;
  if (napi_call_threadsafe_function(g_gamepadKitTsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
    delete payload;
  }
}

// Returns whether the Kit took over the axis path; the caller keeps its own path otherwise.
napi_value GamepadKitSetListener(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 1) {
    return CreateBool(env, false);
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_function) {
    return CreateBool(env, false);
  }
  if (g_gamepadKitTsfn != nullptr) {
    napi_release_threadsafe_function(g_gamepadKitTsfn, napi_tsfn_release);
    g_gamepadKitTsfn = nullptr;
  }
  napi_value resourceName = nullptr;
  napi_create_string_utf8(env, "HmRdpGamepadKitEvent", NAPI_AUTO_LENGTH, &resourceName);
  napi_create_threadsafe_function(env, args[0], nullptr, resourceName, 0, 1, nullptr, nullptr,
                                  nullptr, CallJsGamepadKitEvent, &g_gamepadKitTsfn);
  if (g_gamepadKitTsfn == nullptr || !hmrdp::GamepadKitStart(&OnGamepadKitEvent)) {
    if (g_gamepadKitTsfn != nullptr) {
      napi_release_threadsafe_function(g_gamepadKitTsfn, napi_tsfn_release);
      g_gamepadKitTsfn = nullptr;
    }
    return CreateBool(env, false);
  }
  return CreateBool(env, true);
}

napi_value GamepadKitStopListener(napi_env env, napi_callback_info) {
  hmrdp::GamepadKitStop();
  if (g_gamepadKitTsfn != nullptr) {
    napi_release_threadsafe_function(g_gamepadKitTsfn, napi_tsfn_release);
    g_gamepadKitTsfn = nullptr;
  }
  return CreateUndefined(env);
}

// [{ key, name }] for the online controllers.
napi_value GamepadKitDevices(napi_env env, napi_callback_info) {
  const std::vector<hmrdp::GamepadKitDevice> devices = hmrdp::GamepadKitDevices();
  napi_value array = nullptr;
  napi_create_array_with_length(env, devices.size(), &array);
  for (size_t i = 0; i < devices.size(); i++) {
    napi_value item = nullptr;
    napi_create_object(env, &item);
    napi_value key = nullptr;
    napi_create_string_utf8(env, devices[i].key.c_str(), devices[i].key.size(), &key);
    napi_set_named_property(env, item, "key", key);
    napi_value name = nullptr;
    napi_create_string_utf8(env, devices[i].name.c_str(), devices[i].name.size(), &name);
    napi_set_named_property(env, item, "name", name);
    napi_set_element(env, array, i, item);
  }
  return array;
}

}  // namespace binding
}  // namespace hmrdp
