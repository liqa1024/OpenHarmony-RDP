#include "hmrdp_gamepad_kit.h"

#include <atomic>
#include <cstring>

#include <GameControllerKit/game_device.h>
#include <GameControllerKit/game_device_event.h>
#include <GameControllerKit/game_pad.h>
#include <GameControllerKit/game_pad_event.h>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

std::atomic<bool> g_active{false};
std::atomic<GamepadKitCallback> g_callback{nullptr};

std::string CopyString(char* text) {
  if (text == nullptr) {
    return std::string();
  }
  return std::string(text);
}

void OnDeviceEvent(const GameDevice_DeviceEvent* event) {
  const GamepadKitCallback callback = g_callback.load();
  if (callback == nullptr || event == nullptr) {
    return;
  }
  GameDevice_StatusChangedType changed = OFFLINE;
  OH_GameDevice_DeviceEvent_GetChangedType(event, &changed);
  GameDevice_DeviceInfo* info = nullptr;
  GamepadKitDevice device;
  if (OH_GameDevice_DeviceEvent_GetDeviceInfo(event, &info) == GAME_CONTROLLER_SUCCESS
      && info != nullptr) {
    char* key = nullptr;
    char* name = nullptr;
    OH_GameDevice_DeviceInfo_GetDeviceId(info, &key);
    OH_GameDevice_DeviceInfo_GetName(info, &name);
    device.key = CopyString(key);
    device.name = CopyString(name);
    OH_GameDevice_DestroyDeviceInfo(&info);
  }
  HMRDP_LOGI("gamepad kit: device %{public}s %{public}s", changed == ONLINE ? "online" : "offline",
             device.name.c_str());
  callback(changed == ONLINE ? GamepadKitEvent::kDeviceOnline : GamepadKitEvent::kDeviceOffline,
           GamepadAxisSource::kDpad, device, 0.0f, 0.0f);
}

// One callback serves every axis monitor: the event names the control it came from, and the values
// are read per the documented table (see the constants in the header).
void OnAxisEvent(const GamePad_AxisEvent* event) {
  const GamepadKitCallback callback = g_callback.load();
  if (callback == nullptr || event == nullptr) {
    return;
  }
  GamePad_AxisSourceType source = DPAD;
  if (OH_GamePad_AxisEvent_GetAxisSourceType(event, &source) != GAME_CONTROLLER_SUCCESS) {
    return;
  }
  char* key = nullptr;
  OH_GamePad_AxisEvent_GetDeviceId(event, &key);
  GamepadKitDevice device;
  device.key = CopyString(key);
  double first = 0;
  double second = 0;
  switch (source) {
    case LEFT_THUMBSTICK:
      OH_GamePad_AxisEvent_GetXAxisValue(event, &first);
      OH_GamePad_AxisEvent_GetYAxisValue(event, &second);
      break;
    case RIGHT_THUMBSTICK:
      OH_GamePad_AxisEvent_GetZAxisValue(event, &first);
      OH_GamePad_AxisEvent_GetRZAxisValue(event, &second);
      break;
    case LEFT_TRIGGER:
      OH_GamePad_AxisEvent_GetBrakeAxisValue(event, &first);
      break;
    case RIGHT_TRIGGER:
      OH_GamePad_AxisEvent_GetGasAxisValue(event, &first);
      break;
    case DPAD:
    default:
      OH_GamePad_AxisEvent_GetHatXAxisValue(event, &first);
      OH_GamePad_AxisEvent_GetHatYAxisValue(event, &second);
      break;
  }
  callback(GamepadKitEvent::kAxis, static_cast<GamepadAxisSource>(source), device,
           static_cast<float>(first), static_cast<float>(second));
}

bool RegisterAxisMonitors() {
  const GameController_ErrorCode results[] = {
      OH_GamePad_LeftThumbstick_RegisterAxisInputMonitor(&OnAxisEvent),
      OH_GamePad_RightThumbstick_RegisterAxisInputMonitor(&OnAxisEvent),
      OH_GamePad_Dpad_RegisterAxisInputMonitor(&OnAxisEvent),
      OH_GamePad_LeftTrigger_RegisterAxisInputMonitor(&OnAxisEvent),
      OH_GamePad_RightTrigger_RegisterAxisInputMonitor(&OnAxisEvent),
  };
  for (const GameController_ErrorCode result : results) {
    if (result != GAME_CONTROLLER_SUCCESS) {
      return false;
    }
  }
  return true;
}

void UnregisterAxisMonitors() {
  OH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor();
  OH_GamePad_RightThumbstick_UnregisterAxisInputMonitor();
  OH_GamePad_Dpad_UnregisterAxisInputMonitor();
  OH_GamePad_LeftTrigger_UnregisterAxisInputMonitor();
  OH_GamePad_RightTrigger_UnregisterAxisInputMonitor();
}

}  // namespace

bool GamepadKitStart(GamepadKitCallback callback) {
  if (g_active.load()) {
    return true;
  }
  g_callback.store(callback);
  if (OH_GameDevice_RegisterDeviceMonitor(&OnDeviceEvent) != GAME_CONTROLLER_SUCCESS) {
    g_callback.store(nullptr);
    HMRDP_LOGW("gamepad kit: device monitor refused");
    return false;
  }
  if (!RegisterAxisMonitors()) {
    OH_GameDevice_UnregisterDeviceMonitor();
    g_callback.store(nullptr);
    HMRDP_LOGW("gamepad kit: axis monitor refused");
    return false;
  }
  g_active.store(true);
  HMRDP_LOGI("gamepad kit: axis monitors registered");
  return true;
}

void GamepadKitStop() {
  if (!g_active.load()) {
    return;
  }
  UnregisterAxisMonitors();
  OH_GameDevice_UnregisterDeviceMonitor();
  g_active.store(false);
  g_callback.store(nullptr);
}

bool GamepadKitActive() {
  return g_active.load();
}

std::vector<GamepadKitDevice> GamepadKitDevices() {
  std::vector<GamepadKitDevice> devices;
  GameDevice_AllDeviceInfos* all = nullptr;
  if (OH_GameDevice_GetAllDeviceInfos(&all) != GAME_CONTROLLER_SUCCESS || all == nullptr) {
    return devices;
  }
  int32_t count = 0;
  if (OH_GameDevice_AllDeviceInfos_GetCount(all, &count) != GAME_CONTROLLER_SUCCESS) {
    OH_GameDevice_DestroyAllDeviceInfos(&all);
    return devices;
  }
  for (int32_t i = 0; i < count; i++) {
    GameDevice_DeviceInfo* info = nullptr;
    if (OH_GameDevice_AllDeviceInfos_GetDeviceInfo(all, i, &info) != GAME_CONTROLLER_SUCCESS
        || info == nullptr) {
      continue;
    }
    GamepadKitDevice device;
    char* key = nullptr;
    char* name = nullptr;
    OH_GameDevice_DeviceInfo_GetDeviceId(info, &key);
    OH_GameDevice_DeviceInfo_GetName(info, &name);
    device.key = CopyString(key);
    device.name = CopyString(name);
    OH_GameDevice_DestroyDeviceInfo(&info);
    devices.push_back(device);
  }
  OH_GameDevice_DestroyAllDeviceInfos(&all);
  return devices;
}

}  // namespace hmrdp
