/*
 * HmRdp - thin ArkTS facade over the libhmrdp.so Node-API module.
 *
 * One RdpNative instance owns exactly one native session, so several session
 * windows can run concurrently. Native events carry the session handle and are
 * routed back to the owning instance.
 */
import { clearSurface, connect, createSession, destroySession, disconnect,
  sendKey, sendMouse, sendTouch,
  sendUnicode, setClipboardHtmlRaw, setClipboardImageDib, setClipboardText, setRdpCursor, setSurface,
setSuppressed as setSuppressedNative, setTouchHighRate, updateSurface, setHardwareAccel,
setShortcutPassthrough as setShortcutPassthroughNative,
setGfxCapture,
setClipboardFileOffer, requestRemoteFileContents,
cancelFileTransfer, sendLockState,
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

const byKey: Map<string, RdpNative> = new Map<string, RdpNative>();

export class RdpNative {
  private handle: number = -1;
  private listener: RdpEventListener | undefined = undefined;
  // This session's telemetry aggregation (native sends raw readings only).
  private telemetry: SessionTelemetry = new SessionTelemetry();

  /** Associates an instance with the window's session key for teardown. */
  static register(key: string, native: RdpNative): void {
    byKey.set(key, native);
  }

  /** Returns the instance registered for a session key, if any. */
  static findByKey(key: string): RdpNative | undefined {
    return byKey.get(key);
  }

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
   * Dev/test: replay a recorded hmrdp_gfx.bin capture straight to the screen.
   * Decoding is FreeRDP's gdi pipeline; the presenter is the one the
   * "硬件加速" setting selects.
   * `realtime`: play the capture at its recorded arrival times (the live session's
   * own cadence) instead of the unthrottled fast mode - the mode to use when the
   * question is "can the client carry the *real* load". Captures recorded before
   * arrival times were stored fall back to the unpaced mode and say so in the
   * stats line (`mode=fast(untimed)`).
   * `refMode`: golden reference - 0 = off (perf run), 1 = record this run as the
   * reference, 2 = check against the stored one.
   */
  /**
   * Dev/test: start one replay instance on `surfaceId`; returns its handle (>0)
   * or -1. Instances are independent (each owns its own frame pipeline), so
   * several can run at once - the multi-session regression vehicle.
   * `realtime`: play the capture at its recorded arrival times instead of the
   * unthrottled fast mode. `refMode`: 0 = off, 1 = record, 2 = compare.
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

  /** Pushes the local clipboard text (UTF-8) to the remote session. */
  setClipboard(text: string): void {
    if (this.handle >= 0) {
      setClipboardText(this.handle, text);
    }
  }

  /**
   * Pushes a local HTML clipboard as a ready CF_HTML payload plus its plain-text
   * fallback (see ClipboardCodec.buildCfHtml); native only stores and serves it.
   */
  pushClipboardHtmlRaw(cfHtml: ArrayBuffer, plainText: string): void {
    if (this.handle >= 0) {
      setClipboardHtmlRaw(this.handle, cfHtml, plainText);
    }
  }

  /**
   * Pushes a local image to the remote session. `dib` is a ready CF_DIB payload
   * (see ClipboardCodec.buildDib); native only stores and serves it.
   */
  pushClipboardDib(dib: ArrayBuffer): void {
    if (this.handle >= 0) {
      setClipboardImageDib(this.handle, dib);
    }
  }

  /**
   * Advertises a local file offer: `descriptor` is a ready FILEGROUPDESCRIPTORW
   * (FileTransferCodec) and `paths` the absolute sandbox paths its bytes are
   * streamed from.
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
   * Aborts an in-flight upload (drops partial state and stops serving the
   * advertised file list). Downloads are cancelled on the ArkTS side.
   */
  cancelFileTransfer(): void {
    if (this.handle >= 0) {
      cancelFileTransfer(this.handle);
    }
  }
}
