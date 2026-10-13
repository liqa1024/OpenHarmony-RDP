import { SessionSignal } from '../model/SessionSignal';

export type SessionSignalListener = (signal: SessionSignal) => void;

// Protocol-neutral session backend contract. Shared code (SessionManager, the
// session shell) only sees this interface; the RDP / Moonlight implementations
// live in their own files (RdpSessionBackend.ets / MoonlightSessionBackend.ets).
export interface SessionBackend {
  pageRoute(): string;
  requiresPrepare(): boolean;
  prepare(): Promise<boolean>;
  start(): number;
  stop(): void;
  handleId(): number;
  requiresWindowAtStart(): boolean;
  bindSurface(surfaceId: string, width: number, height: number): boolean;
  clearSurface(): void;
  // Process-global shortcut interceptor; returns whether the requested state is in effect.
  setShortcutPassthrough(enabled: boolean): boolean;
  setSignalListener(listener: SessionSignalListener): void;
  clearSignalListener(): void;
  pointer(flags: number, x: number, y: number): void;
  touch(flags: number, finger: number, pressure: number, x: number, y: number): void;
  key(scancode: number, down: boolean, extended: boolean): void;
  unicode(codepoint: number, down: boolean): void;
}

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
