import { inputDevice } from '@kit.InputKit';
import { TouchpadTapFilter, TouchpadTapTarget } from '../TouchpadTapFilter';
import { InputSink, PadProbeHandler, TouchpadContact, TouchPhase } from './InputSink';
import { TOUCHPAD_PAN_MM_PER_AXIS_PX } from '../TouchpadWheel';
import { clamp01, TOUCHPAD_DOUBLE_TAP_MS, TOUCHPAD_HEIGHT_MM, TOUCHPAD_MOVE_SLOP,
  TOUCHPAD_PAN_CENTER, TOUCHPAD_PAN_END_GRACE_MS, TOUCHPAD_PAN_FINGER_MM, TOUCHPAD_PAN_ID_A,
  TOUCHPAD_PAN_ID_B, TOUCHPAD_PAN_SAFE_HALF, TOUCHPAD_PRESS_HOLD_MS, TOUCHPAD_WIDTH_MM
  } from './InputPolicy';

// Touchpad identity and the host precision-touchpad channel: device detection/cache, the
// double-tap -> middle click filter, and the synthesized two-finger pan frames.
export class TouchpadInput {
  private sink: InputSink;
  private tap: TouchpadTapFilter;
  private devices: Map<number, boolean> = new Map<number, boolean>();
  private middleDoubleTap: boolean = false;
  private touchpadPan: boolean = false;
  private panActive: boolean = false;
  private panX: number = TOUCHPAD_PAN_CENTER;
  private panY: number = TOUCHPAD_PAN_CENTER;
  private panEndTimer: number = -1;
  private probe: PadProbeHandler | undefined = undefined;
  private deviceChangeListener: (data: inputDevice.DeviceListener) => void =
    (data: inputDevice.DeviceListener) => {
      // Device IDs can change on replug, so drop the touchpad cache on a change.
      this.devices.clear();
    };

  constructor(sink: InputSink, buttonTarget: TouchpadTapTarget) {
    this.sink = sink;
    this.tap = new TouchpadTapFilter(buttonTarget, TOUCHPAD_DOUBLE_TAP_MS,
      TOUCHPAD_PRESS_HOLD_MS, TOUCHPAD_MOVE_SLOP);
    try {
      inputDevice.on('change', this.deviceChangeListener);
    } catch (error) {
    }
  }

  setProbe(probe: PadProbeHandler): void {
    this.probe = probe;
  }

  setMiddleDoubleTap(enabled: boolean): void {
    this.middleDoubleTap = enabled;
    if (!enabled) {
      this.tap.reset();
    }
  }

  setPanEnabled(enabled: boolean): void {
    this.touchpadPan = enabled;
    if (!enabled) {
      this.endPan();
    }
  }

  isPanEnabled(): boolean {
    return this.touchpadPan;
  }

  resetTap(): void {
    this.tap.reset();
  }

  dispose(): void {
    try {
      inputDevice.off('change', this.deviceChangeListener);
    } catch (error) {
    }
    this.endPan();
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

  // Sign pair verified on device; flip both together if a host scrolls the wrong way.
  handlePan(vertical: number, horizontal: number): void {
    this.cancelPanEnd();
    if (!this.panActive) {
      this.panX = TOUCHPAD_PAN_CENTER;
      this.panY = TOUCHPAD_PAN_CENTER;
      this.panActive = true;
      this.sendPanFrame(TouchPhase.DOWN);
    }
    const nextX: number = this.panX - horizontal * TOUCHPAD_PAN_MM_PER_AXIS_PX / TOUCHPAD_WIDTH_MM;
    const nextY: number = this.panY - vertical * TOUCHPAD_PAN_MM_PER_AXIS_PX / TOUCHPAD_HEIGHT_MM;
    if (Math.abs(nextX - TOUCHPAD_PAN_CENTER) > TOUCHPAD_PAN_SAFE_HALF
      || Math.abs(nextY - TOUCHPAD_PAN_CENTER) > TOUCHPAD_PAN_SAFE_HALF) {
      this.retouchPad();
      return;
    }
    this.panX = nextX;
    this.panY = nextY;
    this.sendPanFrame(TouchPhase.MOVE);
  }

  // A flapping begin/end stream must not lift the fingers between events, so the lift waits out a
  // short grace the next event cancels.
  schedulePanEnd(): void {
    if (!this.panActive || this.panEndTimer !== -1) {
      return;
    }
    this.panEndTimer = setTimeout(() => {
      this.panEndTimer = -1;
      this.endPan();
    }, TOUCHPAD_PAN_END_GRACE_MS);
  }

  cancelPanEnd(): void {
    if (this.panEndTimer !== -1) {
      clearTimeout(this.panEndTimer);
      this.panEndTimer = -1;
    }
  }

  endPan(): void {
    this.cancelPanEnd();
    if (!this.panActive) {
      return;
    }
    this.panActive = false;
    this.sendPanFrame(TouchPhase.UP);
    this.probeContacts(false, 0, 0, 0, 0);
  }

  private retouchPad(): void {
    // Lift and re-touch at the middle: the declared surface ran out, so the seam carries no motion.
    this.sendPanFrame(TouchPhase.UP);
    this.panX = TOUCHPAD_PAN_CENTER;
    this.panY = TOUCHPAD_PAN_CENTER;
    this.sendPanFrame(TouchPhase.DOWN);
  }

  private sendPanFrame(phase: number): void {
    const halfX: number = TOUCHPAD_PAN_FINGER_MM / 2 / TOUCHPAD_WIDTH_MM;
    const x1: number = clamp01(this.panX - halfX);
    const x2: number = clamp01(this.panX + halfX);
    const contacts: TouchpadContact[] = [
      TouchpadContact.of(phase, TOUCHPAD_PAN_ID_A, x1, this.panY, 0),
      TouchpadContact.of(phase, TOUCHPAD_PAN_ID_B, x2, this.panY, 0)
    ];
    this.sink.touchpadFrame(contacts, 0, TOUCHPAD_WIDTH_MM, TOUCHPAD_HEIGHT_MM);
    // The dev overlay draws whatever contact pair the channel just synthesized, pan included.
    this.probeContacts(phase !== TouchPhase.UP, x1, this.panY, x2, this.panY);
  }

  private probeContacts(active: boolean, x1: number, y1: number, x2: number, y2: number): void {
    const probe: PadProbeHandler | undefined = this.probe;
    if (probe !== undefined) {
      probe(active, x1, y1, x2, y2);
    }
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
