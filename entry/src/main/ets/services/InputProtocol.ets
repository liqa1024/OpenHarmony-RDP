import { MOUSE_MOVE_MIN_INTERVAL_MS } from './input/InputPolicy';

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

// Delivery class of a stream (INPUT_PIPELINE_PLAN.md §3). The stream's window below is its budget:
// an upper bound (defer within it) or, for BOUNDED_REPEAT, a lower bound (drop above it).
export class InputDelivery {
  static readonly ORDERED_RELIABLE: number = 0;
  static readonly LATEST_WINS: number = 1;
  static readonly BOUNDED_REPEAT: number = 2;
  static readonly COALESCE_WINDOW: number = 3;
}

export class InputOp {
  static readonly KEY: number = 1;
  static readonly KEY_REPEAT: number = 2;
  static readonly META: number = 3;
  static readonly UNICODE: number = 4;
  static readonly TEXT_RUN: number = 5;
  static readonly LOCK: number = 6;
  static readonly MOVE_ABS: number = 7;
  static readonly MOVE_REL: number = 8;
  static readonly BUTTON: number = 9;
  static readonly TOUCH_MOVE: number = 10;
  static readonly TOUCH_POINT: number = 11;
  static readonly TOUCHPAD_FRAME: number = 12;
  static readonly SCROLL: number = 13;
  static readonly GAMEPAD_STATE: number = 14;
  static readonly GAMEPAD_ARRIVAL: number = 15;
  // A touchpad frame that carries a contact down/up/cancel is a state transition, so it gets its
  // own ordered op instead of the frame's LATEST_WINS stream.
  static readonly TOUCHPAD_POINT: number = 16;
}

// One slot per op: the gate indexes its per-stream state by op.
export const INPUT_OP_SLOTS: number = 17;

// A transport with one packet per text run (Moonlight's LiSendUtf8TextEvent) lets the gate batch
// code points; without it each one keeps its own ordered call. Declared here so the gate reads a
// capability instead of branching on a protocol.
export function inputTextRun(protocol: number): boolean {
  return protocol === InputProtocol.MOONLIGHT;
}

// The delivery declaration: a new stream is a row here, nothing else changes. Two answers instead of
// a row object because the high-frequency path must not allocate per event.
export function inputDelivery(op: number): number {
  if (op === InputOp.MOVE_ABS || op === InputOp.MOVE_REL || op === InputOp.TOUCH_MOVE
    || op === InputOp.TOUCHPAD_FRAME || op === InputOp.GAMEPAD_STATE) {
    return InputDelivery.LATEST_WINS;
  }
  if (op === InputOp.TEXT_RUN) {
    return InputDelivery.COALESCE_WINDOW;
  }
  if (op === InputOp.KEY_REPEAT) {
    return InputDelivery.BOUNDED_REPEAT;
  }
  return InputDelivery.ORDERED_RELIABLE;
}

// The stream's budget in ms: 0 = straight through (the transport coalesces the stream itself, so an
// extra window would only add latency; a straight-through stream also keeps call order).
export function inputWindowMs(op: number, protocol: number, touchMoveWindowMs: number): number {
  if (op === InputOp.MOVE_ABS || op === InputOp.MOVE_REL) {
    return protocol === InputProtocol.MOONLIGHT ? 0 : MOUSE_MOVE_MIN_INTERVAL_MS;
  }
  if (op === InputOp.TOUCH_MOVE) {
    return touchMoveWindowMs;
  }
  if (op === InputOp.TEXT_RUN) {
    return INPUT_TEXT_RUN_WINDOW_MS;
  }
  if (op === InputOp.KEY_REPEAT) {
    return INPUT_KEY_REPEAT_MIN_INTERVAL_MS;
  }
  if (op === InputOp.TOUCHPAD_FRAME || op === InputOp.GAMEPAD_STATE) {
    return INPUT_LATEST_WINS_WINDOW_MS;
  }
  return 0;
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
  text?: string;
  // Flat numeric fields only: a touchpad frame is TOUCHPAD_FRAME_STRIDE-wide contacts, a key is
  // four numbers (InputKey), a pad's state is nine. No class instance ever crosses the thread.
  frame?: number[];
  key?: InputKey;
  pad?: InputPad;
  arrival?: InputArrival;
}

export interface InputConfig {
  protocol: number;
  handle: number;
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
