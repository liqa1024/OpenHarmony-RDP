import { inputDevice } from '@kit.InputKit';
import { KeyMapping, KeyMapper } from '../utils/KeyMapper';
import { RdpLockFlags } from '../model/RdpModels';
import { DEFAULT_PINCH_NOTCHES_PER_UNIT, DEFAULT_SPEED_SCALE, FLING_FRICTION, FLING_FRICTION_SCALE,
  FLING_MIN_VELOCITY, FLING_VELOCITY_SCALE, MAX_SPEED_SCALE, MIN_SPEED_SCALE, TouchpadWheelMapper,
  WheelChunk, WHEEL_DETENT, WHEEL_VP_PER_DEGREE } from './TouchpadWheel';
import { DEFAULT_PINCH_TOUCH_DISTANCE, PINCH_MODE_NATIVE, PINCH_MODE_TOUCH, PINCH_MODE_WHEEL,
  PINCH_MODE_ZOOM } from './SettingsStore';
import { TouchpadTapFilter, TouchpadTapTarget } from './TouchpadTapFilter';
import { defaultDisplay } from '../utils/DisplayUtil';

export class InputButton {
  static readonly LEFT: number = 1;
  static readonly MIDDLE: number = 2;
  static readonly RIGHT: number = 3;
  static readonly X1: number = 4;
  static readonly X2: number = 5;
}

export class InputAction {
  static readonly PRESS: number = 1;
  static readonly RELEASE: number = 2;
}

export class TouchPhase {
  static readonly DOWN: number = 1;
  static readonly UP: number = 2;
  static readonly MOVE: number = 3;
  static readonly CANCEL: number = 4;
}

// One contact of a native precision-touchpad frame; x/y normalized over the
// physical surface. HarmonyOS exposes no touchpad contacts, so these are synthesized.
export class TouchpadContact {
  phase: number = TouchPhase.MOVE;
  pointerId: number = 0;
  x: number = 0;
  y: number = 0;
  pressure: number = 0;

  static of(phase: number, pointerId: number, x: number, y: number,
    pressure: number): TouchpadContact {
    const contact: TouchpadContact = new TouchpadContact();
    contact.phase = phase;
    contact.pointerId = pointerId;
    contact.x = x;
    contact.y = y;
    contact.pressure = pressure;
    return contact;
  }
}

export interface InputSink {
  key(mapping: KeyMapping, down: boolean): void;
  metaKey(down: boolean): void;
  unicode(code: number, down: boolean): void;
  lockState(flags: number): void;
  pointerMove(x: number, y: number): void;
  button(action: number, button: number, x: number, y: number): void;
  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): void;
  // Native precision-touchpad frame (foundation-sunshine extension): normalized
  // contacts over the physical surface (empty = all lifted); the frame carries the
  // pad's physical size in millimetres so the host can inject a real PT_TOUCHPAD.
  touchpadFrame(contacts: TouchpadContact[], buttonState: number, widthMm: number,
    heightMm: number): void;
  // Wheel deltas in WHEEL_DELTA (120) units; vertical positive is up, horizontal positive is left.
  // x/y are the pointer position in the pointer space (protocols that carry it on the wheel event).
  scroll(vertical: number, horizontal: number, x: number, y: number): void;
}

// Each forwarded move is a separate input message, so absolute moves are rate-limited.
const MOUSE_MOVE_MIN_INTERVAL_MS: number = 8;

// A TouchType.Cancel does not always mean the finger lifted: a Down within this distance of its last position resumes the contact instead of lifting and re-pressing.
const CANCEL_RESUME_RATIO: number = 0.02;
const CANCEL_RESUME_MIN_DISTANCE: number = 16;
const CANCEL_RESUME_MAX_DISTANCE: number = 64;
const CANCEL_HOLD_MS: number = 400;

// 触控板双指双击 -> 中键：从第一次轻触的按下起算的双击窗口。
const TOUCHPAD_DOUBLE_TAP_MS: number = 300;
// 轻触与实体右键的分界；系统双指轻触的按下-抬起远小于此值。
const TOUCHPAD_PRESS_HOLD_MS: number = 30;
// 判定为拖动的最小位移（远端指针空间 px）。
const TOUCHPAD_MOVE_SLOP: number = 8;

// Touchpad fling: estimate velocity over a short sample window, then emit extra wheel rotation
// with the ArkUI decay. The window/tick are estimator parameters; distance/decay are native's.
const AXIS_VELOCITY_WINDOW_MS: number = 100;
const FLING_TICK_MS: number = 16;
const FLING_STOP_PX: number = 0.5;
const FLING_MAX_SECONDS: number = 3;

// One touchpad-axis sample (cumulative travel vs time) for the fling velocity estimate.
class AxisSample {
  t: number = 0;
  v: number = 0;
  h: number = 0;

  constructor(t: number, v: number, h: number) {
    this.t = t;
    this.v = v;
    this.h = h;
  }
}

