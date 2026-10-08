/*
 * HmRdp - thin ArkTS facade over the libhmrdp.so Node-API module.
 *
 * One RdpNative instance owns exactly one native session, so several session
 * windows can run concurrently. Native events carry the session handle and are
 * routed back to the owning instance.
 */
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
import { RdpCounters, SessionTelemetry } from './SessionTelemetry';
import { SessionEventHub } from './SessionEventHub';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpNative';

export type RdpEventListener = (event: RdpEvent, payload: RdpEventPayload) => void;

export class RdpNative {
  private handle: number = -1;
  private listener: RdpEventListener | undefined = undefined;
  // This session's telemetry aggregation (native sends raw readings only).
  private telemetry: SessionTelemetry = new SessionTelemetry();

  /**
   * Process-global RDPEI pacing: forward touch contacts at the full input rate
   * instead of FreeRDP's 20ms (50Hz) coalescing. No-op unless the linked
   * FreeRDP carries the rdpei interval patch.
   */
  static applyTouchHighRate(enabled: boolean): void {
    setTouchHighRate(enabled);
  }

  /**
   * Process-global remote cursor switch. When off, the session leaves the
   * HarmonyOS default cursor untouched. Read at connect time, so it applies to
   * sessions opened afterwards.
   */
  static applyRdpCursor(enabled: boolean): void {
    setRdpCursor(enabled);
  }

  /** "硬件加速": present frames through Vulkan (on) or keep the session on the
   * GLES presenter with no Vulkan at all (off). Decoding is FreeRDP's gdi path
   * either way, so this only decides how frames reach the window. */
  static applyHardwareAccel(enabled: boolean): void {
    setHardwareAccel(enabled);
  }

  /** Dev-only raw GFX channel capture to the given directory. */
  static applyGfxCapture(enabled: boolean, dir: string): void {
    setGfxCapture(enabled, dir);
  }

  /**
   * Dev/test: start one replay instance on `surfaceId`; returns its handle (>0)
   * or -1. Instances are independent (each owns its own frame pipeline), so
   * several can run at once - the multi-session regression vehicle. Decoding is
   * FreeRDP's gdi pipeline and the presenter is the one the "硬件加速" setting
   * selects.
   * `realtime`: play at the capture's recorded arrival times instead of the
   * unthrottled fast mode (a capture without arrival times falls back to fast and
   * reports `mode=fast(untimed)`). `refMode`: golden reference - 0 = off (perf
   * run), 1 = record this run as the reference, 2 = check against the stored one.
   */
  static startReplayInstance(surfaceId: string, surfaceW: number, surfaceH: number,
    gfxPath: string, realtime: boolean, refMode: number): number {
    const handle: number = startReplay(surfaceId, surfaceW, surfaceH, gfxPath,
      realtime ? 1 : 0, refMode);
    hilog.info(DOMAIN, TAG, 'replay start: handle=%{public}d', handle);
    return handle;
  }

  /** Stops one replay instance and releases its surface. */
  static stopReplayInstance(handle: number): void {
    if (handle > 0) {
      stopReplay(handle);
    }
  }

  /** Stops every replay instance. */
  static stopAllReplayInstances(): void {
    stopAllReplays();
  }

  /**
   * Reads a native byte buffer handed over by an event (a cursor / clipboard
   * bitmap) as an ArrayBuffer copy, then releases the native buffer. The buffer
   * is process-global and manually owned, so the release happens here even if
   * the read fails. Returns undefined for an absent handle.
   */
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

  /** Releases a native byte buffer without reading it (dropping a stale chunk). */
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

  /** Multi-line stats of one instance (the dev panel text). */
  static replayInstanceStats(handle: number): string {
    return replayStats(handle);
  }

  /**
   * One compact line per live instance ("count=N\nhandle=<id> <stats>"), the
   * machine-readable form an agent reads from hilog.
   */
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

  /**
   * The native session handle, so a caller can address the shared process-global
   * registries (e.g. a background worker reading this session's buffers or
   * issuing a file-content request). -1 before the session exists.
   */
  handleId(): number {
    return this.handle;
  }

