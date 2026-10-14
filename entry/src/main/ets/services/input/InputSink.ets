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

export type PointMapper = (x: number, y: number) => number[];
export type PinchCursorHandler = (active: boolean, zoomIn: boolean) => void;
export type PinchProbeHandler =
  (active: boolean, p1x: number, p1y: number, p2x: number, p2y: number) => void;
