#include "hmrdp_napi_util.h"

#include <native_window/external_window.h>
#include <napi/native_api.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "hmrdp_log.h"
#include "hmrdp_moonlight.h"
#include "hmrdp_moonlight_audio.h"
#include "hmrdp_moonlight_session.h"

namespace hmrdp {
namespace binding {

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

napi_value MoonlightVideoMaxFps(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t videoFormat = 0;
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 1) {
    napi_get_value_int32(env, args[0], &videoFormat);
  }
  if (argc >= 2) {
    napi_get_value_int32(env, args[1], &width);
  }
  if (argc >= 3) {
    napi_get_value_int32(env, args[2], &height);
  }
  return CreateInt(env, hmrdp::MoonlightVideoMaxFps(videoFormat, width, height));
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
  int magnificationMilli = 1000;
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
  setInt("magnificationMilli", payload->magnificationMilli);
  setInt("shapeId", payload->shapeId);
  setBool("visible", payload->visible);
  setBool("hasShape", payload->hasShape);
  if (!payload->bytes.empty()) {
    setInt(payload->isClipboard ? "dataHandle" : "imageHandle", AddBufferHandle(payload->bytes));
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
  payload->magnificationMilli = shape.magnificationMilli;
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
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 2) {
    napi_get_value_int32(env, args[1], &width);
  }
  if (argc >= 3) {
    napi_get_value_int32(env, args[2], &height);
  }
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
    if (hmrdp::MoonlightSessionActive() && !hmrdp::MoonlightVideoBufferMode()) {
      // Fallback (no presenter): the codec itself holds the old window, so a live replacement would
      // need a decoder restart. Reject it rather than presenting into a dead surface.
      OH_NativeWindow_DestroyNativeWindow(window);
      return CreateBool(env, false);
    }
    OHNativeWindow* previous = g_moonlightWindow;
    g_moonlightWindow = window;
    hmrdp::MoonlightSessionSetWindow(window, width, height);
    // The presenter has already rebuilt its swapchain on the new window; the old native window is
    // no longer referenced (surface mode was rejected above).
    OH_NativeWindow_DestroyNativeWindow(previous);
    return CreateBool(env, true);
  }
  g_moonlightWindow = window;
  hmrdp::MoonlightSessionSetWindow(window, width, height);
  return CreateBool(env, true);
}

napi_value MoonlightSessionClearSurface(napi_env env, napi_callback_info) {
  hmrdp::MoonlightSessionSetWindow(nullptr, 0, 0);
  if (g_moonlightWindow != nullptr && !hmrdp::MoonlightSessionActive()) {
    OH_NativeWindow_DestroyNativeWindow(g_moonlightWindow);
    g_moonlightWindow = nullptr;
  }
  return CreateUndefined(env);
}

napi_value MoonlightSessionResizeSurface(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t width = 0;
  int32_t height = 0;
  if (argc >= 1) {
    napi_get_value_int32(env, args[0], &width);
  }
  if (argc >= 2) {
    napi_get_value_int32(env, args[1], &height);
  }
  hmrdp::MoonlightSessionResizeSurface(width, height);
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
  config.streamWidth = GetIntProperty(env, args[0], "streamWidth", 0);
  config.streamHeight = GetIntProperty(env, args[0], "streamHeight", 0);
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
  config.srBackend = GetIntProperty(env, args[0], "srBackend", 0);
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
  return CreateInt(env, hmrdp::MoonlightSessionMouseMove(dx, dy));
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
  return CreateInt(env, hmrdp::MoonlightSessionMousePosition(x, y, refW, refH));
}

napi_value MoonlightSessionMouseButton(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t action = 0;
  int32_t button = 0;
  napi_get_value_int32(env, args[0], &action);
  napi_get_value_int32(env, args[1], &button);
  return CreateInt(env, hmrdp::MoonlightSessionMouseButton(action, button));
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
  return CreateInt(env, hmrdp::MoonlightSessionKey(keyCode, action, modifiers));
}

napi_value MoonlightSessionScroll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t clicks = 0;
  napi_get_value_int32(env, args[0], &clicks);
  return CreateInt(env, hmrdp::MoonlightSessionScroll(clicks));
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
  return CreateInt(env, hmrdp::MoonlightSessionTouch(eventType, pointerId, static_cast<float>(x),
                                                     static_cast<float>(y),
                                                     static_cast<float>(pressure)));
}

