import { inputDevice } from '@kit.InputKit';
import { TouchpadTapFilter, TouchpadTapTarget } from '../TouchpadTapFilter';
import { InputSink, PadProbeHandler, TouchpadContact, TouchPhase } from './InputSink';
import { TOUCHPAD_PAN_MM_PER_AXIS_PX } from '../TouchpadWheel';
import { clamp01, TOUCHPAD_DOUBLE_TAP_MS, TOUCHPAD_HEIGHT_MM, TOUCHPAD_MOVE_ID,
  TOUCHPAD_MOVE_MM_PER_COUNT, TOUCHPAD_MOVE_SLOP, TOUCHPAD_PAN_CENTER, TOUCHPAD_PAN_END_GRACE_MS,
  TOUCHPAD_PAN_FINGER_MM, TOUCHPAD_PAN_ID_A, TOUCHPAD_PAN_ID_B, TOUCHPAD_PAN_SAFE_HALF,
  TOUCHPAD_PRESS_HOLD_MS, TOUCHPAD_WIDTH_MM } from './InputPolicy';

// Touchpad identity and the host precision-touchpad channel: device detection/cache, the
// double-tap -> middle click filter, and the synthesized contact frames (one contact for
// relative-mode pointer motion, a pair for scroll panning).
export class TouchpadInput {
  private sink: InputSink;
  private tap: TouchpadTapFilter;
  private devices: Map<number, boolean> = new Map<number, boolean>();
  private middleDoubleTap: boolean = false;
  private touchpadPan: boolean = false;
  private moveEnabled: boolean = false;
  private padActive: boolean = false;
  private padPair: boolean = false;
  private padX: number = TOUCHPAD_PAN_CENTER;
  private padY: number = TOUCHPAD_PAN_CENTER;
  private padEndTimer: number = -1;
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
      this.endPad();
    }
  }

  isPanEnabled(): boolean {
    return this.touchpadPan;
  }

  // Enabled only when the host advertises the pad channel; otherwise relative-mode touchpad motion
  // falls back to the sink's relative-mouse path.
  setMoveEnabled(enabled: boolean): void {
    this.moveEnabled = enabled;
    if (!enabled) {
      this.endPad();
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
    this.endPad();
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

  // Relative-mode touchpad motion as one contact on the declared pad. False (not consumed) means
  // there is no pad channel, so the caller sends the motion as a relative mouse move instead.
  consumeRelativeMove(event: MouseEvent): boolean {
    if (!this.moveEnabled || event.action !== MouseAction.Move
      || !this.isTouchpad(event.deviceId, event.sourceTool)) {
      return false;
    }
    if (this.padActive && this.padPair) {
      // A multi-finger gesture owns the stream, so a single-finger frame in between is dropped.
      return true;
    }
    const dx: number = event.rawDeltaX ?? 0;
    const dy: number = event.rawDeltaY ?? 0;
    if (dx === 0 && dy === 0) {
      return true;
    }
    this.cancelPadEnd();
    if (!this.padActive) {
      this.beginPad(false);
    }
    const nextX: number = this.padX + dx * TOUCHPAD_MOVE_MM_PER_COUNT / TOUCHPAD_WIDTH_MM;
    const nextY: number = this.padY + dy * TOUCHPAD_MOVE_MM_PER_COUNT / TOUCHPAD_HEIGHT_MM;
    if (!this.insideSafeArea(nextX, nextY)) {
      this.retouchPad();
      return true;
    }
    this.padX = nextX;
    this.padY = nextY;
    this.sendPadFrame(TouchPhase.MOVE);
    this.schedulePadEnd();
    return true;
  }

  // Sign pair verified on device; flip both together if a host scrolls the wrong way.
  handlePan(vertical: number, horizontal: number): void {
    this.cancelPadEnd();
    // Multi-finger wins over the single-finger pointer stream.
    if (this.padActive && !this.padPair) {
      this.endPad();
    }
    if (!this.padActive) {
      this.beginPad(true);
    }
    const nextX: number = this.padX - horizontal * TOUCHPAD_PAN_MM_PER_AXIS_PX / TOUCHPAD_WIDTH_MM;
    const nextY: number = this.padY - vertical * TOUCHPAD_PAN_MM_PER_AXIS_PX / TOUCHPAD_HEIGHT_MM;
    if (!this.insideSafeArea(nextX, nextY)) {
      this.retouchPad();
      return;
    }
    this.padX = nextX;
    this.padY = nextY;
    this.sendPadFrame(TouchPhase.MOVE);
  }

  // A flapping begin/end stream must not lift the fingers between frames, so the lift waits out a
  // short grace the next frame cancels.
  schedulePadEnd(): void {
    if (!this.padActive || this.padEndTimer !== -1) {
      return;
    }
    this.padEndTimer = setTimeout(() => {
      this.padEndTimer = -1;
      this.endPad();
    }, TOUCHPAD_PAN_END_GRACE_MS);
  }

  cancelPadEnd(): void {
    if (this.padEndTimer !== -1) {
      clearTimeout(this.padEndTimer);
      this.padEndTimer = -1;
    }
  }

  endPad(): void {
    this.cancelPadEnd();
    if (!this.padActive) {
      return;
    }
    this.padActive = false;
    this.sendPadFrame(TouchPhase.UP);
    this.probeContacts(false, 0, 0, 0, 0, 0);
  }

  private beginPad(pair: boolean): void {
    this.padX = TOUCHPAD_PAN_CENTER;
    this.padY = TOUCHPAD_PAN_CENTER;
    this.padActive = true;
    this.padPair = pair;
    this.sendPadFrame(TouchPhase.DOWN);
  }

  private retouchPad(): void {
    // Lift and re-touch at the middle: the declared surface ran out, so the seam carries no motion.
    this.sendPadFrame(TouchPhase.UP);
    this.padX = TOUCHPAD_PAN_CENTER;
    this.padY = TOUCHPAD_PAN_CENTER;
    this.sendPadFrame(TouchPhase.DOWN);
    this.schedulePadEnd();
  }

  private insideSafeArea(x: number, y: number): boolean {
    return Math.abs(x - TOUCHPAD_PAN_CENTER) <= TOUCHPAD_PAN_SAFE_HALF
      && Math.abs(y - TOUCHPAD_PAN_CENTER) <= TOUCHPAD_PAN_SAFE_HALF;
  }

  private sendPadFrame(phase: number): void {
    const contacts: TouchpadContact[] = [];
    if (this.padPair) {
      const halfX: number = TOUCHPAD_PAN_FINGER_MM / 2 / TOUCHPAD_WIDTH_MM;
      contacts.push(TouchpadContact.of(phase, TOUCHPAD_PAN_ID_A,
        clamp01(this.padX - halfX), this.padY, 0));
      contacts.push(TouchpadContact.of(phase, TOUCHPAD_PAN_ID_B,
        clamp01(this.padX + halfX), this.padY, 0));
    } else {
      contacts.push(TouchpadContact.of(phase, TOUCHPAD_MOVE_ID,
        clamp01(this.padX), clamp01(this.padY), 0));
    }
    this.sink.touchpadFrame(contacts, 0, TOUCHPAD_WIDTH_MM, TOUCHPAD_HEIGHT_MM);
    const paired: boolean = contacts.length > 1;
    this.probeContacts(phase !== TouchPhase.UP, contacts.length,
      contacts[0].x, contacts[0].y,
      paired ? contacts[1].x : contacts[0].x,
      paired ? contacts[1].y : contacts[0].y);
  }

  private probeContacts(active: boolean, count: number, x1: number, y1: number, x2: number,
    y2: number): void {
    const probe: PadProbeHandler | undefined = this.probe;
    if (probe !== undefined) {
      probe(active, count, x1, y1, x2, y2);
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
