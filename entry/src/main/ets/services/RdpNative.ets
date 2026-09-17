/*
 * HmRdp - thin ArkTS facade over the libhmrdp.so Node-API module.
 *
 * One RdpNative instance owns exactly one native session, so several session
 * windows can run concurrently. Native events carry the session handle and are
 * routed back to the owning instance.
 */
import { clearSurface, connect, createSession, destroySession, disconnect,
  onEvent, sendKey, sendMouse, sendTouch,
  sendUnicode, setClipboardHtml, setClipboardImage, setClipboardText, setRdpCursor, setSurface,
setTouchHighRate, updateSurface, setHardwareDecode, setRfxDump,
setDecodeThreads, decodeThreadsInfo,
RdpEventCallback,
startGfxReplayTest, stopGfxReplayTest, resizeGfxReplayTest, gfxReplayTestStats,
vulkanInfo } from 'libhmrdp.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RdpConnectOptions, RdpEvent } from '../model/RdpModels';

const GPU_DOMAIN: number = 0xD001;
const GPU_TAG: string = 'HmRdpNative';

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
   * HarmonyOS default cursor untouched (the pre-cursor-support behaviour).
   * Planned at connect time, so it applies to sessions opened afterwards.
   */
  static applyRdpCursor(enabled: boolean): void {
    setRdpCursor(enabled);
  }

  /** Prefer hardware (GPU) RemoteFX decoding; off uses the CPU decoder. Reserved
   * until the GPU engine takes over live sessions (every session currently
   * decodes with gdi / FreeRDP's own CPU path). */
  static applyHardwareDecode(enabled: boolean): void {
    setHardwareDecode(enabled);
  }

  /**
   * Progressive tile decode workers (process-global, live session and replay
   * alike). `workers` = 0 means automatic: the performance-core count when the
   * device exposes it, else min(cores, 4) - the measured sweet spot, see
   * doc_agent/cpu-path.md §5. 1 decodes on the receiving thread with no worker
   * pool at all (the power-optimal end of the range).
   */
  static applyDecodeThreads(workers: number): void {
    setDecodeThreads(workers);
  }

  /** What the decode worker count currently resolves to, for the settings page. */
  static describeDecodeThreads(): string {
    return decodeThreadsInfo();
  }

  /** Dev-only RemoteFX stream capture to the given directory. */
  static applyRfxDump(enabled: boolean, dir: string): void {
    setRfxDump(enabled, dir);
  }

  /**
   * Dev/test: replay a recorded hmrdp_gfx.bin capture straight to the screen.
   * `route`: 0 = CPU (FreeRDP gdi), 1 = Vulkan engine, 2 = Vulkan vs gdi compare.
   * `realtime`: play the capture at its recorded arrival times (the live session's
   * own cadence) instead of the fixed per-frame budget - the mode to use when the
   * question is "can the client carry the *real* load". Captures recorded before
   * arrival times were stored fall back to the fixed pacing, and say so in the
   * stats line (`mode=fast`).
   */
  static startReplayTest(surfaceId: string, surfaceW: number, surfaceH: number,
    gfxPath: string, route: number, realtime: boolean): string {
    const summary: string = startGfxReplayTest(surfaceId, surfaceW, surfaceH, gfxPath, route,
      realtime ? 1 : 0);
    hilog.info(GPU_DOMAIN, GPU_TAG, 'gfx replay: %{public}s', summary);
    return summary;
  }

  static stopReplayTest(): void {
    stopGfxReplayTest();
  }

  static resizeReplayTest(width: number, height: number): void {
    resizeGfxReplayTest(width, height);
  }

  static replayTestStats(): string {
    return gfxReplayTestStats();
  }

  /** Dev/test: logs and returns the Phase 0 Vulkan capability report. */
  static describeVulkan(): string {
    const info: string = vulkanInfo();
    hilog.info(GPU_DOMAIN, GPU_TAG, 'vulkan info: %{public}s', info);
    return info;
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
    this.teardownHandle();
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
    this.teardownHandle();
  }

  private teardownHandle(): void {
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
   * Sends a Win/Meta key event. Used by the configurable "Win key substitute":
   * a normal key that reaches onKeyEvent is remapped to Win.
   */
  winKey(down: boolean): void {
    if (this.handle >= 0) {
      sendKey(this.handle, 0x5B, down, true);
    }
  }

  /** Pushes the local clipboard text (UTF-8) to the remote session. */
  setClipboard(text: string): void {
    if (this.handle >= 0) {
      setClipboardText(this.handle, text);
    }
  }

  /** Pushes a local HTML fragment to the remote session (CF_HTML). */
  pushClipboardHtml(html: string): void {
    if (this.handle >= 0) {
      setClipboardHtml(this.handle, html);
    }
  }

  /**
   * Pushes a local image to the remote session. `pixelFormat` is the HarmonyOS
   * image.PixelMapFormat of `pixels`; native converts it to CF_DIB.
   */
  pushClipboardImage(width: number, height: number, pixelFormat: number,
    pixels: ArrayBuffer): void {
    if (this.handle >= 0) {
      setClipboardImage(this.handle, width, height, pixelFormat, pixels);
    }
  }
}
