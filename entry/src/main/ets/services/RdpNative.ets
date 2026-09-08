/*
 * HmRdp - thin ArkTS facade over the libhmrdp.so Node-API module.
 */
import { attachSurface, clearSurface, connect, createSession, destroySession,
  detachSurface, disconnect, getVersion, isConnected, onEvent, requestResize,
  sendKey, sendMouse, sendUnicode, setSurface, updateSurface, RdpEventCallback }
  from 'libhmrdp.so';
import { RdpConnectOptions, RdpEvent } from '../model/RdpModels';

export type RdpEventListener = (event: RdpEvent, data: string) => void;

export class RdpNative {
  private static instance: RdpNative | undefined = undefined;
  private handle: number = -1;
  private listener: RdpEventListener | undefined = undefined;
  private nativeListenerRegistered: boolean = false;

  static get(): RdpNative {
    if (RdpNative.instance === undefined) {
      RdpNative.instance = new RdpNative();
    }
    return RdpNative.instance;
  }

  version(): string {
    return getVersion();
  }

  setListener(listener: RdpEventListener): void {
    this.listener = listener;
    if (!this.nativeListenerRegistered) {
      const callback: RdpEventCallback = (event: number, data: string): void => {
        const current: RdpEventListener | undefined = this.listener;
        if (current !== undefined) {
          current(event as RdpEvent, data);
        }
      };
      onEvent(callback);
      this.nativeListenerRegistered = true;
    }
  }

  clearListener(): void {
    this.listener = undefined;
  }

  startSession(options: RdpConnectOptions): number {
    this.stopSession();
    this.handle = createSession();
    const started: boolean = connect(this.handle, options);
    if (!started) {
      destroySession(this.handle);
      this.handle = -1;
    }
    return this.handle;
  }

  stopSession(): void {
    if (this.handle < 0) {
      return;
    }
    disconnect(this.handle);
    destroySession(this.handle);
    this.handle = -1;
  }

  bindSurface(surfaceId: string, width: number, height: number): void {
    setSurface(surfaceId, width, height);
    if (this.handle >= 0) {
      attachSurface(this.handle);
    }
  }

  resizeSurface(width: number, height: number): void {
    updateSurface(width, height);
  }

  unbindSurface(): void {
    if (this.handle >= 0) {
      detachSurface(this.handle);
    }
    clearSurface();
  }

  connected(): boolean {
    return this.handle >= 0 && isConnected(this.handle);
  }

  mouse(flags: number, x: number, y: number): void {
    if (this.handle >= 0) {
      sendMouse(this.handle, flags, x, y);
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

  resize(width: number, height: number): void {
    if (this.handle >= 0) {
      requestResize(this.handle, width, height);
    }
  }
}
