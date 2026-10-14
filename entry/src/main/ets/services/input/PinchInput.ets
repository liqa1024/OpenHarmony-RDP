import { KeyMapping } from '../../utils/KeyMapper';
import { DEFAULT_PINCH_TOUCH_DISTANCE, PINCH_MODE_NATIVE, PINCH_MODE_TOUCH, PINCH_MODE_WHEEL,
  PINCH_MODE_ZOOM } from '../SettingsStore';
import { DEFAULT_PINCH_NOTCHES_PER_UNIT, DEFAULT_SPEED_SCALE, MAX_SPEED_SCALE, MIN_SPEED_SCALE,
  WHEEL_DETENT } from '../TouchpadWheel';
import { InputSink, PinchCursorHandler, PadProbeHandler, TouchpadContact, TouchPhase } from './InputSink';
import { clamp01, InputGeometry, isPinchEvent, MAX_PINCH_DISTANCE_PERCENT, MAX_PINCH_RATIO,
  MIN_PINCH_DISTANCE_PERCENT, MIN_PINCH_FRAC, MIN_PINCH_HALF, MIN_PINCH_RATIO,
  NATIVE_PINCH_ANGLE_DEG, NATIVE_PINCH_BASE_PERCENT, PINCH_CURSOR_HYSTERESIS, PINCH_TOUCH_DOWN_FLUSH_MS,
  PINCH_TOUCH_ID_A, PINCH_TOUCH_ID_B, pinchScaleValue, TOUCHPAD_HEIGHT_MM, TOUCHPAD_WIDTH_MM
  } from './InputPolicy';

const PINCH_CTRL: KeyMapping = KeyMapping.of(0x1D, 0xA2, false, 0);

// Touchpad pinch, three modes: Ctrl+wheel (remote app zooms), synthesized two-finger touch, or the
// native precision-touchpad contact frame. No gesture-level locking — only real END ends it.
export class PinchInput {
  private sink: InputSink;
  private geometry: InputGeometry;
  private pinchMode: number = PINCH_MODE_WHEEL;
  private pinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  private pinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  private pinchTouchAngle: number = 30;
  private pinchActive: boolean = false;
  private pinchScale: number = 0;
  private pinchAccum: number = 0;
  private pinchCtrlDown: boolean = false;
  private touchPinchActive: boolean = false;
  private touchPinchZoomIn: boolean = true;
  private touchPinchAnchor: number[] = [0, 0];
  private touchPinchHalf: number = MIN_PINCH_HALF;
  private touchPinchHalfMin: number = MIN_PINCH_HALF;
  private touchPinchLastHalf: number = MIN_PINCH_HALF;
  private touchPinchMotionAt: number = 0;
  private touchPinchUnitX: number = 1;
  private touchPinchUnitY: number = 0;
  private cursorHandler: PinchCursorHandler | undefined = undefined;
  private probe: PadProbeHandler | undefined = undefined;

  constructor(sink: InputSink, geometry: InputGeometry) {
    this.sink = sink;
    this.geometry = geometry;
  }

  setPinchMode(mode: number): void {
    this.pinchMode = mode;
  }

  setPinchZoomSpeed(speed: number): void {
    this.pinchZoomSpeed = Math.max(MIN_SPEED_SCALE, Math.min(MAX_SPEED_SCALE, speed));
  }

  setPinchTouchDistance(percent: number): void {
    this.pinchTouchDistance = percent;
  }

  setPinchTouchAngle(degrees: number): void {
    this.pinchTouchAngle = degrees;
  }

  setCursorHandler(handler: PinchCursorHandler): void {
    this.cursorHandler = handler;
  }

  setProbe(probe: PadProbeHandler): void {
    this.probe = probe;
  }

  isActive(): boolean {
    return this.pinchActive;
  }

  // Called once the facade has established the gesture is a pinch (fresh) or a live pinch.
  handleAxis(event: AxisEvent): void {
    if (event.action === AxisAction.END) {
      this.end();
      return;
    }
    if (isPinchEvent(event)) {
      this.update(event);
    }
  }

