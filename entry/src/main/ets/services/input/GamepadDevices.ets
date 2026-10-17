import { inputDevice } from '@kit.InputKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { GamepadKitDevice, gamepadKitDevices } from 'libhmrdp.so';

// Controller slots, i.e. the width of activeGamepadMask the host is asked to honour (GFE's cap).
export const MAX_PADS: number = 4;

// Connected controllers.
export class GamepadDevices {
  private static ids: number[] = [];

  // One bit per attached controller, in attachment order; 0 when none is attached. The host takes it
  // as the set of controllers to create up front (hot-plug updates it at run time).
  static async attachedMask(): Promise<number> {
    const kit: GamepadKitDevice[] = gamepadKitDevices();
    if (kit.length > 0) {
      return GamepadDevices.maskFor(Math.min(kit.length, MAX_PADS));
    }
    let count: number = 0;
    try {
      const ids: number[] = await inputDevice.getDeviceList();
      for (let i = 0; i < ids.length && count < MAX_PADS; i++) {
        if (GamepadDevices.isJoystick(ids[i])) {
          count++;
        }
      }
    } catch (error) {
      return 0;
    }
    return GamepadDevices.maskFor(count);
  }

  // Cache of the multimodal input ids of the joystick-class devices; refreshed on hot-plug.
  static refresh(): void {
    inputDevice.getDeviceList((error: BusinessError, ids: number[]): void => {
      if (error !== undefined && error !== null) {
        return;
      }
      const joysticks: number[] = [];
      for (let i = 0; i < ids.length; i++) {
        if (GamepadDevices.isJoystick(ids[i])) {
          joysticks.push(ids[i]);
        }
      }
      GamepadDevices.ids = joysticks;
    });
  }

  // Ties the Kit's own device key to the multimodal input device the key events come from: the key
  // itself when it is the id in text form, otherwise the device name, otherwise the single
  // candidate. -1 when it cannot be settled.
  static resolveDeviceId(key: string, name: string): number {
    const ids: number[] = GamepadDevices.ids;
    const numeric: number = Number.parseInt(key, 10);
    if (!Number.isNaN(numeric) && GamepadDevices.isJoystick(numeric)) {
      return numeric;
    }
    if (name.length > 0) {
      for (let i = 0; i < ids.length; i++) {
        if (GamepadDevices.deviceName(ids[i]) === name) {
          return ids[i];
        }
      }
    }
    return ids.length === 1 ? ids[0] : -1;
  }

  static isJoystick(deviceId: number): boolean {
    if (deviceId <= 0) {
      return false;
    }
    try {
      const info: inputDevice.InputDeviceData = inputDevice.getDeviceInfoSync(deviceId);
      return info.sources.indexOf('joystick') >= 0;
    } catch (error) {
    }
    return false;
  }

  private static deviceName(deviceId: number): string {
    try {
      return inputDevice.getDeviceInfoSync(deviceId).name;
    } catch (error) {
    }
    return '';
  }

  private static maskFor(count: number): number {
    let mask: number = 0;
    for (let i = 0; i < count; i++) {
      mask |= (1 << i);
    }
    return mask;
  }
}