const MIN_PINCH_DISTANCE_PERCENT: number = 1;
const MAX_PINCH_DISTANCE_PERCENT: number = 20;
const MIN_PINCH_HALF: number = 8;
// Native-touchpad contact separation floor, as a fraction of the pad surface.
const MIN_PINCH_FRAC: number = 0.02;
// Native-touchpad base half-separation, as a percent of the pad width (so the initial
// separation is ~2.5x the shared touch-mode default). Fixed: it is a property of the pad.
const NATIVE_PINCH_BASE_PERCENT: number = 20;
// The native pair lies along the pad diagonal (45 deg in the normalized pad square, which
// maps to the physical corner-to-corner direction), maximizing the usable separation length.
const NATIVE_PINCH_ANGLE_DEG: number = 45;
// Stand-in physical pad size for a synthesized precision touchpad (no device value on HarmonyOS).
const TOUCHPAD_WIDTH_MM: number = 100;
const TOUCHPAD_HEIGHT_MM: number = 70;
// axisPinch is a distance ratio relative to the gesture start; clamp it so a bad sample cannot explode or collapse the separation.
const MIN_PINCH_RATIO: number = 0.05;
const MAX_PINCH_RATIO: number = 20;
// External touch ids for the synthetic pinch contacts; screen touches use id + 1, so these never collide.
const PINCH_TOUCH_ID_A: number = 20;
const PINCH_TOUCH_ID_B: number = 21;
// FreeRDP coalesces touch frames, so the first MOTION must not reach RDPEI before its DOWN flush or it overwrites the unsent DOWN.
const PINCH_TOUCH_DOWN_FLUSH_MS: number = 30;
// Dead zone before the pinch locator cursor flips open/grab hand, so jitter does not flicker it.
const PINCH_CURSOR_HYSTERESIS: number = 0.02;

const PINCH_CTRL: KeyMapping = KeyMapping.of(0x1D, 0xA2, false, 0);

export type PointMapper = (x: number, y: number) => number[];
export type PinchCursorHandler = (active: boolean, zoomIn: boolean) => void;
// Debug probe: reports the synthesized pinch contacts, normalized to the channel surface
// (remote screen for the touch channel, pad surface for the native-touchpad channel).
export type PinchProbeHandler =
  (active: boolean, p1x: number, p1y: number, p2x: number, p2y: number) => void;

export class SessionInput implements TouchpadTapTarget {
  private sink: InputSink;
  private winKeySubstitute: number = 0;
  private interceptActive: boolean = false;
  private heldKeys: Map<string, KeyMapping> = new Map<string, KeyMapping>();
  private heldUnicode: Set<number> = new Set<number>();
  private heldMeta: boolean = false;
  private heldButtons: Set<number> = new Set<number>();
  private lastPointerX: number = 0;
  private lastPointerY: number = 0;
  private pendingMoveX: number = -1;
  private pendingMoveY: number = -1;
  private lastMoveSentAt: number = 0;
  private moveTimer: number = -1;
  // Pointer space in remote desktop/stream pixels, used for touch clamping and cancel resume.
  private pointerWidth: number = 0;
  private pointerHeight: number = 0;
  private pointMapper: PointMapper = (x: number, y: number): number[] => [x, y];
  private touchEnabled: boolean = false;
  private touchPoints: Map<number, number[]> = new Map<number, number[]>();
  // Kept separate from touchPoints so a contact can be resumed after a Cancel even if the UI renumbers the finger.
  private touchContactIds: Map<number, number> = new Map<number, number>();
  private pendingCancelIds: number[] = [];
  private cancelTimers: Map<number, number> = new Map<number, number>();
  private touchpadWheel: TouchpadWheelMapper = new TouchpadWheelMapper();
  // Mouse wheel keeps its own mapper: it is not the touchpad, so the touchpad speed setting does not apply.
  private wheelMapper: TouchpadWheelMapper = new TouchpadWheelMapper();
  private wheelPxPerVp: number = 0;
  private axisSamples: AxisSample[] = [];
  private axisAccumV: number = 0;
  private axisAccumH: number = 0;
  private flingTimer: number = -1;
  private flingOmega: number = 0;
  private flingTotalV: number = 0;
  private flingTotalH: number = 0;
  private flingEmittedV: number = 0;
  private flingEmittedH: number = 0;
  private flingStartAt: number = 0;
  private flingX: number = 0;
  private flingY: number = 0;
  private wheelChunks: WheelChunk[] = [];
  private touchpadMiddleDoubleTap: boolean = false;
  private touchpadTap: TouchpadTapFilter;
  private touchpadDevices: Map<number, boolean> = new Map<number, boolean>();
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
  private pinchCursorHandler: PinchCursorHandler | undefined = undefined;
  private pinchProbeHandler: PinchProbeHandler | undefined = undefined;
  private deviceChangeListener: (data: inputDevice.DeviceListener) => void =
    (data: inputDevice.DeviceListener) => {
      this.touchpadDevices.clear();
    };

  constructor(sink: InputSink) {
    this.sink = sink;
    this.touchpadTap = new TouchpadTapFilter(this, TOUCHPAD_DOUBLE_TAP_MS,
      TOUCHPAD_PRESS_HOLD_MS, TOUCHPAD_MOVE_SLOP);
    try {
      // Device IDs can change on replug, so drop the touchpad cache on a change.
      inputDevice.on('change', this.deviceChangeListener);
    } catch (error) {
    }
  }

