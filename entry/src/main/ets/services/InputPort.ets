import { ErrorEvent, MessageEvents, worker } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { KeyMapping } from '../utils/KeyMapper';
import { GamepadSink, InputSink, packTouchpadFrame, TOUCHPAD_FRAME_STRIDE, TouchpadContact, TouchPhase } from './input/InputSink';
import { TOUCH_MOVE_INTERVAL_FALLBACK_MS } from './input/InputPolicy';
import { InputArrival, InputCommand, InputConfig, InputControl, InputDelivery, InputKey, InputOp,
  INPUT_OP_SLOTS, InputPad, InputProtocol, InputRecord, InputTelemetry, inputDelivery, inputTextRun,
  inputWindowMs } from './InputProtocol';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpInput';

const MAX_PADS: number = 4;

// L1 side of the input pipeline: the controllers' sink, and the pipeline's only limiter. A verb
// becomes a queued record for the input worker; the queries answer from local state. Every window /
// rate cap / drop lives here, driven by the declaration table in InputProtocol
// (INPUT_PIPELINE_PLAN.md §3).
export class SessionInputPort implements InputSink, GamepadSink {
  private thread: worker.ThreadWorker | undefined = undefined;
  private protocol: number;
  private handle: number;
  private hostScale: number;
  private touchMoveWindowMs: number = TOUCH_MOVE_INTERVAL_FALLBACK_MS;
  private textRun: boolean;
  private relative: boolean = false;
  private lockFlags: number = 0;
  private lockKnown: boolean = false;
  private referenceWidth: number = 0;
  private referenceHeight: number = 0;
  private readonly drops: number[] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  private rejects: number[] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  private readonly padSlots: InputPad[] = [];
  // Windowed streams, indexed by op: the pending sample(s) and the stream's window timer.
  private readonly pending: (Map<number, InputRecord> | undefined)[] = [];
  private readonly timers: number[] = [];
  // Last accepted time per rate-capped stream.
  private readonly lastAcceptedAt: number[] = [];
  private textRunText: string = '';
  private textRunTimer: number = -1;
  private disposed: boolean = false;

