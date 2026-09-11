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
  enableClipboard: boolean;
  enableAudio: boolean;
  enableGfx: boolean;
  enableH264: boolean;
  enableRemoteFx: boolean;
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
 * Prefer hardware (GPU) RemoteFX decoding over the CPU decoder. Applied when a
 * session connects.
 */
export const setHardwareDecode: (enabled: boolean) => boolean;

/**
 * Dev-only: capture incoming RemoteFX/Progressive GFX surface streams to
 * `<dir>/hmrdp_rfx.bin` (PERF-TODO §2).
 */
export const setRfxDump: (enabled: boolean, dir: string) => boolean;
