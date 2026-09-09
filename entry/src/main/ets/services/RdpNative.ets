/*
 * HmRdp - thin ArkTS facade over the libhmrdp.so Node-API module.
 *
 * One RdpNative instance owns exactly one native session, so several session
 * windows can run concurrently. Native events carry the session handle and are
 * routed back to the owning instance.
 */
import { clearSurface, connect, createSession, destroySession, disconnect,
  getVersion, isConnected, onEvent, requestResize, sendKey, sendMouse, sendTouch,
  sendUnicode, setClipboardText, setSurface, updateSurface,
  RdpEventCallback } from 'libhmrdp.so';
import { RdpConnectOptions, RdpEvent } from '../model/RdpModels';

export type RdpEventListener = (event: RdpEvent, data: string) => void;

const instances: Map<number, RdpNative> = new Map<number, RdpNative>();
const byKey: Map<string, RdpNative> = new Map<string, RdpNative>();
let eventBridgeReady: boolean = false;

function ensureEventBridge(): void {
  if (eventBridgeReady) {
    return;
  }
  eventBridgeReady = true;
  const callback: RdpEventCallback = (handle: number, event: number, data: string): void => {
    const target: RdpNative | undefined = instances.get(handle);
    if (target !== undefined) {
      target.dispatch(event as RdpEvent, data);
    }
  };
  onEvent(callback);
}

export class RdpNative {
  private handle: number = -1;
  private listener: RdpEventListener | undefined = undefined;

  /** Associates an instance with the window's session key for teardown. */
  static register(key: string, native: RdpNative): void {
    byKey.set(key, native);
  }

  /** Returns the instance registered for a session key, if any. */
  static findByKey(key: string): RdpNative | undefined {
    return byKey.get(key);
  }

  /** Stops only the session owned by the given key (window close / destroy). */
  static stopByKey(key: string): void {
    const target: RdpNative | undefined = byKey.get(key);
    if (target !== undefined) {
      target.stopSession();
      target.clearListener();
    }
    byKey.delete(key);
  }

  version(): string {
    return getVersion();
  }

  setListener(listener: RdpEventListener): void {
    this.listener = listener;
    ensureEventBridge();
  }

  clearListener(): void {
    this.listener = undefined;
  }

  dispatch(event: RdpEvent, data: string): void {
    const current: RdpEventListener | undefined = this.listener;
    if (current !== undefined) {
      current(event, data);
    }
  }

  startSession(options: RdpConnectOptions): number {
    this.stopSession();
    ensureEventBridge();
    this.handle = createSession();
    if (this.handle < 0) {
      return -1;
    }
    instances.set(this.handle, this);
    const started: boolean = connect(this.handle, options);
    if (!started) {
      this.stopSession();
    }
    return this.handle;
  }

  stopSession(): void {
    if (this.handle < 0) {
      return;
    }
    instances.delete(this.handle);
    disconnect(this.handle);
    destroySession(this.handle);
    this.handle = -1;
  }

  bindSurface(surfaceId: string, width: number, height: number): void {
    if (this.handle >= 0) {
      setSurface(this.handle, surfaceId, width, height);
    }
  }

  resizeSurface(width: number, height: number): void {
    if (this.handle >= 0) {
      updateSurface(this.handle, width, height);
    }
  }

  unbindSurface(): void {
    if (this.handle >= 0) {
      clearSurface(this.handle);
    }
  }

  connected(): boolean {
    return this.handle >= 0 && isConnected(this.handle);
  }

  mouse(flags: number, x: number, y: number): void {
    if (this.handle >= 0) {
      sendMouse(this.handle, flags, x, y);
    }
  }

  touch(flags: number, finger: number, pressure: number, x: number, y: number): void {
    if (this.handle >= 0) {
      sendTouch(this.handle, flags, finger, pressure, x, y);
    }
  }

  key(scancode: number, down: boolean, extended: boolean): void {
    if (this.handle >= 0) {
      sendKey(this.handle, scancode, down, extended);
    }
  }

  unicode(codepoint: number, down: boolean): void {
    if (this.handle >= 0) {
      sendUnicode(this.handle, codepoint, down);
    }
  }

  /** Pushes the local clipboard text (UTF-8) to the remote session. */
  setClipboard(text: string): void {
    if (this.handle >= 0) {
      setClipboardText(this.handle, text);
    }
  }

  resize(width: number, height: number): void {
    if (this.handle >= 0) {
      requestResize(this.handle, width, height);
    }
  }
}
