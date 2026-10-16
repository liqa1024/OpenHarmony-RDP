import { MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import {
  moonlightController, moonlightControllerArrival, moonlightHighResHScroll, moonlightHighResScroll,
  moonlightKey, moonlightMouseButton, moonlightMouseMove, moonlightMousePosition, moonlightText,
  moonlightTouch, moonlightTouchpad
} from 'libhmrdp.so';
import { KeyMapping } from '../utils/KeyMapper';
import { MlVirtualKey } from '../utils/MoonlightKeyMapper';
import { GamepadSink, InputSender, MoonlightSendPort, TextRunSink } from '../services/input/InputSink';
import { MoonlightInputSender } from '../services/input/MoonlightInputSender';
import { RdpInputSender } from '../services/input/RdpInputSender';
import { INPUT_TEXT_RUN_WINDOW_MS, InputCommand, InputControl, InputOp, InputProtocol,
  InputRecord, InputTelemetry } from '../services/InputProtocol';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

const OP_SLOTS: number = 16;

// The worker loads the same .so the UI runtime did, so these reach the live session with no handle
// of their own.
class MoonlightNativePort implements MoonlightSendPort {
  keyVirtual(vk: number, down: boolean, modifiers: number): number {
    return moonlightKey(vk, down ? MlVirtualKey.KEY_DOWN : MlVirtualKey.KEY_UP, modifiers);
  }

  mousePosition(x: number, y: number, refWidth: number, refHeight: number): number {
    return moonlightMousePosition(x, y, refWidth, refHeight);
  }

  mouseMove(deltaX: number, deltaY: number): number {
    return moonlightMouseMove(deltaX, deltaY);
  }

  mouseButton(action: number, button: number): number {
    return moonlightMouseButton(action, button);
  }

  text(text: string): number {
    return moonlightText(text);
  }

  touch(eventType: number, pointerId: number, x: number, y: number, pressure: number): number {
    return moonlightTouch(eventType, pointerId, x, y, pressure);
  }

  highResScroll(amount: number): number {
    return moonlightHighResScroll(amount);
  }

  highResHScroll(amount: number): number {
    return moonlightHighResHScroll(amount);
  }

  touchpad(eventTypes: number[], pointerIds: number[], xs: number[], ys: number[], ps: number[],
    widthMm: number, heightMm: number, buttonState: number): number {
    return moonlightTouchpad(eventTypes, pointerIds, xs, ys, ps, widthMm, heightMm, buttonState);
  }

  controller(controllerNumber: number, activeGamepadMask: number, buttonFlags: number,
    leftTrigger: number, rightTrigger: number, leftStickX: number, leftStickY: number,
    rightStickX: number, rightStickY: number): number {
    return moonlightController(controllerNumber, activeGamepadMask, buttonFlags, leftTrigger,
      rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
  }

  controllerArrival(controllerNumber: number, activeGamepadMask: number, type: number,
    supportedButtonFlags: number, capabilities: number): number {
    return moonlightControllerArrival(controllerNumber, activeGamepadMask, type,
      supportedButtonFlags, capabilities);
  }
}

let sender: InputSender | undefined = undefined;
let gamepad: GamepadSink | undefined = undefined;
// Set only where the transport has a text-run packet (see TextRunSink).
let textSender: TextRunSink | undefined = undefined;
let rdpSender: RdpInputSender | undefined = undefined;
let moonlightSender: MoonlightInputSender | undefined = undefined;
const rejected: number[] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
let telemetryTimer: number = -1;
let pendingReport: boolean = false;
let textRun: string = '';
let textTimer: number = -1;

// A text run is the pipeline's COALESCE_WINDOW: code points inside the window leave as one packet.
// Any other event flushes the run first, so the app's order is the host's order.
function flushText(): void {
  if (textTimer !== -1) {
    clearTimeout(textTimer);
    textTimer = -1;
  }
  const run: string = textRun;
  textRun = '';
  const target: TextRunSink | undefined = textSender;
  if (run.length === 0 || target === undefined) {
    return;
  }
  noteReject(InputOp.UNICODE, target.textRun(run));
}

function appendText(code: number): void {
  textRun += String.fromCharCode(code);
  if (textTimer === -1) {
    textTimer = setTimeout((): void => {
      textTimer = -1;
      flushText();
    }, INPUT_TEXT_RUN_WINDOW_MS);
  }
}

function noteReject(op: number, accepted: boolean): void {
  if (accepted || op < 0 || op >= OP_SLOTS) {
    return;
  }
  rejected[op]++;
  scheduleReport();
}

// Reported back at a low rate, so a dead UI thread is never written to per event.
function scheduleReport(): void {
  if (pendingReport) {
    return;
  }
  pendingReport = true;
  telemetryTimer = setTimeout((): void => {
    pendingReport = false;
    const message: InputTelemetry = { control: InputControl.TELEMETRY, rejected: rejected.slice() };
    workerPort.postMessage(message, []);
  }, 1000);
}

function dispatch(record: InputRecord): void {
  const target: InputSender | undefined = sender;
  if (target === undefined) {
    return;
  }
  const op: number = record.op;
  if (op !== InputOp.UNICODE) {
    flushText();
  }
  if (op === InputOp.KEY || op === InputOp.KEY_REPEAT) {
    const key = record.key;
    if (key !== undefined) {
      const mapping: KeyMapping = KeyMapping.of(key.scancode, key.vk, key.extended, key.lock);
      // A repeat is a key-down on the wire; the class only decided whether it was worth sending.
      noteReject(op, target.key(mapping, op === InputOp.KEY ? record.b === 1 : true));
    }
    return;
  }
  if (op === InputOp.META) {
    noteReject(op, target.metaKey(record.b === 1));
    return;
  }
  if (op === InputOp.UNICODE) {
    const code: number = record.a ?? 0;
    const down: boolean = record.b === 1;
    if (textSender !== undefined) {
      // Only presses carry text; a run is ended by its window or by the next non-text event.
      if (down && code > 0) {
        appendText(code);
      }
      return;
    }
    noteReject(op, target.unicode(code, down));
    return;
  }
  if (op === InputOp.LOCK) {
    noteReject(op, target.lockState(record.a ?? 0));
    return;
  }
  if (op === InputOp.MOVE_ABS) {
    noteReject(op, target.pointerMove(record.x ?? 0, record.y ?? 0));
    return;
  }
  if (op === InputOp.MOVE_REL) {
    noteReject(op, target.pointerMoveRelative(record.x ?? 0, record.y ?? 0));
    return;
  }
  if (op === InputOp.BUTTON) {
    noteReject(op, target.button(record.a ?? 0, record.b ?? 0, record.x ?? 0, record.y ?? 0));
    return;
  }
  if (op === InputOp.TOUCH) {
    noteReject(op, target.touch(record.a ?? 0, record.b ?? 0, record.x ?? 0, record.y ?? 0,
      record.z ?? 0));
    return;
  }
  if (op === InputOp.TOUCHPAD_FRAME) {
    const contacts = record.contacts;
    noteReject(op, target.touchpadFrame(contacts !== undefined ? contacts : [], record.a ?? 0,
      record.c ?? 0, record.d ?? 0));
    return;
  }
  if (op === InputOp.SCROLL) {
    noteReject(op, target.scroll(record.a ?? 0, record.b ?? 0, record.x ?? 0, record.y ?? 0));
    return;
  }
  if (op === InputOp.GAMEPAD_STATE) {
    const pad = record.pad;
    const padTarget: GamepadSink | undefined = gamepad;
    if (pad !== undefined && padTarget !== undefined) {
      padTarget.controllerState(pad.controllerNumber, pad.activeGamepadMask, pad.buttonFlags,
        pad.leftTrigger, pad.rightTrigger, pad.leftStickX, pad.leftStickY, pad.rightStickX,
        pad.rightStickY);
    }
    return;
  }
  if (op === InputOp.GAMEPAD_ARRIVAL) {
    const arrival = record.arrival;
    const padTarget: GamepadSink | undefined = gamepad;
    if (arrival !== undefined && padTarget !== undefined) {
      padTarget.controllerArrival(arrival.controllerNumber, arrival.activeGamepadMask, arrival.type,
        arrival.supportedButtonFlags, arrival.capabilities);
    }
  }
}

function configure(message: InputCommand): void {
  const config = message.config;
  if (config === undefined) {
    return;
  }
  if (telemetryTimer !== -1) {
    clearTimeout(telemetryTimer);
    telemetryTimer = -1;
    pendingReport = false;
  }
  flushText();
  if (config.protocol === InputProtocol.RDP) {
    const rdp: RdpInputSender = new RdpInputSender(config.handle);
    rdpSender = rdp;
    moonlightSender = undefined;
    textSender = undefined;
    gamepad = undefined;
    sender = rdp;
    return;
  }
  const port: MoonlightNativePort = new MoonlightNativePort();
  const ml: MoonlightInputSender = new MoonlightInputSender(port);
  ml.setReferenceSize(config.referenceWidth, config.referenceHeight);
  moonlightSender = ml;
  rdpSender = undefined;
  textSender = ml;
  gamepad = ml;
  sender = ml;
}

workerPort.onmessage = (e: MessageEvents): void => {
  const message: InputCommand = e.data as InputCommand;
  if (message.control !== undefined) {
    if (message.control === InputControl.CONFIGURE) {
      configure(message);
      return;
    }
    if (message.control === InputControl.SET_RELATIVE) {
      moonlightSender?.setRelativePointer(message.enabled ?? false);
      return;
    }
    if (message.control === InputControl.SET_REFERENCE_SIZE) {
      moonlightSender?.setReferenceSize(message.width ?? 0, message.height ?? 0);
      return;
    }
    if (message.control === InputControl.SET_HANDLE) {
      rdpSender?.setHandle(message.handle ?? -1);
      return;
    }
    return;
  }
  dispatch(e.data as InputRecord);
};
