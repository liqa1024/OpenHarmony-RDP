/*
 * HmRdp - protocol-neutral session backend.
 *
 * SessionManager is the single lifecycle authority, but it must not know which
 * native client it drives. A SessionBackend is the seam: RDP is the first
 * implementation (RdpSessionBackend, over RdpNative); a Moonlight backend plugs
 * in with the native Moonlight session without touching the manager. The manager
 * only sees lifecycle signals and the protocol-neutral input verbs; a
 * protocol-specific session window downcasts to its own backend for the rest.
 */
import { RdpEventPayload } from 'libhmrdp.so';
import {
  PendingConnection, PROTOCOL_MOONLIGHT, PROTOCOL_RDP, RdpConnectOptions, RdpEvent
} from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { RdpNative } from './RdpNative';

export type SessionSignalListener = (signal: SessionSignal) => void;

/**
 * What SessionManager needs from one session, independent of its protocol. The
 * input verbs are the protocol-neutral core; a session window that needs more
 * (clipboard, cursor, telemetry) downcasts to its own backend.
 */
export interface SessionBackend {
  protocol(): number;
  /** Creates the native session; returns its handle (>0) or -1. */
  start(): number;
  /** Tears the native session down and drops the listener (idempotent). */
  stop(): void;
  /** Native session handle (-1 before start / after stop). */
  handleId(): number;
  /**
   * True when start() needs the session window with its surface already bound
   * (Moonlight: LiStartConnection takes the render target). False for RDP, which
   * starts window-less and binds the surface when the window opens.
   */
  requiresWindowAtStart(): boolean;
  /** Binds the session window surface; false when it could not be created. */
  bindSurface(surfaceId: string, width: number, height: number): boolean;
  /** Detaches/releases the session window surface (after stop). */
  clearSurface(): void;
  setSignalListener(listener: SessionSignalListener): void;
  clearSignalListener(): void;
  pointer(flags: number, x: number, y: number): void;
  touch(flags: number, finger: number, pressure: number, x: number, y: number): void;
  key(scancode: number, down: boolean, extended: boolean): void;
  unicode(codepoint: number, down: boolean): void;
}

/** Maps a session key to the backend that owns it (one per open window). */
export class SessionBackendRegistry {
  private static byKey: Map<string, SessionBackend> = new Map<string, SessionBackend>();

  static register(key: string, backend: SessionBackend): void {
    SessionBackendRegistry.byKey.set(key, backend);
  }

  static find(key: string): SessionBackend | undefined {
    return SessionBackendRegistry.byKey.get(key);
  }

  static unregister(key: string): void {
    SessionBackendRegistry.byKey.delete(key);
  }
}

/** The RDP session backend: wraps one RdpNative instance. */
export class RdpSessionBackend implements SessionBackend {
  // RDP-only operations the session window still calls directly.
  readonly native: RdpNative = new RdpNative();
  private listener: SessionSignalListener | undefined = undefined;
  private options: RdpConnectOptions;

  constructor(options: RdpConnectOptions) {
    this.options = options;
  }

  protocol(): number {
    return PROTOCOL_RDP;
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

  /** Maps an RDP native event onto the protocol-neutral envelope. */
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

/** Creates the backend for a connection, or undefined when the protocol has none. */
export class SessionBackendFactory {
  static create(request: PendingConnection): SessionBackend | undefined {
    if (request.protocol === PROTOCOL_MOONLIGHT) {
      // MoonlightSessionBackend is ready (see MoonlightSessionBackend.ets) but
      // is not handed out until the Moonlight session window lands: the manager
      // opens the window before start for it (requiresWindowAtStart), and the
      // window is what binds the surface and calls SessionManager.beginStream.
      // Returning it now would open an RDP page with nothing to start it.
      return undefined;
    }
    return new RdpSessionBackend(request.options);
  }
}