  // TouchpadTapTarget
  rightDown(x: number, y: number): void {
    this.handleButton(InputAction.PRESS, InputButton.RIGHT, x, y);
  }

  rightUp(x: number, y: number): void {
    this.handleButton(InputAction.RELEASE, InputButton.RIGHT, x, y);
  }

  clickRight(x: number, y: number): void {
    this.sendSyntheticClick(InputButton.RIGHT, x, y);
  }

  clickMiddle(x: number, y: number): void {
    this.sendSyntheticClick(InputButton.MIDDLE, x, y);
  }

  dispose(): void {
    try {
      inputDevice.off('change', this.deviceChangeListener);
    } catch (error) {
    }
    this.endPinch();
    this.touchpadTap.reset();
  }

  setWinKeySubstitute(keyCode: number): void {
    this.winKeySubstitute = keyCode;
  }

  setPointerSpace(width: number, height: number): void {
    this.pointerWidth = width;
    this.pointerHeight = height;
  }

  setPointMapper(mapper: PointMapper): void {
    this.pointMapper = mapper;
  }

  setTouchEnabled(enabled: boolean): void {
    this.touchEnabled = enabled;
  }

  setTouchpadScrollSpeed(scale: number): void {
    this.touchpadWheel.setSpeed(scale);
  }