  startSession(options: RdpConnectOptions): number {
    this.teardownHandle();
    this.handle = createSession();
    if (this.handle < 0) {
      return -1;
    }
    SessionEventHub.register(this.handle, (event: RdpEvent, payload: RdpEventPayload): void => {
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
    SessionEventHub.unregister(this.handle);
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

  /**
   * Client Suppress Output: while true the server stops sending screen updates
   * and the native side drops the surface commands, so a hidden window stops
   * decoding and presenting. Applied per session; safe to call any time.
   */
  setSuppressed(suppressed: boolean): void {
    if (this.handle >= 0) {
      setSuppressedNative(this.handle, suppressed);
    }
  }

  /**
   * The latest display telemetry window of this session (see RdpCounters). The
   * native side returns raw readings (RdpTelemetry); this instance's aggregator
   * differences/averages them over the pull cadence. Returns undefined until a
   * baseline exists, so the caller falls back to its current figures.
   */
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

  /**
   * Reports the local keyboard's toggle keys (RdpLockFlags bits) so the remote
   * session follows HarmonyOS. Deduplicated natively, so every key event may
   * call it.
   */
  lockState(flags: number): void {
    if (this.handle >= 0) {
      sendLockState(this.handle, flags);
    }
  }

  /**
   * Re-asserts the local toggle keys with an unconditional Client Synchronize
   * PDU (no dedup). Used when the server's RemoteLockState disagrees with the
   * local keyboard; the decision (and its rate limit) is this side's.
   */
  reassertLockState(flags: number): boolean {
    if (this.handle < 0) {
      return false;
    }
    return sendSynchronize(this.handle, flags);
  }

  /**
   * Sends a Win/Meta key event. Used by the configurable "Win key substitute":
   * a normal key that reaches onKeyEvent is remapped to Win.
   */
  winKey(down: boolean): void {
    if (this.handle >= 0) {
      sendKey(this.handle, 0x5B, down, true);
    }
  }

  /**
   * 快捷键穿透: route this session's system-intercepted keys (KeyIntercept
   * events). Returns whether the state is really in effect.
   */
  setShortcutPassthrough(enabled: boolean): boolean {
    if (this.handle < 0) {
      return false;
    }
    return setShortcutPassthroughNative(this.handle, enabled);
  }

  /**
   * Advertises a local file offer: `descriptor` is a ready FILEGROUPDESCRIPTORW
   * (FileTransferCodec) and `paths` the absolute sandbox paths its bytes are
   * streamed from. Text / HTML / image pushes run on the background worker
   * (BackgroundWorker) so the native store copy and advertise never block the UI.
   */
  pushClipboardFileOffer(descriptor: ArrayBuffer, paths: string[]): void {
    if (this.handle >= 0 && paths.length > 0) {
      setClipboardFileOffer(this.handle, descriptor, paths.join('\n'));
    }
  }

  /**
   * One step of the ArkTS-driven file download (see FileDownload): a size query
   * when `sizeOnly`, else the byte range at `offset`. The response arrives as a
   * RemoteFileChunk event. Returns false when the channel is unavailable or a
   * request is already in flight.
   */
  requestRemoteFileContents(listIndex: number, sizeOnly: boolean, offset: number,
    length: number): boolean {
    if (this.handle < 0) {
      return false;
    }
    return requestRemoteFileContents(this.handle, listIndex, sizeOnly, offset, length);
  }

  /**
   * Requests one remote clipboard format ArkTS chose from a RemoteFormatList
   * event (see the clipboard negotiation in SessionPage). The channel does not
   * tag data responses, so ArkTS serialises these: only one may be in flight.
   * Returns false when the channel is unavailable.
   */
  requestClipboardFormat(formatId: number, kind: number): boolean {
    if (this.handle < 0) {
      return false;
    }
    return requestRemoteFormat(this.handle, formatId, kind);
  }

  /**
   * Aborts an in-flight upload (drops partial state and stops serving the
   * advertised file list). Downloads are cancelled on the ArkTS side.
   */
  cancelFileTransfer(): void {
    if (this.handle >= 0) {
      cancelFileTransfer(this.handle);
    }
  }
}
