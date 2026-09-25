export interface RdpOptions {
  host: string;
  port: number;
  username: string;
  password: string;
  domain: string;
  width: number;
  height: number;
  scalePercent: number;
  colorDepth: number;
  ignoreCertificate: boolean;
  enableAudio: boolean;
  enableGfx: boolean;
  enableH264: boolean;
  enableRemoteFx: boolean;
  /**
   * 超分 (super resolution). `width`/`height` are the *output* resolution; with
   * this on, the session asks the server for `output ÷ srRatioPercent` and the
   * Vulkan presenter upscales every frame back with XEngine's GPU spatial
   * upscale. Ignored by the GLES presenter, so it requires 硬件加速.
   */
  srEnabled: boolean;
  /** Upscale ratio in percent (125 = 1.25×); ignored unless srEnabled. */
  srRatioPercent: number;
  performanceFlags: number;
  gatewayHost: string;
  gatewayPort: number;
  gatewayUsername: string;
  gatewayPassword: string;
  gatewayDomain: string;
}

export type RdpEventCallback = (handle: number, event: number, data: string) => void;

export const createSession: () => number;
export const destroySession: (handle: number) => void;
export const connect: (handle: number, options: RdpOptions) => boolean;
export const disconnect: (handle: number) => void;
export const setSurface: (handle: number, surfaceId: string, width: number, height: number) => void;
export const updateSurface: (handle: number, width: number, height: number) => void;
export const clearSurface: (handle: number) => void;
export const sendMouse: (handle: number, flags: number, x: number, y: number) => boolean;
export const sendTouch: (handle: number, flags: number, finger: number, pressure: number,
  x: number, y: number) => boolean;
export const sendKey: (handle: number, scancode: number, down: boolean, extended: boolean) => boolean;
export const sendUnicode: (handle: number, codepoint: number, down: boolean) => boolean;
export const setClipboardText: (handle: number, text: string) => boolean;

/** Pushes a local HTML fragment to the session (advertised as CF_HTML). */
export const setClipboardHtml: (handle: number, html: string) => boolean;

/**
 * Pushes a local image to the session. `pixelFormat` is the HarmonyOS
 * image.PixelMapFormat of `pixels`; the native side converts it to CF_DIB.
 */
export const setClipboardImage: (handle: number, width: number, height: number,
  pixelFormat: number, pixels: ArrayBuffer) => boolean;

/**
 * Pushes local files to the session (advertised as FileGroupDescriptorW +
 * FileContents). `paths` holds the absolute sandbox paths, one per line.
 */
export const setClipboardFiles: (handle: number, paths: string) => boolean;

/**
 * Downloads the file list the server last advertised into `destDir`, which must
 * already exist. Completion arrives as the ClipboardFilesReady event; progress
 * as FileTransferProgress.
 */
export const pullClipboardFiles: (handle: number, destDir: string) => boolean;

/** Aborts an in-flight download and removes its partial files. */
export const cancelFileTransfer: (handle: number) => boolean;

export const onEvent: (callback: RdpEventCallback) => void;

/**
 * Whether this device provides an audio output. False means the native OHAudio
 * library is unavailable, so remote audio is disabled gracefully.
 */
export const isAudioSupported: () => boolean;

/**
 * Process-global RDPEI frame pacing. When true, touch contacts are forwarded at
 * the full input rate instead of FreeRDP's default ~20ms (50Hz) coalescing.
 * Requires the patched FreeRDP build; returns false if the hook is absent.
 */
export const setTouchHighRate: (enabled: boolean) => boolean;

/**
 * Process-global remote cursor switch. When true, RDP pointer updates drive the
 * HarmonyOS system cursor; when false they are ignored and the default cursor is
 * kept. Applied when a session connects.
 */
export const setRdpCursor: (enabled: boolean) => boolean;

/**
 * "硬件加速": present frames through Vulkan (on) or through the GLES presenter
 * only (off - no Vulkan in the session at all). Applied when a session connects;
 * decoding is FreeRDP's gdi path either way.
 */
export const setHardwareAccel: (enabled: boolean) => boolean;

/**
 * Dev-only: capture the full GFX command stream (`<dir>/hmrdp_gfx.bin`) plus
 * per-frame surface baselines (`<dir>/hmrdp_gfx_surface.bin`) and the legacy
 * RemoteFX stream (`<dir>/hmrdp_rfx.bin`, PERF-TODO §2).
 */
export const setRfxDump: (enabled: boolean, dir: string) => boolean;

/**
 * Dev-only: replay a recorded hmrdp_gfx.bin capture straight to the screen.
 * Decoding is FreeRDP's gdi pipeline; the presenter is the one the "硬件加速"
 * setting selects.
 * `realtime` plays the capture at its recorded arrival times (the cadence the
 * live session ran at) instead of a fixed per-frame budget; it is ignored for
 * captures recorded before the arrival times were stored.
 * `refMode` selects the golden-reference check: 0 = off, 1 = record, 2 = compare.
 */
export const startGfxReplayTest: (surfaceId: string, surfaceW: number, surfaceH: number,
  gfxPath: string, realtime: number, refMode: number) => string;

export const stopGfxReplayTest: () => void;
export const resizeGfxReplayTest: (width: number, height: number) => void;
export const gfxReplayTestStats: () => string;

/**
 * Dev/test: Vulkan capability report - loader/device version, migration-relevant
 * extensions, memory types and queue families. Multi-line, for the dev panel.
 */
export const vulkanInfo: () => string;

/**
 * Whether frames can be presented through Vulkan on this device - the capability
 * behind the "硬件加速" setting. Returns "1", or "0|<code>" with a stable code:
 * `no-vulkan` / `no-instance` / `no-device` / `no-host-memory` / `no-surface` /
 * `emulator`. The UI layer maps the code to its own wording.
 */
export const vulkanAccelSupport: () => string;

/**
 * Whether this device can upscale a presented frame with XEngine's GPU spatial
 * upscale - the capability behind the "超分" setting. Returns "1", or "0|<code>"
 * with a stable code: the `vulkanAccelSupport` codes (`no-vulkan` / `no-instance`
 * / `no-device` / `no-host-memory` / `no-surface` / `emulator`) when the Vulkan
 * presenter itself is unusable, or `no-xengine` (libxengine.so missing) /
 * `no-extension` (the GPU has no XEG_spatial_upscale). 超分 needs the Vulkan
 * presenter, so the caller must also check `vulkanAccelSupport`.
 */
export const superResolutionSupport: () => string;
export const superResolutionBackends: () => string;
