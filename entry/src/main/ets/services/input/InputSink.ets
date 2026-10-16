import { KeyMapping } from '../../utils/KeyMapper';

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

// One precision-touchpad contact per five numbers, the wire shape the input record carries
// (see InputProtocol): phase, pointerId, x, y, pressure.
export const TOUCHPAD_FRAME_STRIDE: number = 5;

export function packTouchpadFrame(contacts: TouchpadContact[]): number[] {
  const frame: number[] = [];
  for (let i = 0; i < contacts.length; i++) {
    const contact: TouchpadContact = contacts[i];
    frame.push(contact.phase, contact.pointerId, contact.x, contact.y, contact.pressure);
  }
  return frame;
}

// L3: send one input verb on the protocol's transport (input thread only). Returns whether the
// transport took it; false is counted by the pipeline as a rejection.
export interface InputSender {
  key(mapping: KeyMapping, down: boolean): boolean;
  metaKey(down: boolean): boolean;
  unicode(code: number, down: boolean): boolean;
  lockState(flags: number): boolean;
  pointerMove(x: number, y: number): boolean;
  // Device counts (ArkUI rawDelta), used only while relativePointerEnabled().
  pointerMoveRelative(deltaX: number, deltaY: number): boolean;
  button(action: number, button: number, x: number, y: number): boolean;
  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): boolean;
  // Native precision-touchpad frame (foundation-sunshine extension): a flat number[] of
  // TOUCHPAD_FRAME_STRIDE-wide contacts over the physical surface (empty = all lifted); the frame
  // carries the pad's physical size in millimetres.
  touchpadFrame(frame: number[], buttonState: number, widthMm: number, heightMm: number): boolean;
  // Wheel deltas in WHEEL_DELTA (120) units; vertical positive is up, horizontal positive is left.
  scroll(vertical: number, horizontal: number, x: number, y: number): boolean;
}

// L1: the sink the controllers see. Sending is an enqueue; the queries answer from local state.
// Its touchpad verb takes the controllers' contacts and packs them for the wire, so no class
// instance ever crosses to the input thread.
export interface InputSink {
  key(mapping: KeyMapping, down: boolean): boolean;
  // A re-press of a held key (auto-repeat): no state changes, so the pipeline may cap its rate.
  keyRepeat(mapping: KeyMapping): boolean;
  metaKey(down: boolean): boolean;
  unicode(code: number, down: boolean): boolean;
  lockState(flags: number): boolean;
  pointerMove(x: number, y: number): boolean;
  pointerMoveRelative(deltaX: number, deltaY: number): boolean;
  button(action: number, button: number, x: number, y: number): boolean;
  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): boolean;
  touchpadFrame(contacts: TouchpadContact[], buttonState: number, widthMm: number,
    heightMm: number): boolean;
  scroll(vertical: number, horizontal: number, x: number, y: number): boolean;
  setRelativePointer(enabled: boolean): void;
  relativePointerEnabled(): boolean;
  // Display scale (percent) in effect on the streamed desktop: relative-motion counts are host
  // logical pixels, so this divides them (0 = unknown/100%).
  hostDisplayScalePercent(): number;
  // The touch-move stream's declared window (ms), a session setting. Every window / rate cap lives
  // in the port's gate, driven by the declaration table (see InputProtocol), so a controller only
  // hands over verbs.
  setTouchMoveInterval(ms: number): void;
  dispose(): void;
}

// Optional: a transport with one packet per text run (Moonlight's LiSendUtf8TextEvent) lets the
// pipeline batch code points; without it each code point keeps its own call.
export interface TextRunSink {
  textRun(text: string): boolean;
}

// Moonlight's controller path: one event carries a slot's whole state, an arrival event declares
// its type / capabilities. RDP has no gamepad, so this stays out of InputSender.
export interface GamepadSink {
  controllerState(controllerNumber: number, activeGamepadMask: number, buttonFlags: number,
    leftTrigger: number, rightTrigger: number, leftStickX: number, leftStickY: number,
    rightStickX: number, rightStickY: number): void;
  controllerArrival(controllerNumber: number, activeGamepadMask: number, type: number,
    supportedButtonFlags: number, capabilities: number): void;
}

// The library calls the Moonlight sink needs, so it can be built on the input thread. Each returns
// the library's result code, which the sink turns into accepted / rejected.
export interface MoonlightSendPort {
  keyVirtual(vk: number, down: boolean, modifiers: number): number;
  mousePosition(x: number, y: number, refWidth: number, refHeight: number): number;
  mouseMove(deltaX: number, deltaY: number): number;
  mouseButton(action: number, button: number): number;
  text(text: string): number;
  touch(eventType: number, pointerId: number, x: number, y: number, pressure: number): number;
  highResScroll(amount: number): number;
  highResHScroll(amount: number): number;
  touchpad(eventTypes: number[], pointerIds: number[], xs: number[], ys: number[], ps: number[],
    widthMm: number, heightMm: number, buttonState: number): number;
  controller(controllerNumber: number, activeGamepadMask: number, buttonFlags: number,
    leftTrigger: number, rightTrigger: number, leftStickX: number, leftStickY: number,
    rightStickX: number, rightStickY: number): number;
  controllerArrival(controllerNumber: number, activeGamepadMask: number, type: number,
    supportedButtonFlags: number, capabilities: number): number;
}

export type PointMapper = (x: number, y: number) => number[];
export type PinchCursorHandler = (active: boolean, zoomIn: boolean) => void;
// Debug tap on the synthesized pad contacts (scroll pan / pinch pair).
export type PadProbeHandler =
  (active: boolean, p1x: number, p1y: number, p2x: number, p2y: number) => void;