  constructor(protocol: number, handle: number, hostScalePercent: number) {
    this.protocol = protocol;
    this.handle = handle;
    this.hostScale = hostScalePercent;
    this.textRun = inputTextRun(protocol);
    for (let i = 0; i < MAX_PADS; i++) {
      this.padSlots.push(SessionInputPort.newPad());
    }
    for (let i = 0; i < INPUT_OP_SLOTS; i++) {
      this.pending.push(undefined);
      this.timers.push(-1);
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

  // The touch-move window is a session setting; it changes the stream's declared budget in place.
  setTouchMoveInterval(ms: number): void {
    this.touchMoveWindowMs = ms;
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
    for (let i = 0; i < INPUT_OP_SLOTS; i++) {
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
    this.clearPending();
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
    return this.enqueue({ op: InputOp.KEY, key: SessionInputPort.keyOf(mapping), b: down ? 1 : 0 }, 0);
  }

  keyRepeat(mapping: KeyMapping): boolean {
    return this.enqueue({ op: InputOp.KEY_REPEAT, key: SessionInputPort.keyOf(mapping), b: 1 }, 0);
  }

  metaKey(down: boolean): boolean {
    return this.enqueue({ op: InputOp.META, b: down ? 1 : 0 }, 0);
  }

  unicode(code: number, down: boolean): boolean {
    if (!this.textRun) {
      return this.enqueue({ op: InputOp.UNICODE, a: code, b: down ? 1 : 0 }, 0);
    }
    // Only presses carry text; a run ends on its window or on the next non-text event.
    if (down && code > 0) {
      this.textRunText += String.fromCharCode(code);
      if (this.textRunTimer === -1) {
        this.textRunTimer = setTimeout((): void => {
          this.textRunTimer = -1;
          this.flushText();
        }, inputWindowMs(InputOp.TEXT_RUN, this.protocol, this.touchMoveWindowMs));
      }
    }
    return true;
  }

  lockState(flags: number): boolean {
    this.lockFlags = flags;
    this.lockKnown = true;
    return this.enqueue({ op: InputOp.LOCK, a: flags }, 0);
  }

  pointerMove(x: number, y: number): boolean {
    return this.enqueue({ op: InputOp.MOVE_ABS, x: x, y: y }, 0);
  }

  pointerMoveRelative(deltaX: number, deltaY: number): boolean {
    return this.enqueue({ op: InputOp.MOVE_REL, x: deltaX, y: deltaY }, 0);
  }

  button(action: number, button: number, x: number, y: number): boolean {
    return this.enqueue({ op: InputOp.BUTTON, a: action, b: button, x: x, y: y }, 0);
  }

  // Down/up/cancel are state transitions: they never enter a window, and a move waits in the
  // contact's own slot so a sequence can never be reordered past its own points.
  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): boolean {
    const op: number = phase === TouchPhase.MOVE ? InputOp.TOUCH_MOVE : InputOp.TOUCH_POINT;
    return this.enqueue({ op: op, a: phase, b: pointerId, x: x, y: y, z: pressure }, pointerId);
  }

  touchpadFrame(contacts: TouchpadContact[], buttonState: number, widthMm: number,
    heightMm: number): boolean {
    // A frame whose contacts only move is a continuous sample; one that carries down/up/cancel is a
    // state transition (never merged away), exactly like the touch stream's two ops.
    const frame: number[] = packTouchpadFrame(contacts);
    const op: number = SessionInputPort.touchpadOp(frame);
    const record: InputRecord = {
      op: op, frame: frame, a: buttonState, c: widthMm, d: heightMm
    };
    return this.enqueue(record, 0);
  }

  scroll(vertical: number, horizontal: number, x: number, y: number): boolean {
    return this.enqueue({ op: InputOp.SCROLL, a: vertical, b: horizontal, x: x, y: y }, 0);
  }

  // ---- GamepadSink: a slot's whole state is a continuous stream (LATEST_WINS) ----

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
    this.enqueue({ op: InputOp.GAMEPAD_STATE, pad: pad }, controllerNumber);
  }

  // The arrival declaration is a state transition (it must not be merged away).
  controllerArrival(controllerNumber: number, activeGamepadMask: number, type: number,
    supportedButtonFlags: number, capabilities: number): void {
    const arrival: InputArrival = {
      controllerNumber: controllerNumber, activeGamepadMask: activeGamepadMask, type: type,
      supportedButtonFlags: supportedButtonFlags, capabilities: capabilities
    };
    this.enqueue({ op: InputOp.GAMEPAD_ARRIVAL, arrival: arrival }, 0);
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

  // The declared class decides the gate. A windowed stream keeps at most one pending sample per key
  // (so the queue length is pinned by the stream count, not the event rate) and ends its window on a
  // timer, not a poll; a window of 0 posts straight through, which keeps call order. BOUNDED_REPEAT
  // drops above its rate cap — never deferred, since waiting is the latency the order rule forbids.
  private enqueue(record: InputRecord, key: number): boolean {
    const op: number = record.op;
    // Text is a sequence interleaved with everything else: any other event flushes the run first.
    if (op !== InputOp.UNICODE) {
      this.flushText();
    }
    const delivery: number = inputDelivery(op);
    if (delivery === InputDelivery.ORDERED_RELIABLE) {
      // A discrete event never enters a window: flush every pending windowed value first, so the
      // remote sees the app's order (the old flushMove(), now in one place).
      this.flushWindows();
      return this.send(record);
    }
    if (delivery === InputDelivery.BOUNDED_REPEAT) {
      const windowMs: number = inputWindowMs(op, this.protocol, this.touchMoveWindowMs);
      const now: number = Date.now();
      const last: number = this.lastAcceptedAt[op];
      if (last >= 0 && now - last < windowMs) {
        this.bump(this.drops, op);
        return false;
      }
      this.lastAcceptedAt[op] = now;
      return this.send(record);
    }
    if (delivery === InputDelivery.COALESCE_WINDOW) {
      return this.send(record);
    }
    const windowMs: number = inputWindowMs(op, this.protocol, this.touchMoveWindowMs);
    if (windowMs <= 0) {
      return this.send(record);
    }
    let slot: Map<number, InputRecord> | undefined = this.pending[op];
    if (slot === undefined) {
      slot = new Map<number, InputRecord>();
      this.pending[op] = slot;
    }
    slot.set(key, record);
    if (this.timers[op] === -1) {
      this.timers[op] = setTimeout((): void => {
        this.timers[op] = -1;
        this.flushOp(op);
      }, windowMs);
    }
    return true;
  }

  private flushOp(op: number): void {
    if (this.timers[op] !== -1) {
      clearTimeout(this.timers[op]);
      this.timers[op] = -1;
    }
    const slot: Map<number, InputRecord> | undefined = this.pending[op];
    if (slot === undefined || slot.size === 0) {
      return;
    }
    const records: InputRecord[] = [];
    slot.forEach((record: InputRecord): void => {
      records.push(record);
    });
    slot.clear();
    for (let i = 0; i < records.length; i++) {
      this.send(records[i]);
    }
  }

  private flushWindows(): void {
    for (let op = 0; op < INPUT_OP_SLOTS; op++) {
      this.flushOp(op);
    }
  }

  private flushText(): void {
    if (this.textRunTimer !== -1) {
      clearTimeout(this.textRunTimer);
      this.textRunTimer = -1;
    }
    const text: string = this.textRunText;
    this.textRunText = '';
    if (text.length === 0) {
      return;
    }
    this.send({ op: InputOp.TEXT_RUN, text: text });
  }

  private clearPending(): void {
    for (let op = 0; op < INPUT_OP_SLOTS; op++) {
      if (this.timers[op] !== -1) {
        clearTimeout(this.timers[op]);
        this.timers[op] = -1;
      }
      const slot: Map<number, InputRecord> | undefined = this.pending[op];
      if (slot !== undefined) {
        slot.clear();
      }
    }
    if (this.textRunTimer !== -1) {
      clearTimeout(this.textRunTimer);
      this.textRunTimer = -1;
    }
    this.textRunText = '';
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

  private static touchpadOp(frame: number[]): number {
    for (let i = 0; i + TOUCHPAD_FRAME_STRIDE <= frame.length; i += TOUCHPAD_FRAME_STRIDE) {
      if (frame[i] !== TouchPhase.MOVE) {
        return InputOp.TOUCHPAD_POINT;
      }
    }
    return InputOp.TOUCHPAD_FRAME;
  }

  private static newPad(): InputPad {
    return {
      controllerNumber: 0, activeGamepadMask: 0, buttonFlags: 0, leftTrigger: 0, rightTrigger: 0,
      leftStickX: 0, leftStickY: 0, rightStickX: 0, rightStickY: 0
    };
  }

  private bump(counts: number[], op: number): void {
    if (op < 0 || op >= INPUT_OP_SLOTS) {
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
      protocol: this.protocol, handle: this.handle, referenceWidth: this.referenceWidth,
      referenceHeight: this.referenceHeight
    };
    this.control(InputControl.CONFIGURE, (message: InputCommand): void => {
      message.config = config;
    });
  }
}