  end(): void {
    this.endTouchPinch();
    this.pinchActive = false;
    this.pinchScale = 0;
    this.pinchAccum = 0;
    this.touchPinchMotionAt = 0;
    this.setPinchCtrl(false);
  }

  private update(event: AxisEvent): void {
    const scale: number = pinchScaleValue(event);
    if (!this.pinchActive) {
      this.pinchActive = true;
      this.pinchAccum = 0;
      if (this.useTouchPinch()) {
        this.beginTouchPinch(event, scale);
      } else if (this.pinchMode !== PINCH_MODE_ZOOM) {
        this.pinchScale = scale > 0 ? scale : 0;
        this.setPinchCtrl(true);
      }
      return;
    }
    if (scale <= 0) {
      return;
    }
    if (this.useTouchPinch()) {
      this.updateTouchPinch(scale);
      if (Date.now() >= this.touchPinchMotionAt) {
        this.sendTouchPinchContacts(TouchPhase.MOVE);
      }
      return;
    }
    if (this.pinchMode === PINCH_MODE_ZOOM) {
      return;
    }
    if (this.pinchScale <= 0) {
      this.pinchScale = scale;
      return;
    }
    this.pinchAccum += scale - this.pinchScale;
    this.pinchScale = scale;
    const notchesPerUnit: number = DEFAULT_PINCH_NOTCHES_PER_UNIT * this.pinchZoomSpeed;
    const step: number = notchesPerUnit > 0 ? 1 / notchesPerUnit : 0;
    if (step <= 0) {
      return;
    }
    const point: number[] = this.geometry.map(event.x, event.y);
    while (this.pinchAccum >= step) {
      this.pinchAccum -= step;
      this.sink.scroll(WHEEL_DETENT, 0, point[0], point[1]);
    }
    while (this.pinchAccum <= -step) {
      this.pinchAccum += step;
      this.sink.scroll(-WHEEL_DETENT, 0, point[0], point[1]);
    }
  }

  private beginTouchPinch(event: AxisEvent, scale: number): void {
    let percent: number;
    let angleDeg: number;
    let surface: number;
    if (this.pinchMode === PINCH_MODE_NATIVE) {
      this.touchPinchAnchor = [0.5, 0.5];
      this.touchPinchHalfMin = MIN_PINCH_FRAC;
      surface = 1;
      percent = NATIVE_PINCH_BASE_PERCENT;
      angleDeg = NATIVE_PINCH_ANGLE_DEG;
    } else {
      this.touchPinchAnchor = this.geometry.map(event.x, event.y);
      this.touchPinchHalfMin = MIN_PINCH_HALF;
      surface = this.geometry.minDim();
      percent = Math.max(MIN_PINCH_DISTANCE_PERCENT,
        Math.min(MAX_PINCH_DISTANCE_PERCENT, this.pinchTouchDistance));
      angleDeg = this.pinchTouchAngle;
    }
    this.touchPinchHalf = Math.max(this.touchPinchHalfMin, surface * percent / 100);
    this.touchPinchLastHalf = this.touchPinchHalf;
    const radians: number = Math.max(-180, Math.min(180, angleDeg)) * Math.PI / 180;
    this.touchPinchUnitX = Math.cos(radians);
    this.touchPinchUnitY = Math.sin(radians);
    this.touchPinchActive = true;
    this.touchPinchZoomIn = true;
    this.updateTouchPinch(scale);
    this.notifyCursor(true, this.touchPinchZoomIn);
    this.sendTouchPinchContacts(TouchPhase.DOWN);
    this.touchPinchMotionAt = Date.now() + PINCH_TOUCH_DOWN_FLUSH_MS;
  }

