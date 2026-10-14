import { HeldInputs } from './input/HeldInputs';
import { GamepadSink, InputAction, InputButton, InputSink, PadProbeHandler, PinchCursorHandler,
  PointMapper, TouchpadContact, TouchPhase } from './input/InputSink';
import { GamepadInput } from './input/GamepadInput';
import { isPinchEvent, InputGeometry, MOUSE_MOVE_MIN_INTERVAL_MS, pinchScaleValue,
  TOUCH_MOVE_INTERVAL_FALLBACK_MS } from './input/InputPolicy';
import { KeyInput } from './input/KeyInput';
import { PinchInput } from './input/PinchInput';
import { PointerInput } from './input/PointerInput';
import { TouchInput } from './input/TouchInput';
import { TouchpadInput } from './input/TouchpadInput';
import { WheelInput } from './input/WheelInput';

export { GamepadInput, GamepadSink, InputAction, InputButton, InputSink, TouchPhase,
  TouchpadContact };
export type { PadProbeHandler, PinchCursorHandler, PointMapper };

// Thin facade over services/input/*: owns the sink and shared geometry, and is the only place
// that knows the controllers' ordering.
export class SessionInput {
  private sink: InputSink;
  private geometry: InputGeometry = new InputGeometry();
  private held: HeldInputs = new HeldInputs();
  private keys: KeyInput;
  private pointer: PointerInput;
  private touch: TouchInput;
  private touchpad: TouchpadInput;
  private pinch: PinchInput;
  private wheel: WheelInput;
  private touchMoveIntervalMs: number = TOUCH_MOVE_INTERVAL_FALLBACK_MS;
  private padProbeHandler: PadProbeHandler | undefined = undefined;
  private gamepad: GamepadInput | undefined = undefined;

  constructor(sink: InputSink) {
    this.sink = sink;
    this.keys = new KeyInput(sink, this.held);
    this.pointer = new PointerInput(sink, this.held, this.geometry, MOUSE_MOVE_MIN_INTERVAL_MS);
    this.touch = new TouchInput(sink, this.geometry, this.touchMoveIntervalMs);
    this.touchpad = new TouchpadInput(sink, this.pointer);
    this.pinch = new PinchInput(sink, this.geometry);
    this.wheel = new WheelInput(sink, this.geometry,
      (deviceId: number | undefined, sourceTool: SourceTool | undefined): boolean =>
        this.touchpad.isTouchpad(deviceId, sourceTool),
      (): boolean => this.pinch.isActive(),
      (): boolean => this.touchpad.isPanEnabled());
    this.touch.setDirectContactListener((): void => {
      this.wheel.noteDirectContact();
    });
    const probe: PadProbeHandler =
      (active: boolean, p1x: number, p1y: number, p2x: number, p2y: number): void => {
        const handler: PadProbeHandler | undefined = this.padProbeHandler;
        if (handler !== undefined) {
          handler(active, p1x, p1y, p2x, p2y);
        }
      };
    this.pinch.setProbe(probe);
    this.touchpad.setProbe(probe);
  }

  dispose(): void {
    this.touchpad.dispose();
    this.pinch.end();
    this.gamepad?.dispose();
  }

  // Only Moonlight has a gamepad path; a session without one leaves the keys to the keyboard path.
  setGamepad(gamepad: GamepadInput): void {
    this.gamepad = gamepad;
  }

  setWinKeySubstitute(keyCode: number): void {
    this.keys.setWinKeySubstitute(keyCode);
  }

  setPointerSpace(width: number, height: number): void {
    this.geometry.setSpace(width, height);
  }

  setPointMapper(mapper: PointMapper): void {
    this.geometry.setMapper(mapper);
  }

  setTouchEnabled(enabled: boolean): void {
    this.touch.setTouchEnabled(enabled);
  }

  setTouchMoveIntervalMs(ms: number): void {
    this.touchMoveIntervalMs = ms;
    this.touch.setMoveInterval(ms);
  }

  setTouchpadScrollSpeed(scale: number): void {
    this.wheel.setTouchpadScrollSpeed(scale);
  }

  setTouchpadMiddleDoubleTap(enabled: boolean): void {
    this.touchpad.setMiddleDoubleTap(enabled);
  }

  setPinchMode(mode: number): void {
    this.pinch.setPinchMode(mode);
  }

  setPinchZoomSpeed(speed: number): void {
    this.pinch.setPinchZoomSpeed(speed);
  }

  setPinchTouchDistance(percent: number): void {
    this.pinch.setPinchTouchDistance(percent);
  }

  setPinchTouchAngle(degrees: number): void {
    this.pinch.setPinchTouchAngle(degrees);
  }

  setPinchCursorHandler(handler: PinchCursorHandler): void {
    this.pinch.setCursorHandler(handler);
  }

  setPadProbeHandler(handler: PadProbeHandler): void {
    this.padProbeHandler = handler;
  }

  setInterceptActive(active: boolean): void {
    this.keys.setInterceptActive(active);
  }

  setEngineEnabled(enabled: boolean): void {
    this.wheel.setEngineEnabled(enabled);
  }

