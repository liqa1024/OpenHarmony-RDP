#ifndef HMRDP_GAMEPAD_KIT_H
#define HMRDP_GAMEPAD_KIT_H

#include <string>
#include <vector>

namespace hmrdp {

// GamePad_AxisSourceType, in the platform's own order.
enum class GamepadAxisSource : int {
  kDpad = 0,
  kLeftStick = 1,
  kRightStick = 2,
  kLeftTrigger = 3,
  kRightTrigger = 4,
};

enum class GamepadKitEvent : int {
  kDeviceOnline = 0,
  kDeviceOffline = 1,
  kAxis = 2,
};

struct GamepadKitDevice {
  // The platform's own device key (opaque, unique per plugged device) and the device name.
  std::string key;
  std::string name;
};

// Called from the platform's callback thread; must not block. Which axis values an event carries
// follows the documented table for its source: the sticks and the D-pad carry value1/value2, a
// trigger carries value1.
using GamepadKitCallback = void (*)(GamepadKitEvent event, GamepadAxisSource source,
                                    const GamepadKitDevice& device, float value1, float value2);

// Registers the Game Controller Kit's device monitor and the five axis monitors. False when the
// service refuses (no gamepad service on this device, or it is already taken by another caller), in
// which case the caller keeps using the multimodal input events.
bool GamepadKitStart(GamepadKitCallback callback);
void GamepadKitStop();

// Online controllers, for the launch mask and for naming them in the UI.
std::vector<GamepadKitDevice> GamepadKitDevices();

// True once the axis monitors are registered.
bool GamepadKitActive();

}

#endif