  private updateTouchPinch(scale: number): void {
    const ratio: number = Math.max(MIN_PINCH_RATIO, Math.min(MAX_PINCH_RATIO, scale));
    this.touchPinchLastHalf =
      Math.max(this.touchPinchHalfMin, this.touchPinchHalf * ratio);
    const zoomIn: boolean = ratio > 1 + PINCH_CURSOR_HYSTERESIS
      ? true : (ratio < 1 - PINCH_CURSOR_HYSTERESIS ? false : this.touchPinchZoomIn);
    if (zoomIn !== this.touchPinchZoomIn) {
      this.touchPinchZoomIn = zoomIn;
      this.notifyCursor(true, zoomIn);
    }
  }

  private sendTouchPinchContacts(phase: number): void {
    const point: number[] = this.touchPinchAnchor;
    const half: number = this.touchPinchLastHalf;
    const dx: number = half * this.touchPinchUnitX;
    const dy: number = half * this.touchPinchUnitY;
    if (this.pinchMode === PINCH_MODE_NATIVE) {
      const x1: number = clamp01(point[0] + dx);
      const y1: number = clamp01(point[1] + dy);
      const x2: number = clamp01(point[0] - dx);
      const y2: number = clamp01(point[1] - dy);
      const contacts: TouchpadContact[] = [
        TouchpadContact.of(phase, PINCH_TOUCH_ID_A, x1, y1, 0),
        TouchpadContact.of(phase, PINCH_TOUCH_ID_B, x2, y2, 0)
      ];
      this.sink.touchpadFrame(contacts, 0, TOUCHPAD_WIDTH_MM, TOUCHPAD_HEIGHT_MM);
      this.probeContacts(true, x1, y1, x2, y2);
      return;
    }
    const maxX: number = Math.max(0, this.geometry.pointerWidth() - 1);
    const maxY: number = Math.max(0, this.geometry.pointerHeight() - 1);
    const x1: number = Math.max(0, Math.min(maxX, Math.round(point[0] + dx)));
    const y1: number = Math.max(0, Math.min(maxY, Math.round(point[1] + dy)));
    const x2: number = Math.max(0, Math.min(maxX, Math.round(point[0] - dx)));
    const y2: number = Math.max(0, Math.min(maxY, Math.round(point[1] - dy)));
    this.sink.touch(phase, PINCH_TOUCH_ID_A, x1, y1, 0);
    this.sink.touch(phase, PINCH_TOUCH_ID_B, x2, y2, 0);
    const pw: number = this.geometry.pointerWidth() > 0 ? this.geometry.pointerWidth() : 1;
    const ph: number = this.geometry.pointerHeight() > 0 ? this.geometry.pointerHeight() : 1;
    this.probeContacts(true, x1 / pw, y1 / ph, x2 / pw, y2 / ph);
  }

  private useTouchPinch(): boolean {
    return this.pinchMode === PINCH_MODE_TOUCH || this.pinchMode === PINCH_MODE_NATIVE;
  }

  private endTouchPinch(): void {
    if (!this.touchPinchActive) {
      return;
    }
    this.touchPinchActive = false;
    this.sendTouchPinchContacts(TouchPhase.UP);
    this.probeContacts(false, 0, 0, 0, 0);
    this.notifyCursor(false, this.touchPinchZoomIn);
  }

  private setPinchCtrl(down: boolean): void {
    if (this.pinchCtrlDown === down) {
      return;
    }
    this.pinchCtrlDown = down;
    this.sink.key(PINCH_CTRL, down);
  }

  private notifyCursor(active: boolean, zoomIn: boolean): void {
    const handler: PinchCursorHandler | undefined = this.cursorHandler;
    if (handler !== undefined && this.pinchMode === PINCH_MODE_TOUCH) {
      handler(active, zoomIn);
    }
  }

  private probeContacts(active: boolean, x1: number, y1: number, x2: number, y2: number): void {
    const probe: PadProbeHandler | undefined = this.probe;
    if (probe !== undefined) {
      probe(active, x1, y1, x2, y2);
    }
  }
}