  setTouchpadPanEnabled(enabled: boolean): void {
    this.touchpad.setPanEnabled(enabled);
  }

  setRelativeSpeed(speed: number): void {
    this.pointer.setRelativeSpeed(speed);
  }

  // Relative-pointer mode: the host owns the cursor position, so motion is the difference of the
  // local pointer's mapped positions (touchpad included) and buttons carry no position.
  setRelativePointer(enabled: boolean): void {
    this.sink.setRelativePointer(enabled);
    this.pointer.setRelativePointer(enabled);
  }

  // Absolute protocols report positions continuously, so only relative mode needs an anchor.
  reportPointerPosition(): boolean {
    if (!this.sink.relativePointerEnabled()) {
      return false;
    }
    return this.pointer.reportPosition();
  }

  // Focus regained: the host cursor may have been left anywhere, so the next pointer event
  // re-states the local position before resuming relative motion.
  reanchorPointer(): void {
    if (!this.sink.relativePointerEnabled()) {
      return;
    }
    this.pointer.reanchor();
  }

  isTouchpad(deviceId: number | undefined, sourceTool: SourceTool | undefined): boolean {
    return this.touchpad.isTouchpad(deviceId, sourceTool);
  }

  static isPinchEvent(event: AxisEvent): boolean {
    return isPinchEvent(event);
  }

  static pinchScaleValue(event: AxisEvent): number {
    return pinchScaleValue(event);
  }

  handleKey(event: KeyEvent): void {
    // Gamepad keys are taken first and never reach the key table (its D-pad codes map to arrows).
    if (this.gamepad !== undefined && this.gamepad.handleKey(event)) {
      return;
    }
    this.keys.handleKey(event);
  }

  handleFocusAxis(event: FocusAxisEvent): void {
    this.gamepad?.handleFocusAxis(event);
  }

  handleInterceptedKey(keyCode: number, down: boolean): void {
    this.keys.handleInterceptedKey(keyCode, down);
  }

  handleMouseEvent(event: MouseEvent): void {
    if (this.touch.isEnabled() && event.source === SourceType.TouchScreen) {
      return;
    }
    const point: number[] = this.geometry.map(event.x, event.y);
    if (this.touchpad.consumeMouse(event, point)) {
      return;
    }
    this.pointer.handleMouseEvent(event, point);
  }

  handleAxis(event: AxisEvent): void {
    const isPinch: boolean = isPinchEvent(event);
    if (isPinch || this.pinch.isActive()) {
      if (isPinch) {
        this.touchpad.endPan();
      }
      this.pinch.handleAxis(event);
      return;
    }
    const touchpadEvent: boolean = this.touchpad.isTouchpad(event.deviceId, event.sourceTool);
    const panning: boolean = this.touchpad.isPanEnabled() && touchpadEvent;
    if (event.action === AxisAction.END || event.action === AxisAction.CANCEL) {
      if (panning) {
        this.touchpad.schedulePanEnd();
      }
      return;
    }
    if (event.action !== AxisAction.BEGIN && event.action !== AxisAction.UPDATE) {
      return;
    }
    if (event.action === AxisAction.BEGIN) {
      this.wheel.beginAxis();
      this.touchpad.cancelPanEnd();
    }
    const vertical: number = event.axisVertical ?? 0;
    const horizontal: number = event.axisHorizontal ?? 0;
    if (vertical === 0 && horizontal === 0) {
      return;
    }
    const point: number[] = this.geometry.map(event.x, event.y);
    if (panning) {
      this.touchpad.handlePan(vertical, horizontal);
      return;
    }
    this.pointer.flushMove();
    this.wheel.scroll(vertical, horizontal, point, touchpadEvent);
  }

  handleTouch(event: TouchEvent): void {
    this.touch.handleTouch(event);
  }

  // Replay entry for a caller that withheld a contact (the toolbar reveal gesture) and is now
  // handing it back to the ordinary touch path.
  handleTouchPoint(phase: TouchType, id: number, x: number, y: number,
    pressure: number | undefined): void {
    this.touch.handleTouchPoint(phase, id, x, y, pressure);
  }

  noteTouchContact(): void {
    this.touch.noteDirectContact();
  }

  touchForwarded(): boolean {
    return this.touch.isEnabled();
  }

  // Frames carry neither position nor source: the wheel/touchpad split comes from the raw axis events.
  noteAxisEvent(event: AxisEvent): void {
    this.wheel.noteAxisEvent(event);
  }

  engineAcceptsFrame(): boolean {
    return this.wheel.engineAcceptsFrame();
  }

  handleEngineScroll(dx: number, dy: number, x: number, y: number): void {
    this.wheel.handleEngineScroll(dx, dy, x, y);
  }

  consumesAxis(event: AxisEvent): boolean {
    return this.wheel.consumesAxis(event);
  }

  releaseAll(): void {
    this.pointer.cancelMove();
    this.held.releaseAll(this.sink, this.pointer.lastPointerX(), this.pointer.lastPointerY());
    this.touch.releaseAll();
    this.touchpad.endPan();
    this.pinch.end();
    this.touchpad.resetTap();
    this.gamepad?.releaseAll();
  }
}
