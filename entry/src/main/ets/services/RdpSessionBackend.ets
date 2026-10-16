import { RdpEventPayload } from 'libhmrdp.so';
import { RdpConnectOptions, RdpEvent } from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { RdpNative } from './RdpNative';
import { SessionBackend, SessionSignalListener } from './SessionBackend';

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

