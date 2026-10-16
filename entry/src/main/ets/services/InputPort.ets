import { ErrorEvent, MessageEvents, worker } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { KeyMapping } from '../utils/KeyMapper';
import { ImmediateMoveDelivery, MoveDelivery, MoveFlush } from './input/MoveDelivery';
import { GamepadSink, InputSink, TouchpadContact } from './input/InputSink';
import { RateCoalescer } from './input/RateCoalescer';
import { InputArrival, InputCommand, InputConfig, InputControl, InputDelivery, InputKey, InputOp,
  InputPad, InputRecord, InputTelemetry, inputDelivery, inputWindowMs } from './InputProtocol';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpInput';

const OP_SLOTS: number = 16;
const MAX_PADS: number = 4;

// L1 side of the input pipeline: the controllers' sink. Sends become queued records for the input
// worker; the queries answer from local state (INPUT_REFACTOR_PLAN.md §3).
export class SessionInputPort implements InputSink, GamepadSink {
  private thread: worker.ThreadWorker | undefined = undefined;
  private protocol: number;
  private handle: number;
  private hostScale: number;
  private moveIntervalMs: number;
  private relative: boolean = false;
  private lockFlags: number = 0;
  private lockKnown: boolean = false;
  private referenceWidth: number = 0;
  private referenceHeight: number = 0;
  private readonly drops: number[] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  private rejects: number[] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  private readonly padSlots: InputPad[] = [];
  // Per windowed stream, indexed by op: the pending sample and its window timer.
  private readonly gatePending: (InputRecord | undefined)[] = [];
  private readonly gateTimers: number[] = [];
  // Last accepted sample time per rate-capped stream.
  private readonly lastAcceptedAt: number[] = [];
  private disposed: boolean = false;

  constructor(protocol: number, handle: number, hostScalePercent: number,
    moveIntervalMs: number) {
    this.protocol = protocol;
    this.handle = handle;
    this.hostScale = hostScalePercent;
    this.moveIntervalMs = moveIntervalMs;
    for (let i = 0; i < MAX_PADS; i++) {
      this.padSlots.push(SessionInputPort.newPad());
    }
    for (let i = 0; i < OP_SLOTS; i++) {
      this.gatePending.push(undefined);
      this.gateTimers.push(-1);
      this.lastAcceptedAt.push(-1);
    }
    this.start();
  }

  // ---- L1 queries (answered locally; the input thread is never on this path) ----

  setRelativePointer(enabled: boolean): void {
    this.relative = enabled;
    this.control(InputControl.SET_RELATIVE, (message: InputCommand): void => {
      message.enabled = enabled;
    });
  }

  relativePointerEnabled(): boolean {
    return this.relative;
  }

  hostDisplayScalePercent(): number {
    return this.hostScale;
  }

  // Settlement can precede the input build; the page re-states it once connected.
  setHostScale(percent: number): void {
    this.hostScale = percent;
  }

  moveDelivery<T>(flush: MoveFlush<T>): MoveDelivery<T> {
    // The declared window is a protocol fact: 0 means the transport coalesces the stream itself.
    return this.moveIntervalMs > 0
      ? new RateCoalescer<T>(this.moveIntervalMs, flush)
      : new ImmediateMoveDelivery<T>(flush);
  }

  lastLockFlags(): number {
    return this.lockFlags;
  }

  hasLockState(): boolean {
    return this.lockKnown;
  }

  // Rejects reported by the input thread: the pipeline counts them, the protocol never does.
  inputDropped(): number {
    let total: number = 0;
    for (let i = 0; i < OP_SLOTS; i++) {
      total += this.drops[i] + this.rejects[i];
    }
    return total;
  }

  // ---- Session configuration (forwarded to the input thread) ----

  setHandle(handle: number): void {
    this.handle = handle;
    this.control(InputControl.SET_HANDLE, (message: InputCommand): void => {
      message.handle = handle;
    });
  }

  setReferenceSize(width: number, height: number): void {
    this.referenceWidth = width;
    this.referenceHeight = height;
    this.control(InputControl.SET_REFERENCE_SIZE, (message: InputCommand): void => {
      message.width = width;
      message.height = height;
    });
  }

