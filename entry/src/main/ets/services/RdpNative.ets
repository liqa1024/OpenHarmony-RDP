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
setTouchHighRate, updateSurface, setHardwareDecode, setRfxDump, gpuComputeInfo,
RdpEventCallback,
startGfxReplayTest, stopGfxReplayTest, resizeGfxReplayTest, gfxReplayTestStats,
setGfxReplayBatchArea, vulkanInfo, startVulkanTest, presentVulkanTest, resizeVulkanTest,
stopVulkanTest, startVulkanEngineTest, stopVulkanEngineTest, resizeVulkanEngineTest,
vulkanEngineTestStats } from 'libhmrdp.so';
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

  /** Prefer hardware (GPU) RemoteFX decoding; off uses the CPU decoder. */
  static applyHardwareDecode(enabled: boolean): void {
    setHardwareDecode(enabled);
  }

  /** Dev-only RemoteFX stream capture to the given directory. */
  static applyRfxDump(enabled: boolean, dir: string): void {
    setRfxDump(enabled, dir);
  }

  /** Dev/test: logs the device GLES compute capability, returns its summary. */
  static describeGpuCompute(): string {
    const desc: string = gpuComputeInfo();
    hilog.info(GPU_DOMAIN, GPU_TAG, 'gpu compute: %{public}s', desc);
    return desc;
  }

  /**
   * Dev/test: replay a recorded hmrdp_gfx.bin capture straight to the screen.
   * `route`: 0 = GPU desktop engine, 1 = FreeRDP gdi (CPU reference route).
   */
  static startReplayTest(surfaceId: string, surfaceW: number, surfaceH: number,
    gfxPath: string, route: number): string {
    const summary: string = startGfxReplayTest(surfaceId, surfaceW, surfaceH, gfxPath, route);
    hilog.info(GPU_DOMAIN, GPU_TAG, 'gfx replay: %{public}s', summary);
    return summary;
  }

  static stopReplayTest(): void {
    stopGfxReplayTest();
  }

  /**
   * Dev/test: ClearCodec batch granularity for the GPU replay (union-rectangle
   * pixel cap). Takes effect on the next replay start.
   */
  static setReplayBatchArea(pixels: number): void {
    setGfxReplayBatchArea(pixels);
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

  /** Dev/test: bind the XComponent surface to the Vulkan swapchain. */
  static startVulkanTest(surfaceId: string, surfaceW: number, surfaceH: number): string {
    const summary: string = startVulkanTest(surfaceId, surfaceW, surfaceH);
    hilog.info(GPU_DOMAIN, GPU_TAG, 'vulkan test: %{public}s', summary);
    return summary;
  }

  static presentVulkanTest(red: number, green: number, blue: number): boolean {
    return presentVulkanTest(red, green, blue);
  }

  static resizeVulkanTest(width: number, height: number): void {
    resizeVulkanTest(width, height);
  }

  static stopVulkanTest(): void {
    stopVulkanTest();
  }

  /**
   * Dev/test (V1): replay the capture through the Vulkan surface engine and
   * compare it pixel by pixel with FreeRDP's gdi pipeline on the same bytes.
   * `present` != 0 also blits the Vulkan screen to the XComponent.
   */
  static startVulkanEngineTest(gfxPath: string, surfaceId: string, surfaceW: number,
    surfaceH: number, present: number): string {
    const summary: string = startVulkanEngineTest(gfxPath, surfaceId, surfaceW, surfaceH, present);
    hilog.info(GPU_DOMAIN, GPU_TAG, 'vk engine test: %{public}s', summary);
    return summary;
  }

  static stopVulkanEngineTest(): void {
    stopVulkanEngineTest();
  }

  static resizeVulkanEngineTest(width: number, height: number): void {
    resizeVulkanEngineTest(width, height);
  }

  static vulkanEngineTestStats(): string {
    return vulkanEngineTestStats();
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
