import { clearSurface, connect, createSession, destroySession, disconnect,
  sendKey, sendMouse, sendTouch,
  sendUnicode, setRdpCursor, setSurface,
setSuppressed as setSuppressedNative, setTouchHighRate, updateSurface, setHardwareAccel,
setShortcutPassthrough as setShortcutPassthroughNative,
setGfxCapture,
setClipboardFileOffer, requestRemoteFileContents,
cancelFileTransfer, sendLockState, sendSynchronize, requestRemoteFormat,
RdpEventPayload, RdpTelemetry, sessionTelemetry,
bufferRead, bufferRelease,
startReplay, stopReplay, stopAllReplays, resizeReplay, replayStats, listReplays
} from 'libhmrdp.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RdpConnectOptions, RdpEvent } from '../model/RdpModels';
import { RdpCounters, RdpTelemetryAggregator } from './RdpTelemetry';
import { RdpEventHub } from './RdpEventHub';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpNative';

export type RdpEventListener = (event: RdpEvent, payload: RdpEventPayload) => void;

export class RdpNative {
  private handle: number = -1;
  private listener: RdpEventListener | undefined = undefined;
  private telemetry: RdpTelemetryAggregator = new RdpTelemetryAggregator();

  static applyTouchHighRate(enabled: boolean): void {
    setTouchHighRate(enabled);
  }

  static applyRdpCursor(enabled: boolean): void {
    setRdpCursor(enabled);
  }

  static applyHardwareAccel(enabled: boolean): void {
    setHardwareAccel(enabled);
  }

  static applyGfxCapture(enabled: boolean, dir: string): void {
    setGfxCapture(enabled, dir);
  }

  static startReplayInstance(surfaceId: string, surfaceW: number, surfaceH: number,
    gfxPath: string, realtime: boolean, refMode: number): number {
    const handle: number = startReplay(surfaceId, surfaceW, surfaceH, gfxPath,
      realtime ? 1 : 0, refMode);
    hilog.info(DOMAIN, TAG, 'replay start: handle=%{public}d', handle);
    return handle;
  }

  static stopReplayInstance(handle: number): void {
    if (handle > 0) {
      stopReplay(handle);
    }
  }

  static stopAllReplayInstances(): void {
    stopAllReplays();
  }

  static readAndReleaseBuffer(handle: number): ArrayBuffer | undefined {
    if (handle <= 0) {
      return undefined;
    }
    try {
      return bufferRead(handle);
    } finally {
      bufferRelease(handle);
    }
  }

  static releaseBuffer(handle: number): void {
    if (handle > 0) {
      bufferRelease(handle);
    }
  }

  static resizeReplayInstance(handle: number, width: number, height: number): void {
    if (handle > 0) {
      resizeReplay(handle, width, height);
    }
  }

  static replayInstanceStats(handle: number): string {
    return replayStats(handle);
  }

  static listReplayInstances(): string {
    const summary: string = listReplays();
    hilog.info(DOMAIN, TAG, 'replay list: %{public}s', summary);
    return summary;
  }

  setListener(listener: RdpEventListener): void {
    this.listener = listener;
  }

  clearListener(): void {
    this.listener = undefined;
  }

  dispatch(event: RdpEvent, payload: RdpEventPayload): void {
    const current: RdpEventListener | undefined = this.listener;
    if (current !== undefined) {
      current(event, payload);
    }
  }

  handleId(): number {
    return this.handle;
  }

  startSession(options: RdpConnectOptions): number {
    this.teardownHandle();
    this.handle = createSession();
    if (this.handle < 0) {
      return -1;
    }
    RdpEventHub.register(this.handle, (event: RdpEvent, payload: RdpEventPayload): void => {
      this.dispatch(event, payload);
    });
    const started: boolean = connect(this.handle, options);
    if (!started) {
      this.stopSession();
    }
    return this.handle;
  }

  stopSession(): void {
    this.teardownHandle();
  }

  private teardownHandle(): void {
    if (this.handle < 0) {
      return;
    }
    RdpEventHub.unregister(this.handle);
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

  setSuppressed(suppressed: boolean): void {
    if (this.handle >= 0) {
      setSuppressedNative(this.handle, suppressed);
    }
  }

  readCounters(): RdpCounters | undefined {
    if (this.handle < 0) {
      return undefined;
    }
    const raw: RdpTelemetry = sessionTelemetry(this.handle);
    return this.telemetry.sample(raw);
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

  lockState(flags: number): void {
    if (this.handle >= 0) {
      sendLockState(this.handle, flags);
    }
  }

  reassertLockState(flags: number): boolean {
    if (this.handle < 0) {
      return false;
    }
    return sendSynchronize(this.handle, flags);
  }

  winKey(down: boolean): void {
    if (this.handle >= 0) {
      sendKey(this.handle, 0x5B, down, true);
    }
  }

  setShortcutPassthrough(enabled: boolean): boolean {
    if (this.handle < 0) {
      return false;
    }
    return setShortcutPassthroughNative(this.handle, enabled);
  }

  pushClipboardFileOffer(descriptor: ArrayBuffer, paths: string[]): void {
    if (this.handle >= 0 && paths.length > 0) {
      setClipboardFileOffer(this.handle, descriptor, paths.join('\n'));
    }
  }

  requestRemoteFileContents(listIndex: number, sizeOnly: boolean, offset: number,
    length: number): boolean {
    if (this.handle < 0) {
      return false;
    }
    return requestRemoteFileContents(this.handle, listIndex, sizeOnly, offset, length);
  }

  requestClipboardFormat(formatId: number, kind: number): boolean {
    if (this.handle < 0) {
      return false;
    }
    return requestRemoteFormat(this.handle, formatId, kind);
  }

  cancelFileTransfer(): void {
    if (this.handle >= 0) {
      cancelFileTransfer(this.handle);
    }
  }
}
