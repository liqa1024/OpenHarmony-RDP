import { RdpEventPayload } from 'libhmrdp.so';
import { RdpConnectOptions, RdpEvent, RdpPointerFlags, RdpTouchFlags } from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { KeyMapping } from '../utils/KeyMapper';
import { RdpNative } from './RdpNative';
import { SessionBackend, SessionSignalListener } from './SessionBackend';
import { InputAction, InputButton, InputSink, TouchpadContact, TouchPhase } from './SessionInput';

export class RdpSessionBackend implements SessionBackend {
  readonly native: RdpNative = new RdpNative();
  private listener: SessionSignalListener | undefined = undefined;
  private options: RdpConnectOptions;

  constructor(options: RdpConnectOptions) {
    this.options = options;
  }

  pageRoute(): string {
    return 'pages/SessionPage';
  }

  requiresPrepare(): boolean {
    return false;
  }

  prepare(): Promise<boolean> {
    return Promise.resolve(true);
  }

  start(): number {
    this.native.setListener((event: RdpEvent, payload: RdpEventPayload): void => {
      this.emit(event, payload);
    });
    return this.native.startSession(this.options);
  }

  stop(): void {
    this.native.stopSession();
    this.clearSignalListener();
  }

  handleId(): number {
    return this.native.handleId();
  }

  requiresWindowAtStart(): boolean {
    return false;
  }

  bindSurface(surfaceId: string, width: number, height: number): boolean {
    this.native.bindSurface(surfaceId, width, height);
    return true;
  }

  clearSurface(): void {
    this.native.unbindSurface();
  }

  setShortcutPassthrough(enabled: boolean): boolean {
    return this.native.setShortcutPassthrough(enabled);
  }

  setSignalListener(listener: SessionSignalListener): void {
    this.listener = listener;
  }

  clearSignalListener(): void {
    this.listener = undefined;
  }

  pointer(flags: number, x: number, y: number): void {
    this.native.mouse(flags, x, y);
  }

  touch(flags: number, finger: number, pressure: number, x: number, y: number): void {
    this.native.touch(flags, finger, pressure, x, y);
  }

  key(scancode: number, down: boolean, extended: boolean): void {
    this.native.key(scancode, down, extended);
  }

  unicode(codepoint: number, down: boolean): void {
    this.native.unicode(codepoint, down);
  }

  private emit(event: RdpEvent, payload: RdpEventPayload): void {
    const listener: SessionSignalListener | undefined = this.listener;
    if (listener === undefined) {
      return;
    }
    const signal: SessionSignal = new SessionSignal();
    signal.eventCode = event;
    signal.payload = payload;
    if (event === RdpEvent.Connected) {
      signal.kind = SessionSignalKind.Connected;
    } else if (event === RdpEvent.Disconnected) {
      signal.kind = SessionSignalKind.Disconnected;
      signal.message = payload.message ?? '';
      signal.code = payload.code ?? 0;
    } else if (event === RdpEvent.Error) {
      signal.kind = SessionSignalKind.Error;
      signal.message = payload.message ?? '';
      signal.code = payload.code ?? 0;
    } else {
      signal.kind = SessionSignalKind.UiEvent;
    }
    listener(signal);
  }
}

// Largest wheel rotation a single RDP pointer event can carry (9-bit wheel field).
const RDP_WHEEL_MAX_CHUNK: number = 0xFF;

// RDP landing of the protocol-neutral input verbs: PS/2 scancodes, RDP pointer
// flags, RDPEI touches, and 9-bit wheel chunks. RDP has no native touchpad endpoint.
export class RdpInputSink implements InputSink {
  private native: RdpNative;
  private lastX: number = 0;
  private lastY: number = 0;
  private lockFlags: number = 0;
  private lockKnown: boolean = false;

  constructor(native: RdpNative) {
    this.native = native;
  }

  lastLockFlags(): number {
    return this.lockFlags;
  }

  hasLockState(): boolean {
    return this.lockKnown;
  }

  key(mapping: KeyMapping, down: boolean): void {
    this.native.key(mapping.scancode, down, mapping.extended);
  }

  metaKey(down: boolean): void {
    this.native.winKey(down);
  }

  unicode(code: number, down: boolean): void {
    this.native.unicode(code, down);
  }

  lockState(flags: number): void {
    this.lockFlags = flags;
    this.lockKnown = true;
    this.native.lockState(flags);
  }

  pointerMove(x: number, y: number): void {
    this.lastX = x;
    this.lastY = y;
    this.native.mouse(RdpPointerFlags.MOVE, x, y);
  }

  button(action: number, button: number, x: number, y: number): void {
    this.lastX = x;
    this.lastY = y;
    const flag: number = RdpInputSink.buttonFlag(button);
    if (flag === 0) {
      return;
    }
    if (action === InputAction.PRESS) {
      this.native.mouse(RdpPointerFlags.MOVE, x, y);
      this.native.mouse(flag | RdpPointerFlags.DOWN, x, y);
    } else {
      this.native.mouse(flag, x, y);
    }
  }

  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): void {
    let flags: number = RdpTouchFlags.MOTION;
    if (phase === TouchPhase.DOWN) {
      flags = RdpTouchFlags.DOWN;
    } else if (phase === TouchPhase.UP) {
      flags = RdpTouchFlags.UP;
    }
    const scaled: number = pressure <= 0 ? 0 : Math.max(1, Math.min(1024, Math.round(pressure * 1024)));
    if (scaled > 0) {
      flags |= RdpTouchFlags.HAS_PRESSURE;
    }
    this.native.touch(flags, pointerId, scaled, x, y);
  }

  touchpadFrame(_contacts: TouchpadContact[], _buttonState: number, _widthMm: number,
    _heightMm: number): void {
    // RDP has no native touchpad endpoint.
  }

  scroll(vertical: number, horizontal: number, x: number, y: number): void {
    this.lastX = x;
    this.lastY = y;
    this.sendWheel(vertical, false, x, y);
    this.sendWheel(horizontal, true, x, y);
  }

  private sendWheel(amount: number, horizontal: boolean, x: number, y: number): void {
    let remaining: number = amount;
    while (Math.abs(remaining) >= 1) {
      const magnitude: number = Math.min(Math.floor(Math.abs(remaining)), RDP_WHEEL_MAX_CHUNK);
      if (magnitude < 1) {
        break;
      }
      const negative: boolean = remaining < 0;
      // Negative rotation is 9-bit two's complement: magnitude m -> WHEEL_NEGATIVE | (0x100 - m).
      let flags: number = horizontal ? RdpPointerFlags.HWHEEL : RdpPointerFlags.WHEEL;
      flags |= negative
        ? RdpPointerFlags.WHEEL_NEGATIVE | (0x100 - magnitude)
        : magnitude;
      this.native.mouse(flags, x, y);
      remaining -= negative ? -magnitude : magnitude;
    }
  }

  private static buttonFlag(button: number): number {
    if (button === InputButton.LEFT) {
      return RdpPointerFlags.BUTTON1;
    }
    if (button === InputButton.RIGHT) {
      return RdpPointerFlags.BUTTON2;
    }
    if (button === InputButton.MIDDLE) {
      return RdpPointerFlags.BUTTON3;
    }
    return 0;
  }
}