  setTouchpadMiddleDoubleTap(enabled: boolean): void {
    this.touchpadMiddleDoubleTap = enabled;
    if (!enabled) {
      this.touchpadTap.reset();
    }
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

  setPinchCursorHandler(handler: PinchCursorHandler): void {
    this.pinchCursorHandler = handler;
  }

  setPinchProbeHandler(handler: PinchProbeHandler): void {
    this.pinchProbeHandler = handler;
  }

  private probePinch(active: boolean, p1x: number, p1y: number, p2x: number, p2y: number): void {
    const handler: PinchProbeHandler | undefined = this.pinchProbeHandler;
    if (handler !== undefined) {
      handler(active, p1x, p1y, p2x, p2y);
    }
  }

  // While a system key interceptor owns the keyboard, onKeyEvent must not also forward.
  setInterceptActive(active: boolean): void {
    this.interceptActive = active;
  }

  handleKey(event: KeyEvent): void {
    if (this.interceptActive) {
      return;
    }
    const down: boolean = event.type === KeyType.Down;
    // The real Win key is owned by the system; Win for the remote comes from the substitute key.
    if (event.keyCode === 2076 || event.keyCode === 2077) {
      return;
    }
    const mapping: KeyMapping | undefined = KeyMapper.mapFull(event.keyCode);
    const lockKey: boolean = mapping !== undefined && mapping.lock !== 0;
    // A non-lock key is synced before itself so the character gets the right case; a lock key after, since its press toggles the remote.
    if (!lockKey) {
      this.syncLocalLocks(event);
    }
    if (this.winKeySubstitute !== 0 && event.keyCode === this.winKeySubstitute) {
      this.trackMeta(down);
      this.sink.metaKey(down);
      return;
    }
    if (mapping !== undefined) {
      this.trackKey(event.keyCode, mapping, down);
      this.sink.key(mapping, down);
      if (lockKey) {
        this.syncLocalLocks(event);
      }
      return;
    }
    const text: string = event.keyText;
    if (text.length === 1) {
      const code: number = text.charCodeAt(0);
      if (code >= 0x20) {
        this.trackUnicodeKey(code, down);
        this.sink.unicode(code, down);
      }
    }
  }

  private trackUnicodeKey(code: number, down: boolean): void {
    if (down) {
      this.heldUnicode.add(code);
    } else {
      this.heldUnicode.delete(code);
    }
  }

  // A key delivered by the system key interceptor; unlike handleKey it forwards the real Win key.
  handleInterceptedKey(keyCode: number, down: boolean): void {
    const mapping: KeyMapping | undefined = KeyMapper.mapFull(keyCode);
    if (mapping === undefined) {
      return;
    }
    this.trackKey(keyCode, mapping, down);
    this.sink.key(mapping, down);
  }

  // A touchscreen can also emit compatibility mouse events; those go through handleTouch instead.
  handleMouseEvent(event: MouseEvent): void {
    if (this.touchEnabled && event.source === SourceType.TouchScreen) {
      return;
    }
    const point: number[] = this.pointMapper(event.x, event.y);
    if (this.touchpadMiddleDoubleTap && this.isTouchpad(event.deviceId, event.sourceTool)) {
      if (event.action === MouseAction.Move) {
        this.touchpadTap.observeMove(point[0], point[1]);
      } else if (event.action === MouseAction.CANCEL) {
        this.touchpadTap.cancel(point[0], point[1]);
      } else if (event.button === MouseButton.Right) {
        if (event.action === MouseAction.Press) {
          this.touchpadTap.press(point[0], point[1]);
          return;
        }
        if (event.action === MouseAction.Release) {
          this.touchpadTap.release(point[0], point[1]);
          return;
        }
      }
    }
    if (event.action === MouseAction.Press) {
      this.handleButton(InputAction.PRESS, SessionInput.buttonFor(event.button), point[0], point[1]);
    } else if (event.action === MouseAction.Release) {
      this.handleButton(InputAction.RELEASE, SessionInput.buttonFor(event.button), point[0], point[1]);
    } else if (event.action === MouseAction.Move) {
      this.handleMove(point[0], point[1]);
    }
  }

  handleAxis(event: AxisEvent): void {
    const isPinch: boolean = SessionInput.isPinchEvent(event);
    if (isPinch || this.pinchActive) {
      // The gesture ends only on the real AxisAction.END (fingers lifted).
      if (event.action === AxisAction.END) {
        this.endPinch();
        return;
      }
      if (isPinch) {
        this.updatePinch(event);
      }
      return;
    }
    const touchpad: boolean = this.isTouchpad(event.deviceId, event.sourceTool);
    if (event.action === AxisAction.END || event.action === AxisAction.CANCEL) {
      // Touchpad flicks coast on after the fingers lift (native Scrollable flings); the mouse
      // wheel does not (IsMouseWheelScroll excludes TOUCHPAD).
      if (touchpad && event.action === AxisAction.END) {
        this.startFling();
      }
      return;
    }
    if (event.action !== AxisAction.BEGIN && event.action !== AxisAction.UPDATE) {
      return;
    }
    if (event.action === AxisAction.BEGIN) {
      this.cancelFling();
      this.touchpadWheel.reset();
      this.wheelMapper.reset();
      this.axisSamples.length = 0;
      this.axisAccumV = 0;
      this.axisAccumH = 0;
    }
    const vertical: number = event.axisVertical ?? 0;
    const horizontal: number = event.axisHorizontal ?? 0;
    if (vertical === 0 && horizontal === 0) {
      return;
    }
    const point: number[] = this.pointMapper(event.x, event.y);
    if (touchpad) {
      this.trackAxisVelocity(vertical, horizontal);
      this.flingX = point[0];
      this.flingY = point[1];
      this.handleWheelScroll(this.touchpadWheel, vertical, horizontal, point);
    } else {
      // Wheel axis is in degrees; ArkUI turns one degree into LINE_HEIGHT_DESKTOP / MOUSE_WHEEL_DEGREES
      // vp and then that vp into px (x density), so fold both in to share the touchpad's px口径 and
      // keep high-resolution deltas. The host wheel-speed setting rides in the axis value, so
      // scrollStep is not read; the remote follows it implicitly.
      const scale: number = WHEEL_VP_PER_DEGREE * this.pxPerVp();
      this.handleWheelScroll(this.wheelMapper, vertical * scale, horizontal * scale, point);
    }
  }

  // Only real touchscreen input reaches here; converted mouse touches go through handleMouseEvent.
  handleTouch(event: TouchEvent): void {
    if (!this.touchEnabled || event.source !== SourceType.TouchScreen) {
      return;
    }
    const changed: TouchObject[] = event.changedTouches;
    for (let i = 0; i < changed.length; i++) {
      const touch: TouchObject = changed[i];
      const point: number[] = this.pointMapper(touch.x, touch.y);
      const pressure: number = SessionInput.normalizePressure(touch.pressure);
      if (touch.type === TouchType.Down) {
        this.onTouchDown(touch.id, point, pressure);
      } else if (touch.type === TouchType.Move) {
        this.onTouchMove(touch.id, point, pressure);
      } else if (touch.type === TouchType.Up) {
        this.onTouchUp(touch.id, point, pressure);
      } else if (touch.type === TouchType.Cancel) {
        this.onTouchCancel(touch.id, point);
      }
    }
  }

  releaseAll(): void {
    if (this.moveTimer !== -1) {
      clearTimeout(this.moveTimer);
      this.moveTimer = -1;
    }
    this.pendingMoveX = -1;
    this.pendingMoveY = -1;
    this.heldKeys.forEach((mapping: KeyMapping) => {
      this.sink.key(mapping, false);
    });
    this.heldKeys.clear();
    this.heldUnicode.forEach((code: number) => {
      this.sink.unicode(code, false);
    });
    this.heldUnicode.clear();
    if (this.heldMeta) {
      this.sink.metaKey(false);
      this.heldMeta = false;
    }
    this.heldButtons.forEach((button: number) => {
      this.sink.button(InputAction.RELEASE, button, this.lastPointerX, this.lastPointerY);
    });
    this.heldButtons.clear();
    this.releaseAllTouches();
    this.endPinch();
    this.cancelFling();
    // Don't let a pending 中键 candidate fire into a window that lost focus.
    this.touchpadTap.reset();
  }

  private handleButton(action: number, button: number, x: number, y: number): void {
    this.lastPointerX = x;
    this.lastPointerY = y;
    // Any button event flushes the pending move first, or the click lands on a stale position.
    this.flushMove();
    this.sink.button(action, button, x, y);
    if (action === InputAction.PRESS) {
      this.heldButtons.add(button);
    } else {
      this.heldButtons.delete(button);
    }
  }

  private handleMove(x: number, y: number): void {
    this.lastPointerX = x;
    this.lastPointerY = y;
    this.pendingMoveX = x;
    this.pendingMoveY = y;
    const elapsed: number = Date.now() - this.lastMoveSentAt;
    if (elapsed >= MOUSE_MOVE_MIN_INTERVAL_MS) {
      this.flushMove();
    } else if (this.moveTimer === -1) {
      this.moveTimer = setTimeout(() => {
        this.moveTimer = -1;
        this.flushMove();
      }, MOUSE_MOVE_MIN_INTERVAL_MS - elapsed);
    }
  }

  /** One whole click as press + release, so the remote never keeps a button down. */
  private sendSyntheticClick(button: number, x: number, y: number): void {
    this.handleButton(InputAction.PRESS, button, x, y);
    this.handleButton(InputAction.RELEASE, button, x, y);
  }

  private trackAxisVelocity(vertical: number, horizontal: number): void {
    const now: number = Date.now();
    this.axisAccumV += vertical;
    this.axisAccumH += horizontal;
    this.axisSamples.push(new AxisSample(now, this.axisAccumV, this.axisAccumH));
    while (this.axisSamples.length > 1 && now - this.axisSamples[0].t > AXIS_VELOCITY_WINDOW_MS) {
      this.axisSamples.shift();
    }
  }

  private startFling(): void {
    this.cancelFling();
    const count: number = this.axisSamples.length;
    if (count < 2) {
      return;
    }
    const first: AxisSample = this.axisSamples[0];
    const last: AxisSample = this.axisSamples[count - 1];
    const dt: number = (last.t - first.t) / 1000;
    if (dt <= 0) {
      return;
    }
    const velV: number = (last.v - first.v) / dt;
    const velH: number = (last.h - first.h) / dt;
    if (Math.sqrt(velV * velV + velH * velH) < FLING_MIN_VELOCITY) {
      return;
    }
    this.flingOmega = FLING_FRICTION * FLING_FRICTION_SCALE;
    const travel: number = FLING_VELOCITY_SCALE / this.flingOmega;
    this.flingTotalV = velV * travel;
    this.flingTotalH = velH * travel;
    this.flingStartAt = Date.now();
    this.scheduleFlingTick();
  }

  private scheduleFlingTick(): void {
    this.flingTimer = setTimeout(() => {
      this.flingTimer = -1;
      this.tickFling();
    }, FLING_TICK_MS);
  }

  private tickFling(): void {
    const t: number = (Date.now() - this.flingStartAt) / 1000;
    const done: number = 1 - (1 + this.flingOmega * t) * Math.exp(-this.flingOmega * t);
    const reachedV: number = this.flingTotalV * done;
    const reachedH: number = this.flingTotalH * done;
    const dv: number = reachedV - this.flingEmittedV;
    const dh: number = reachedH - this.flingEmittedH;
    this.flingEmittedV = reachedV;
    this.flingEmittedH = reachedH;
    if (dv !== 0 || dh !== 0) {
      this.handleWheelScroll(this.touchpadWheel, dv, dh, [this.flingX, this.flingY]);
    }
    const remain: number = Math.abs(this.flingTotalV - reachedV) + Math.abs(this.flingTotalH - reachedH);
    if (remain < FLING_STOP_PX || t > FLING_MAX_SECONDS) {
      return;
    }
    this.scheduleFlingTick();
  }

  private cancelFling(): void {
    if (this.flingTimer !== -1) {
      clearTimeout(this.flingTimer);
      this.flingTimer = -1;
    }
    this.flingTotalV = 0;
    this.flingTotalH = 0;
    this.flingEmittedV = 0;
    this.flingEmittedH = 0;
  }

  private pxPerVp(): number {
    if (this.wheelPxPerVp <= 0) {
      const disp = defaultDisplay();
      this.wheelPxPerVp = disp !== undefined && disp.densityPixels > 0 ? disp.densityPixels : 1;
    }
    return this.wheelPxPerVp;
  }

  private handleWheelScroll(mapper: TouchpadWheelMapper, vertical: number, horizontal: number,
    point: number[]): void {
    this.wheelChunks.length = 0;
    mapper.map(vertical, horizontal, this.wheelChunks);
    let wheelV: number = 0;
    let wheelH: number = 0;
    for (let i = 0; i < this.wheelChunks.length; i++) {
      const chunk: WheelChunk = this.wheelChunks[i];
      const amount: number = chunk.negative ? -chunk.rotation : chunk.rotation;
      if (chunk.horizontal) {
        wheelH += amount;
      } else {
        wheelV += amount;
      }
    }
    if (wheelV !== 0 || wheelH !== 0) {
      this.sink.scroll(wheelV, wheelH, point[0], point[1]);
    }
  }

  static isPinchEvent(event: AxisEvent): boolean {
    if ((event.axisPinch ?? 0) > 0) {
      return true;
    }
    try {
      return event.hasAxis(AxisType.PINCH_AXIS);
    } catch (error) {
      return false;
    }
  }

  static pinchScaleValue(event: AxisEvent): number {
    const direct: number = event.axisPinch ?? 0;
    if (direct > 0) {
      return direct;
    }
    try {
      return event.getPinchAxisScaleValue();
    } catch (error) {
      return 0;
    }
  }

  private updatePinch(event: AxisEvent): void {
    const scale: number = SessionInput.pinchScaleValue(event);
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
      // Skip MOTIONs that would let FreeRDP's 50Hz RDPEI coalescing overwrite the unsent DOWN; the distance is still tracked above.
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
    // axisPinch is the ratio to the initial distance; the notch step derives from the configured zoom speed, so a larger speed emits more notches.
    this.pinchAccum += scale - this.pinchScale;
    this.pinchScale = scale;
    const notchesPerUnit: number = DEFAULT_PINCH_NOTCHES_PER_UNIT * this.pinchZoomSpeed;
    const step: number = notchesPerUnit > 0 ? 1 / notchesPerUnit : 0;
    if (step <= 0) {
      return;
    }
    const point: number[] = this.pointMapper(event.x, event.y);
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
    // The pair is synthesized the same way in both channels: a base separation that is a
    // percent of that channel's surface, scaled by the pinch ratio. Only the `× ratio` step is
    // shared; the base itself is deliberately different per channel (screen vs pad), as are
    // the surface, floor, anchor and tilt.
    let percent: number;
    let angleDeg: number;
    let surface: number;
    if (this.pinchMode === PINCH_MODE_NATIVE) {
      // No pad position from HarmonyOS: the pair is centered on the pad (surface = 1.0), and
      // the base separation is a fixed fraction of the pad width - it must be wide enough for
      // Windows to classify the gesture and to leave room to pinch in. It is laid along the
      // pad diagonal, where the usable separation is longest.
      this.touchPinchAnchor = [0.5, 0.5];
      this.touchPinchHalfMin = MIN_PINCH_FRAC;
      surface = 1;
      percent = NATIVE_PINCH_BASE_PERCENT;
      angleDeg = NATIVE_PINCH_ANGLE_DEG;
    } else {
      this.touchPinchAnchor = this.pointMapper(event.x, event.y);
      this.touchPinchHalfMin = MIN_PINCH_HALF;
      surface = Math.min(this.pointerWidth, this.pointerHeight);
      percent = Math.max(MIN_PINCH_DISTANCE_PERCENT,
        Math.min(MAX_PINCH_DISTANCE_PERCENT, this.pinchTouchDistance));
      angleDeg = this.pinchTouchAngle;
    }
    this.touchPinchHalf = Math.max(this.touchPinchHalfMin, surface * percent / 100);
    this.touchPinchLastHalf = this.touchPinchHalf;
    // ArkUI reports no finger angle, so a configured (or fixed) tilt stands in for the hand.
    const radians: number = Math.max(-180, Math.min(180, angleDeg)) * Math.PI / 180;
    this.touchPinchUnitX = Math.cos(radians);
    this.touchPinchUnitY = Math.sin(radians);
    this.touchPinchActive = true;
    this.touchPinchZoomIn = true;
    this.updateTouchPinch(scale);
    const handler: PinchCursorHandler | undefined = this.pinchCursorHandler;
    // The locator hand is only for the synthetic-touch path (the OS hides the pointer
    // while a screen touch is down); native touchpad pinch never touches the screen.
    if (handler !== undefined && this.pinchMode === PINCH_MODE_TOUCH) {
      handler(true, this.touchPinchZoomIn);
    }
    this.sendTouchPinchContacts(TouchPhase.DOWN);
    // Let the DOWN frame flush before the first MOTION can overwrite it.
    this.touchPinchMotionAt = Date.now() + PINCH_TOUCH_DOWN_FLUSH_MS;
  }

  private updateTouchPinch(scale: number): void {
    const ratio: number = Math.max(MIN_PINCH_RATIO, Math.min(MAX_PINCH_RATIO, scale));
    this.touchPinchLastHalf =
      Math.max(this.touchPinchHalfMin, this.touchPinchHalf * ratio);
    // Flip the locator only past the dead zone and only when it changes: a per-motion pointer-style call would be a syscall per axis event.
    const zoomIn: boolean = ratio > 1 + PINCH_CURSOR_HYSTERESIS
      ? true : (ratio < 1 - PINCH_CURSOR_HYSTERESIS ? false : this.touchPinchZoomIn);
    if (zoomIn !== this.touchPinchZoomIn) {
      this.touchPinchZoomIn = zoomIn;
      const handler: PinchCursorHandler | undefined = this.pinchCursorHandler;
      if (handler !== undefined && this.pinchMode === PINCH_MODE_TOUCH) {
        handler(true, zoomIn);
      }
    }
  }

  private sendTouchPinchContacts(phase: number): void {
    const point: number[] = this.touchPinchAnchor;
    const half: number = this.touchPinchLastHalf;
    const dx: number = half * this.touchPinchUnitX;
    const dy: number = half * this.touchPinchUnitY;
    if (this.pinchMode === PINCH_MODE_NATIVE) {
      const x1: number = SessionInput.clamp01(point[0] + dx);
      const y1: number = SessionInput.clamp01(point[1] + dy);
      const x2: number = SessionInput.clamp01(point[0] - dx);
      const y2: number = SessionInput.clamp01(point[1] - dy);
      const contacts: TouchpadContact[] = [
        TouchpadContact.of(phase, PINCH_TOUCH_ID_A, x1, y1, 0),
        TouchpadContact.of(phase, PINCH_TOUCH_ID_B, x2, y2, 0)
      ];
      this.sink.touchpadFrame(contacts, 0, TOUCHPAD_WIDTH_MM, TOUCHPAD_HEIGHT_MM);
      this.probePinch(true, x1, y1, x2, y2);
      return;
    }
    const maxX: number = Math.max(0, this.pointerWidth - 1);
    const maxY: number = Math.max(0, this.pointerHeight - 1);
    const x1: number = Math.max(0, Math.min(maxX, Math.round(point[0] + dx)));
    const y1: number = Math.max(0, Math.min(maxY, Math.round(point[1] + dy)));
    const x2: number = Math.max(0, Math.min(maxX, Math.round(point[0] - dx)));
    const y2: number = Math.max(0, Math.min(maxY, Math.round(point[1] - dy)));
    this.sink.touch(phase, PINCH_TOUCH_ID_A, x1, y1, 0);
    this.sink.touch(phase, PINCH_TOUCH_ID_B, x2, y2, 0);
    const pw: number = this.pointerWidth > 0 ? this.pointerWidth : 1;
    const ph: number = this.pointerHeight > 0 ? this.pointerHeight : 1;
    this.probePinch(true, x1 / pw, y1 / ph, x2 / pw, y2 / ph);
  }

  private static clamp01(value: number): number {
    return Math.max(0, Math.min(1, value));
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
    this.probePinch(false, 0, 0, 0, 0);
    const handler: PinchCursorHandler | undefined = this.pinchCursorHandler;
    if (handler !== undefined && this.pinchMode === PINCH_MODE_TOUCH) {
      handler(false, this.touchPinchZoomIn);
    }
  }

  private setPinchCtrl(down: boolean): void {
    if (this.pinchCtrlDown === down) {
      return;
    }
    this.pinchCtrlDown = down;
    this.sink.key(PINCH_CTRL, down);
  }

  private endPinch(): void {
    this.endTouchPinch();
    this.pinchActive = false;
    this.pinchScale = 0;
    this.pinchAccum = 0;
    this.touchPinchMotionAt = 0;
    // Release unconditionally: the final event may not carry the pinch axis, so pinchActive can already be false while Ctrl is still logically held.
    this.setPinchCtrl(false);
  }

  // A touchpad tap loses the TOUCHPAD tag, so the originating device is the fallback; sourceTool is checked first to keep scroll cheap.
  isTouchpad(deviceId: number | undefined, sourceTool: SourceTool | undefined): boolean {
    return sourceTool === SourceTool.TOUCHPAD || this.isTouchpadDevice(deviceId);
  }

  private isTouchpadDevice(deviceId: number | undefined): boolean {
    if (deviceId === undefined || deviceId <= 0) {
      return false;
    }
    const cached: boolean | undefined = this.touchpadDevices.get(deviceId);
    if (cached !== undefined) {
      return cached;
    }
    try {
      const info: inputDevice.InputDeviceData = inputDevice.getDeviceInfoSync(deviceId);
      const touchpad: boolean = info.sources.indexOf('touchpad') >= 0;
      this.touchpadDevices.set(deviceId, touchpad);
      return touchpad;
    } catch (error) {
    }
    return false;
  }

  private onTouchDown(id: number, point: number[], pressure: number): void {
    // A Down for a contact already tracked is a retransmission.
    if (this.touchContactIds.has(id)) {
      return;
    }
    // A held Cancel resumed near its last position means the same finger is still down: move it instead of lifting and re-pressing.
    const resumed: number = this.findResumableCancel(point);
    if (resumed >= 0) {
      const known: number | undefined = this.touchContactIds.get(resumed);
      const resumedId: number = known !== undefined ? known : resumed + 1;
      this.clearCancelTimer(resumed);
      this.removePendingCancel(resumed);
      this.touchPoints.delete(resumed);
      this.touchContactIds.delete(resumed);
      this.touchPoints.set(id, point);
      this.touchContactIds.set(id, resumedId);
      this.sink.touch(TouchPhase.MOVE, resumedId, point[0], point[1], pressure);
      return;
    }
    this.finalizeAllCancels();
    const pointerId: number = id + 1;
    this.touchPoints.set(id, point);
    this.touchContactIds.set(id, pointerId);
    this.sink.touch(TouchPhase.DOWN, pointerId, point[0], point[1], pressure);
  }

  private onTouchMove(id: number, point: number[], pressure: number): void {
    const pointerId: number | undefined = this.touchContactIds.get(id);
    if (pointerId === undefined) {
      return;
    }
    if (this.isPendingCancel(id)) {
      this.clearCancelTimer(id);
      this.removePendingCancel(id);
    }
    const last: number[] | undefined = this.touchPoints.get(id);
    if (last !== undefined && last[0] === point[0] && last[1] === point[1]) {
      return;
    }
    this.touchPoints.set(id, point);
    this.sink.touch(TouchPhase.MOVE, pointerId, point[0], point[1], pressure);
  }

  private onTouchUp(id: number, point: number[], pressure: number): void {
    const pointerId: number | undefined = this.touchContactIds.get(id);
    if (pointerId === undefined) {
      return;
    }
    this.clearCancelTimer(id);
    this.removePendingCancel(id);
    this.touchPoints.delete(id);
    this.touchContactIds.delete(id);
    this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], pressure);
  }

  private onTouchCancel(id: number, point: number[]): void {
    if (!this.touchContactIds.has(id) || this.isPendingCancel(id)) {
      return;
    }
    // The finger is often still down; CANCEL_HOLD_MS is only a safety release for a Cancel that never resumes.
    this.touchPoints.set(id, point);
    this.pendingCancelIds.push(id);
    const timer: number = setTimeout(() => {
      this.cancelTimers.delete(id);
      this.finalizeCancel(id);
    }, CANCEL_HOLD_MS);
    this.cancelTimers.set(id, timer);
  }

  private findResumableCancel(point: number[]): number {
    const minDim: number = Math.min(this.pointerWidth, this.pointerHeight);
    const limit: number = Math.max(CANCEL_RESUME_MIN_DISTANCE,
      Math.min(CANCEL_RESUME_MAX_DISTANCE, Math.round(minDim * CANCEL_RESUME_RATIO)));
    const limitSq: number = limit * limit;
    let best: number = -1;
    let bestDist: number = limitSq;
    for (let i = 0; i < this.pendingCancelIds.length; i++) {
      const id: number = this.pendingCancelIds[i];
      const last: number[] | undefined = this.touchPoints.get(id);
      if (last === undefined) {
        continue;
      }
      const dx: number = last[0] - point[0];
      const dy: number = last[1] - point[1];
      const dist: number = dx * dx + dy * dy;
      if (dist <= bestDist) {
        bestDist = dist;
        best = id;
      }
    }
    return best;
  }

  private isPendingCancel(id: number): boolean {
    return this.pendingCancelIds.indexOf(id) >= 0;
  }

  private removePendingCancel(id: number): void {
    const index: number = this.pendingCancelIds.indexOf(id);
    if (index >= 0) {
      this.pendingCancelIds.splice(index, 1);
    }
  }

  private clearCancelTimer(id: number): void {
    const timer: number | undefined = this.cancelTimers.get(id);
    if (timer !== undefined) {
      clearTimeout(timer);
      this.cancelTimers.delete(id);
    }
  }

  private finalizeCancel(id: number): void {
    this.clearCancelTimer(id);
    this.removePendingCancel(id);
    const pointerId: number | undefined = this.touchContactIds.get(id);
    const point: number[] | undefined = this.touchPoints.get(id);
    this.touchPoints.delete(id);
    this.touchContactIds.delete(id);
    if (pointerId !== undefined && point !== undefined) {
      this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], 0);
    }
  }

  private finalizeAllCancels(): void {
    // Copy first: finalizeCancel mutates the list.
    const ids: number[] = this.pendingCancelIds.slice();
    for (let i = 0; i < ids.length; i++) {
      this.finalizeCancel(ids[i]);
    }
  }

  private releaseAllTouches(): void {
    this.finalizeAllCancels();
    // Lift every tracked contact individually; a protocol-specific "all gone" primitive
    // would leave RDP with stuck fingers.
    this.touchContactIds.forEach((pointerId: number, id: number) => {
      const point: number[] | undefined = this.touchPoints.get(id);
      if (point !== undefined) {
        this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], 0);
      }
    });
    this.touchPoints.clear();
    this.touchContactIds.clear();
  }

  // ArkUI pressure is [0, 65535); shared form is 0..1, where zero means the device did not report it.
  private static normalizePressure(pressure: number | undefined): number {
    if (pressure === undefined || pressure <= 0) {
      return 0;
    }
    return Math.min(1, pressure / 65535);
  }

  private static buttonFor(button: MouseButton): number {
    if (button === MouseButton.Left) {
      return InputButton.LEFT;
    }
    if (button === MouseButton.Middle) {
      return InputButton.MIDDLE;
    }
    if (button === MouseButton.Right) {
      return InputButton.RIGHT;
    }
    return 0;
  }

  private flushMove(): void {
    if (this.moveTimer !== -1) {
      clearTimeout(this.moveTimer);
      this.moveTimer = -1;
    }
    if (this.pendingMoveX < 0) {
      return;
    }
    const x: number = this.pendingMoveX;
    const y: number = this.pendingMoveY;
    this.pendingMoveX = -1;
    this.pendingMoveY = -1;
    this.lastMoveSentAt = Date.now();
    this.sink.pointerMove(x, y);
  }

  private trackKey(keyCode: number, mapping: KeyMapping, down: boolean): void {
    const key: string = keyCode.toString();
    if (down) {
      if (!this.heldKeys.has(key)) {
        this.heldKeys.set(key, mapping);
      }
    } else {
      this.heldKeys.delete(key);
    }
  }

  private trackMeta(down: boolean): void {
    this.heldMeta = down;
  }

  // The lock states are only exposed from API 19; when absent the remote keeps its own state.
  private syncLocalLocks(event: KeyEvent): void {
    const caps: boolean | undefined = event.isCapsLockOn;
    const num: boolean | undefined = event.isNumLockOn;
    const scroll: boolean | undefined = event.isScrollLockOn;
    if (caps === undefined || num === undefined || scroll === undefined) {
      return;
    }
    let flags: number = 0;
    if (scroll) {
      flags |= RdpLockFlags.SCROLL;
    }
    if (num) {
      flags |= RdpLockFlags.NUM;
    }
    if (caps) {
      flags |= RdpLockFlags.CAPS;
    }
    this.sink.lockState(flags);
  }
}
