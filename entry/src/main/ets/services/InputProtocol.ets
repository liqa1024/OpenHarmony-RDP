import { TouchpadContact } from './input/InputSink';

// Wire shape between L1 (enqueues) and the input worker (sends): one record per verb, flat numeric
// fields. The worker's message queue is the FIFO, so a record's position in it is its order.

export class InputProtocol {
  static readonly RDP: number = 0;
  static readonly MOONLIGHT: number = 1;
}

// LATEST_WINS window: one sample per window, a later one overwrites the pending in place.
export const INPUT_LATEST_WINS_WINDOW_MS: number = 8;

// Text run: code points arriving inside the window go out as one packet (see TextRunSink).
export const INPUT_TEXT_RUN_WINDOW_MS: number = 8;

// Repeat stream rate cap in ms (~50/s): above any OS auto-repeat rate, so ordinary typing never
// trips it and the drop counter stays 0.
export const INPUT_KEY_REPEAT_MIN_INTERVAL_MS: number = 20;

// Delivery class of a stream (INPUT_REFACTOR_PLAN.md §3). The stream's window below is its budget:
// an upper bound (defer within it) or, for BOUNDED_REPEAT, a lower bound (drop above it).
export class InputDelivery {
  static readonly ORDERED_RELIABLE: number = 0;
  static readonly LATEST_WINS: number = 1;
  static readonly BOUNDED_REPEAT: number = 2;
}

// The delivery declaration: a new stream is a row here (class + window), nothing else changes. Two
// answers instead of a row object because the high-frequency path must not allocate per event.
// Two streams answer elsewhere on purpose: the position stream (its window rides the coalescer its
// controller owns, which also flushes before a discrete event — see InputSink.moveDelivery) and the
// text run (L2 accumulates it once it sees a TextRunSink, so the port must not defer it).
export function inputDelivery(op: number): number {
  if (op === InputOp.TOUCHPAD_FRAME || op === InputOp.GAMEPAD_STATE) {
    return InputDelivery.LATEST_WINS;
  }
  if (op === InputOp.KEY_REPEAT) {
    return InputDelivery.BOUNDED_REPEAT;
  }
  return InputDelivery.ORDERED_RELIABLE;
}

export function inputWindowMs(op: number): number {
  const delivery: number = inputDelivery(op);
  if (delivery === InputDelivery.LATEST_WINS) {
    return INPUT_LATEST_WINS_WINDOW_MS;
  }
  if (delivery === InputDelivery.BOUNDED_REPEAT) {
    return INPUT_KEY_REPEAT_MIN_INTERVAL_MS;
  }
  return 0;
}

export class InputOp {
  static readonly KEY: number = 1;
  static readonly META: number = 2;
  static readonly UNICODE: number = 3;
  static readonly LOCK: number = 4;
  static readonly MOVE_ABS: number = 5;
  static readonly MOVE_REL: number = 6;
  static readonly BUTTON: number = 7;
  static readonly TOUCH: number = 8;
  static readonly TOUCHPAD_FRAME: number = 9;
  static readonly SCROLL: number = 10;
  static readonly GAMEPAD_STATE: number = 11;
  static readonly GAMEPAD_ARRIVAL: number = 12;
  // Keyboard auto-repeat; its own op so it can carry its own delivery class.
  static readonly KEY_REPEAT: number = 13;
}

export class InputControl {
  static readonly CONFIGURE: number = 100;
  static readonly SET_RELATIVE: number = 101;
  static readonly SET_REFERENCE_SIZE: number = 102;
  static readonly SET_HANDLE: number = 103;
  static readonly TELEMETRY: number = 104;
}

// The protocol-neutral view of a key; each sink reads the fields its protocol uses.
export interface InputKey {
  scancode: number;
  vk: number;
  extended: boolean;
  lock: number;
}

// One controller slot's whole state (Moonlight's LiSendMultiControllerEvent payload).
export interface InputPad {
  controllerNumber: number;
  activeGamepadMask: number;
  buttonFlags: number;
  leftTrigger: number;
  rightTrigger: number;
  leftStickX: number;
  leftStickY: number;
  rightStickX: number;
  rightStickY: number;
}

// One controller's arrival declaration (Moonlight's LiSendControllerArrivalEvent payload).
export interface InputArrival {
  controllerNumber: number;
  activeGamepadMask: number;
  type: number;
  supportedButtonFlags: number;
  capabilities: number;
}

export interface InputRecord {
  op: number;
  a?: number;
  b?: number;
  c?: number;
  d?: number;
  x?: number;
  y?: number;
  z?: number;
  contacts?: TouchpadContact[];
  key?: InputKey;
  pad?: InputPad;
  arrival?: InputArrival;
}

export interface InputConfig {
  protocol: number;
  handle: number;
  hostScalePercent: number;
  // Continuous-position window in ms; 0 = the transport coalesces the stream itself.
  moveIntervalMs: number;
  referenceWidth: number;
  referenceHeight: number;
}

export interface InputCommand {
  control: number;
  config?: InputConfig;
  enabled?: boolean;
  handle?: number;
  width?: number;
  height?: number;
}

// Rejects reported by the input thread, indexed by InputOp (the count is the pipeline's product).
export interface InputTelemetry {
  control: number;
  rejected: number[];
}