  dispose(): void {
    this.disposed = true;
    this.clearGates();
    const thread: worker.ThreadWorker | undefined = this.thread;
    this.thread = undefined;
    if (thread !== undefined) {
      try {
        thread.terminate();
      } catch (error) {
      }
    }
  }

  // ---- InputSender: every verb becomes one queued record ----

  key(mapping: KeyMapping, down: boolean): boolean {
    return this.enqueue({ op: InputOp.KEY, key: SessionInputPort.keyOf(mapping), b: down ? 1 : 0 });
  }

  keyRepeat(mapping: KeyMapping): boolean {
    return this.enqueue({ op: InputOp.KEY_REPEAT, key: SessionInputPort.keyOf(mapping), b: 1 });
  }

  metaKey(down: boolean): boolean {
    return this.enqueue({ op: InputOp.META, b: down ? 1 : 0 });
  }

  unicode(code: number, down: boolean): boolean {
    return this.enqueue({ op: InputOp.UNICODE, a: code, b: down ? 1 : 0 });
  }

  lockState(flags: number): boolean {
    this.lockFlags = flags;
    this.lockKnown = true;
    return this.enqueue({ op: InputOp.LOCK, a: flags });
  }

  pointerMove(x: number, y: number): boolean {
    return this.enqueue({ op: InputOp.MOVE_ABS, x: x, y: y });
  }

  pointerMoveRelative(deltaX: number, deltaY: number): boolean {
    return this.enqueue({ op: InputOp.MOVE_REL, x: deltaX, y: deltaY });
  }

