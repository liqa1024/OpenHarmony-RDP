import { inputDevice } from '@kit.InputKit';
import { TouchpadTapFilter, TouchpadTapTarget } from '../TouchpadTapFilter';
import { TOUCHPAD_DOUBLE_TAP_MS, TOUCHPAD_MOVE_SLOP, TOUCHPAD_PRESS_HOLD_MS } from './InputPolicy';

// Touchpad identity: device detection/cache and the double-tap -> middle click filter.
export class TouchpadInput {
  private tap: TouchpadTapFilter;
  private devices: Map<number, boolean> = new Map<number, boolean>();
  private middleDoubleTap: boolean = false;
  private deviceChangeListener: (data: inputDevice.DeviceListener) => void =
    (data: inputDevice.DeviceListener) => {
      // Device IDs can change on replug, so drop the touchpad cache on a change.
      this.devices.clear();
    };

  constructor(buttonTarget: TouchpadTapTarget) {
    this.tap = new TouchpadTapFilter(buttonTarget, TOUCHPAD_DOUBLE_TAP_MS,
      TOUCHPAD_PRESS_HOLD_MS, TOUCHPAD_MOVE_SLOP);
    try {
      inputDevice.on('change', this.deviceChangeListener);
    } catch (error) {
    }
  }

  setMiddleDoubleTap(enabled: boolean): void {
    this.middleDoubleTap = enabled;
    if (!enabled) {
      this.tap.reset();
    }
  }

  resetTap(): void {
    this.tap.reset();
  }

  dispose(): void {
    try {
      inputDevice.off('change', this.deviceChangeListener);
    } catch (error) {
    }
    this.tap.reset();
  }

  isTouchpad(deviceId: number | undefined, sourceTool: SourceTool | undefined): boolean {
    return sourceTool === SourceTool.TOUCHPAD || this.isTouchpadDevice(deviceId);
  }

  // The double-tap filter owns the touchpad right click and observes its move/cancel; every
  // other pointer event falls through. Returns true when the event was consumed.
  consumeMouse(event: MouseEvent, point: number[]): boolean {
    if (!this.middleDoubleTap || !this.isTouchpad(event.deviceId, event.sourceTool)) {
      return false;
    }
    if (event.action === MouseAction.Move) {
      this.tap.observeMove(point[0], point[1]);
    } else if (event.action === MouseAction.CANCEL) {
      this.tap.cancel(point[0], point[1]);
    } else if (event.button === MouseButton.Right) {
      if (event.action === MouseAction.Press) {
        this.tap.press(point[0], point[1]);
        return true;
      }
      if (event.action === MouseAction.Release) {
        this.tap.release(point[0], point[1]);
        return true;
      }
    }
    return false;
  }

  private isTouchpadDevice(deviceId: number | undefined): boolean {
    if (deviceId === undefined || deviceId <= 0) {
      return false;
    }
    const cached: boolean | undefined = this.devices.get(deviceId);
    if (cached !== undefined) {
      return cached;
    }
    try {
      const info: inputDevice.InputDeviceData = inputDevice.getDeviceInfoSync(deviceId);
      const touchpad: boolean = info.sources.indexOf('touchpad') >= 0;
      this.devices.set(deviceId, touchpad);
      return touchpad;
    } catch (error) {
    }
    return false;
  }
}