// eventTypes / pointerIds / x / y / pressure are parallel arrays; width/height are
// millimetres and buttonState a LI_TOUCHPAD_BUTTON_* mask.
napi_value MoonlightSessionTouchpadFrame(napi_env env, napi_callback_info info) {
  constexpr uint32_t kMaxContacts = 5;
  size_t argc = 8;
  napi_value args[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  if (argc < 8) {
    // Unreachable: the caller always hands over the eight arguments.
    return CreateInt(env, 0);
  }
  int32_t widthMm = 0;
  int32_t heightMm = 0;
  int32_t buttonState = 0;
  napi_get_value_int32(env, args[5], &widthMm);
  napi_get_value_int32(env, args[6], &heightMm);
  napi_get_value_int32(env, args[7], &buttonState);

  uint32_t count = 0;
  if (napi_get_array_length(env, args[0], &count) != napi_ok) {
    // Unreachable: the caller always hands over real arrays.
    return CreateInt(env, 0);
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
  return CreateInt(env, hmrdp::MoonlightSessionTouchpadFrame(static_cast<int>(count), eventTypes,
                                                             pointerIds, x, y, pressure, widthMm,
                                                             heightMm, buttonState));
}

napi_value MoonlightSessionHighResScroll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t amount = 0;
  napi_get_value_int32(env, args[0], &amount);
  return CreateInt(env, hmrdp::MoonlightSessionHighResScroll(amount));
}

napi_value MoonlightSessionHighResHScroll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t amount = 0;
  napi_get_value_int32(env, args[0], &amount);
  return CreateInt(env, hmrdp::MoonlightSessionHighResHScroll(amount));
}

// controllerNumber / activeGamepadMask / buttonFlags / triggers / sticks, as the library takes them.
napi_value MoonlightSessionController(napi_env env, napi_callback_info info) {
  size_t argc = 9;
  napi_value args[9] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t values[9] = {0};
  for (size_t i = 0; i < 9; i++) {
    napi_get_value_int32(env, args[i], &values[i]);
  }
  return CreateInt(env, hmrdp::MoonlightSessionControllerState(values[0], values[1], values[2],
                                                               values[3], values[4], values[5],
                                                               values[6], values[7], values[8]));
}

napi_value MoonlightSessionControllerArrival(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value args[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  int32_t controllerNumber = 0;
  int32_t activeGamepadMask = 0;
  int32_t type = 0;
  int32_t supportedButtonFlags = 0;
  int32_t capabilities = 0;
  napi_get_value_int32(env, args[0], &controllerNumber);
  napi_get_value_int32(env, args[1], &activeGamepadMask);
  napi_get_value_int32(env, args[2], &type);
  napi_get_value_int32(env, args[3], &supportedButtonFlags);
  napi_get_value_int32(env, args[4], &capabilities);
  return CreateInt(env, hmrdp::MoonlightSessionControllerArrival(controllerNumber,
                                                                 activeGamepadMask, type,
                                                                 supportedButtonFlags, capabilities));
}

napi_value MoonlightSessionText(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  size_t length = 0;
  if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &length) != napi_ok) {
    // Unreachable: the caller always hands over a string.
    return CreateInt(env, 0);
  }
  std::string text;
  text.resize(length + 1);
  napi_get_value_string_utf8(env, args[0], text.data(), length + 1, &length);
  return CreateInt(env, hmrdp::MoonlightSessionText(text.c_str(), static_cast<int>(length)));
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
  setInt("presentedFrames", static_cast<int64_t>(telemetry.presentedFrames));
  setInt("submittedFrames", static_cast<int64_t>(telemetry.submittedFrames));
  setInt("droppedFrames", static_cast<int64_t>(telemetry.droppedFrames));
  setInt("presentDroppedFrames", static_cast<int64_t>(telemetry.presentDroppedFrames));
  setInt("handoffReplacedFrames", static_cast<int64_t>(telemetry.handoffReplacedFrames));
  setInt("refreshDroppedFrames", static_cast<int64_t>(telemetry.refreshDroppedFrames));
  setInt("displayRefreshRateHz", telemetry.displayRefreshRateHz);
  setInt("bytes", static_cast<int64_t>(telemetry.bytes));
  setInt("hostLatencyTenthMs", static_cast<int64_t>(telemetry.hostLatencyTenthMs));
  setInt("hostLatencyFrames", static_cast<int64_t>(telemetry.hostLatencyFrames));
  setInt("decodeRoundTripUs", static_cast<int64_t>(telemetry.decodeRoundTripUs));
  setInt("decodeRoundTripFrames", static_cast<int64_t>(telemetry.decodeRoundTripFrames));
  setInt("gpuCopyUs", static_cast<int64_t>(telemetry.gpuCopyUs));
  setInt("gpuSrUs", static_cast<int64_t>(telemetry.gpuSrUs));
  setInt("gpuBlitUs", static_cast<int64_t>(telemetry.gpuBlitUs));
  setInt("presentWaitUs", static_cast<int64_t>(telemetry.presentWaitUs));
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

}  // namespace binding
}  // namespace hmrdp