  button(action: number, button: number, x: number, y: number): boolean {
    return this.enqueue({ op: InputOp.BUTTON, a: action, b: button, x: x, y: y });
  }

  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): boolean {
    return this.enqueue({ op: InputOp.TOUCH, a: phase, b: pointerId, x: x, y: y, z: pressure });
  }

  touchpadFrame(contacts: TouchpadContact[], buttonState: number, widthMm: number,
    heightMm: number): boolean {
    const record: InputRecord = {
      op: InputOp.TOUCHPAD_FRAME, contacts: contacts, a: buttonState, c: widthMm, d: heightMm
    };
    return this.enqueue(record);
  }

  scroll(vertical: number, horizontal: number, x: number, y: number): boolean {
    return this.enqueue({ op: InputOp.SCROLL, a: vertical, b: horizontal, x: x, y: y });
  }

  // ---- GamepadSink: the slot's whole state is a continuous stream (LATEST_WINS) ----

  controllerState(controllerNumber: number, activeGamepadMask: number, buttonFlags: number,
    leftTrigger: number, rightTrigger: number, leftStickX: number, leftStickY: number,
    rightStickX: number, rightStickY: number): void {
    const pad: InputPad = this.padFor(controllerNumber);
    pad.controllerNumber = controllerNumber;
    pad.activeGamepadMask = activeGamepadMask;
    pad.buttonFlags = buttonFlags;
    pad.leftTrigger = leftTrigger;
    pad.rightTrigger = rightTrigger;
    pad.leftStickX = leftStickX;
    pad.leftStickY = leftStickY;
    pad.rightStickX = rightStickX;
    pad.rightStickY = rightStickY;
    this.enqueue({ op: InputOp.GAMEPAD_STATE, pad: pad });
  }

  // The arrival declaration is a state transition (it must not be merged away).
  controllerArrival(controllerNumber: number, activeGamepadMask: number, type: number,
    supportedButtonFlags: number, capabilities: number): void {
    const arrival: InputArrival = {
      controllerNumber: controllerNumber, activeGamepadMask: activeGamepadMask, type: type,
      supportedButtonFlags: supportedButtonFlags, capabilities: capabilities
    };
    this.enqueue({ op: InputOp.GAMEPAD_ARRIVAL, arrival: arrival });
  }

  // ---- Queue ----

  private send(record: InputRecord): boolean {
    const thread: worker.ThreadWorker | undefined = this.thread;
    if (thread === undefined || this.disposed) {
      return false;
    }
    try {
      thread.postMessage(record, []);
      return true;
    } catch (error) {
      this.bump(this.drops, record.op);
      return false;
    }
  }

  // The declared class decides the gate. LATEST_WINS keeps at most one pending sample (so the queue
  // length is pinned by the stream count, not the event rate) and ends its window on a timer, not a
  // poll. BOUNDED_REPEAT drops above its rate cap — never deferred, since waiting is the latency the
  // order rule forbids. Everything else goes out now.
  private enqueue(record: InputRecord): boolean {
    const op: number = record.op;
    const delivery: number = inputDelivery(op);
    if (delivery === InputDelivery.BOUNDED_REPEAT) {
      const windowMs: number = inputWindowMs(op);
      const now: number = Date.now();
      const last: number = this.lastAcceptedAt[op];
      if (last >= 0 && now - last < windowMs) {
        this.bump(this.drops, op);
        return false;
      }
      this.lastAcceptedAt[op] = now;
      return this.send(record);
    }
    if (delivery !== InputDelivery.LATEST_WINS) {
      return this.send(record);
    }
    const windowMs: number = inputWindowMs(op);
    this.gatePending[op] = record;
    if (this.gateTimers[op] === -1) {
      this.gateTimers[op] = setTimeout((): void => {
        this.gateTimers[op] = -1;
        const pending: InputRecord | undefined = this.gatePending[op];
        this.gatePending[op] = undefined;
        if (pending !== undefined) {
          this.send(pending);
        }
      }, windowMs);
    }
    return true;
  }

  private clearGates(): void {
    for (let i = 0; i < OP_SLOTS; i++) {
      if (this.gateTimers[i] !== -1) {
        clearTimeout(this.gateTimers[i]);
        this.gateTimers[i] = -1;
      }
      this.gatePending[i] = undefined;
    }
  }

  private padFor(controllerNumber: number): InputPad {
    const slot: number = controllerNumber >= 0 && controllerNumber < MAX_PADS ? controllerNumber : 0;
    return this.padSlots[slot];
  }

  private static keyOf(mapping: KeyMapping): InputKey {
    return {
      scancode: mapping.scancode, vk: mapping.vk, extended: mapping.extended, lock: mapping.lock
    };
  }

  private static newPad(): InputPad {
    return {
      controllerNumber: 0, activeGamepadMask: 0, buttonFlags: 0, leftTrigger: 0, rightTrigger: 0,
      leftStickX: 0, leftStickY: 0, rightStickX: 0, rightStickY: 0
    };
  }

  private bump(counts: number[], op: number): void {
    if (op < 0 || op >= OP_SLOTS) {
      return;
    }
    counts[op]++;
  }

  private control(control: number, fill: (message: InputCommand) => void): void {
    const message: InputCommand = { control: control };
    fill(message);
    const thread: worker.ThreadWorker | undefined = this.thread;
    if (thread === undefined || this.disposed) {
      return;
    }
    try {
      thread.postMessage(message, []);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'input control post failed: %{public}s', JSON.stringify(error));
    }
  }

  private start(): void {
    const thread: worker.ThreadWorker =
      new worker.ThreadWorker('entry/ets/workers/input.worker.ets', { name: 'hmrdp-input' });
    thread.onmessage = (e: MessageEvents): void => {
      const telemetry: InputTelemetry = e.data as InputTelemetry;
      if (telemetry.control === InputControl.TELEMETRY) {
        this.rejects = telemetry.rejected;
      }
    };
    thread.onerror = (e: ErrorEvent): void => {
      hilog.error(DOMAIN, TAG, 'input worker error: %{public}s', e.message);
    };
    this.thread = thread;
    const config: InputConfig = {
      protocol: this.protocol, handle: this.handle, hostScalePercent: this.hostScale,
      moveIntervalMs: this.moveIntervalMs, referenceWidth: this.referenceWidth,
      referenceHeight: this.referenceHeight
    };
    this.control(InputControl.CONFIGURE, (message: InputCommand): void => {
      message.config = config;
    });
  }
}
