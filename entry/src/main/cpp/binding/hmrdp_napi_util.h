#ifndef HMRDP_NAPI_UTIL_H
#define HMRDP_NAPI_UTIL_H

#include <napi/native_api.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hmrdp {
namespace binding {

// 快捷键穿透: one process-global key interceptor, delivered only while this app is
// focused. Ownership follows the focused session; the token-keyed registry keeps it generic.
using InterceptSink = std::function<void(int32_t keyCode, bool down)>;

napi_value CreateInt(napi_env env, int64_t value);
napi_value CreateBool(napi_env env, bool value);
napi_value CreateStringUtf8(napi_env env, const std::string& value);
napi_value CreateUndefined(napi_env env);

std::string GetStringArgument(napi_env env, napi_value value);
std::string GetStringProperty(napi_env env, napi_value object, const char* name);
int32_t GetIntProperty(napi_env env, napi_value object, const char* name, int32_t fallback);
bool GetBoolProperty(napi_env env, napi_value object, const char* name, bool fallback);

// Bytes handed to ArkTS through a handle read by `bufferRead` / `bufferRelease`.
int64_t AddBufferHandle(const std::vector<uint8_t>& bytes);

void RegisterInterceptSink(int64_t token, InterceptSink sink);
void UnregisterInterceptSink(int64_t token);
bool SetInterceptOwner(int64_t token, bool enabled);

// Exports defined by the domain translation units; hmrdp_napi.cpp owns the module table.
napi_value MoonlightProbe(napi_env env, napi_callback_info info);
napi_value MoonlightVideoFormats(napi_env env, napi_callback_info info);
napi_value MoonlightVideoMaxFps(napi_env env, napi_callback_info info);
napi_value MoonlightVideoCodecSupport(napi_env env, napi_callback_info info);
napi_value MoonlightVideoCodec(napi_env env, napi_callback_info info);
napi_value MoonlightVideoWarning(napi_env env, napi_callback_info info);
napi_value MoonlightSetSessionListener(napi_env env, napi_callback_info info);
napi_value MoonlightSendClipboard(napi_env env, napi_callback_info info);
napi_value MoonlightSetShortcutPassthrough(napi_env env, napi_callback_info info);
napi_value MoonlightSessionSetSurface(napi_env env, napi_callback_info info);
napi_value MoonlightSessionClearSurface(napi_env env, napi_callback_info info);
napi_value MoonlightSessionResizeSurface(napi_env env, napi_callback_info info);
napi_value MoonlightSessionStart(napi_env env, napi_callback_info info);
napi_value MoonlightSessionStop(napi_env env, napi_callback_info info);
napi_value MoonlightSessionActive(napi_env env, napi_callback_info info);
napi_value MoonlightAudioRead(napi_env env, napi_callback_info info);
napi_value MoonlightAudioFormat(napi_env env, napi_callback_info info);
napi_value MoonlightLaunchQueryParameters(napi_env env, napi_callback_info info);
napi_value MoonlightSessionMouseMove(napi_env env, napi_callback_info info);
napi_value MoonlightSessionMousePosition(napi_env env, napi_callback_info info);
napi_value MoonlightSessionMouseButton(napi_env env, napi_callback_info info);
napi_value MoonlightSessionKey(napi_env env, napi_callback_info info);
napi_value MoonlightSessionScroll(napi_env env, napi_callback_info info);
napi_value MoonlightSessionTouch(napi_env env, napi_callback_info info);
napi_value MoonlightSessionTouchpadFrame(napi_env env, napi_callback_info info);
napi_value MoonlightSessionHighResScroll(napi_env env, napi_callback_info info);
napi_value MoonlightSessionHighResHScroll(napi_env env, napi_callback_info info);
napi_value MoonlightSessionController(napi_env env, napi_callback_info info);
napi_value MoonlightSessionControllerArrival(napi_env env, napi_callback_info info);
napi_value MoonlightSessionText(napi_env env, napi_callback_info info);
napi_value MoonlightSessionHostFeatures(napi_env env, napi_callback_info info);
napi_value MoonlightGetTelemetry(napi_env env, napi_callback_info info);

napi_value GamepadKitSetListener(napi_env env, napi_callback_info info);
napi_value GamepadKitStopListener(napi_env env, napi_callback_info info);
napi_value GamepadKitDevices(napi_env env, napi_callback_info info);

}  // namespace binding
}  // namespace hmrdp

#endif
