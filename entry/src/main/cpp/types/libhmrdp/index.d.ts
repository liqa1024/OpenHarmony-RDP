export interface RdpOptions {
  host: string;
  port: number;
  username: string;
  password: string;
  domain: string;
  width: number;
  height: number;
  scalePercent: number;
  ignoreCertificate: boolean;
  enableAudio: boolean;
  /**
   * 超分 (super resolution). `width`/`height` are the *output* resolution; with
   * this on, the session asks the server for `output ÷ srRatioPercent` and the
   * Vulkan presenter upscales every frame back to the output resolution. Ignored
   * by the GLES presenter, so it requires 硬件加速.
   */
  srEnabled: boolean;
  /**
   * Which upscaler runs: 0 = XEngine (platform spatial upscale, needs the device
   * feature), 1 = the presenter's own FSR 1.0. Ignored unless srEnabled.
   */
  srBackend: number;
  /** Upscale ratio in percent (125 = 1.25×); ignored unless srEnabled. */
  srRatioPercent: number;
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

/**
 * Client Suppress Output: while `suppressed` is true the server stops sending
 * screen updates and the client drops the surface commands, so a hidden
 * (minimized / fully occluded) session stops decoding and presenting. The server
 * repaints when it is set back to false.
 */
export const setSuppressed: (handle: number, suppressed: boolean) => boolean;
export const sendTouch: (handle: number, flags: number, finger: number, pressure: number,
  x: number, y: number) => boolean;
export const sendKey: (handle: number, scancode: number, down: boolean, extended: boolean) => boolean;
export const sendUnicode: (handle: number, codepoint: number, down: boolean) => boolean;

/**
 * Reports the local keyboard's toggle keys (KBD_SYNC_* bits: scroll/num/caps)
 * so the server follows HarmonyOS. Deduplicated natively, so it is safe to call
 * on every key event.
 */
export const sendLockState: (handle: number, flags: number) => boolean;

/**
 * 快捷键穿透: route the system-intercepted key events (OH_Input_AddKeyEventInterceptor,
 * INTERCEPT_INPUT_EVENT ACL) to this session while enabled, so Win / Alt+Tab / Esc
 * reach the remote. Each arrives as a KeyIntercept event, payload "<keyCode>|<down>".
 * Returns whether the state is in effect (false when registration failed).
 */
export const setShortcutPassthrough: (handle: number, enabled: boolean) => boolean;

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
 * Dev-only: capture the raw (still ZGX-compressed) GFX channel stream to
 * `<dir>/hmrdp_gfx.bin`.
 */
export const setGfxCapture: (enabled: boolean, dir: string) => boolean;

/**
 * Dev/test: replay a recorded hmrdp_gfx.bin capture straight to the screen.
 * Each call starts an independent instance on `surfaceId` and returns its handle
 * (>0), or -1 on failure. Several instances can run at once - each owns its own
 * frame pipeline - so this is also the multi-session regression vehicle.
 * Decoding is FreeRDP's gdi pipeline; the presenter is the one the "硬件加速"
 * setting selects at start.
 * `realtime` plays the capture at its recorded arrival times (the cadence the
 * live session ran at) instead of the unthrottled fast mode; it is ignored for
 * captures recorded before the arrival times were stored.
 * `refMode` selects the golden-reference check: 0 = off, 1 = record, 2 = compare.
 */
export const startReplay: (surfaceId: string, surfaceW: number, surfaceH: number,
  gfxPath: string, realtime: number, refMode: number) => number;

/** Stops one replay instance and releases its surface. */
export const stopReplay: (handle: number) => void;

/** Stops every replay instance. */
export const stopAllReplays: () => void;

export const resizeReplay: (handle: number, width: number, height: number) => void;

/** Multi-line stats of one instance (the dev panel text). */
export const replayStats: (handle: number) => string;

/**
 * One compact line per live instance ("count=N\nhandle=<id> <stats>"). This is
 * the machine-readable form for the dev harness and for an agent reading hilog.
 */
export const listReplays: () => string;

/**
 * Dev/test: Vulkan capability report - loader/device version, extensions, memory
 * types and queue families, one field per line.
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
 * Whether this device can upscale a presented frame at all (at least one 超分
 * backend usable) - the capability behind the "超分" setting. Returns "1", or
 * "0|<code>" with a stable code: the `vulkanAccelSupport` codes when the Vulkan
 * presenter itself is unusable, else `no-xengine` (no usable backend). 超分 needs
 * the Vulkan presenter, so the caller must also check `vulkanAccelSupport`.
 */
export const superResolutionSupport: () => string;
/**
 * Which 超分 backends this build and device can run, comma-separated:
 * "xengine" / "fsr" / "xengine,fsr", or "" when none. The UI greys out the ones
 * absent from the list.
 */
export const superResolutionBackends: () => string;

/**
 * Whether the Moonlight (GameStream) client library is linked into this build.
 * "1", or "0|<code>" (currently only `no-moonlight`).
 */
export const moonlightProbe: () => string;

/**
 * Whether the Moonlight HTTPS control layer (serverinfo / pairing / app list) is
 * built into this module. "1", or "0|<code>" (currently only
 * `no-moonlight-http`) when the OpenSSL archives were not linked in.
 */
export const moonlightHttpProbe: () => string;

/**
 * Bitmask of the Moonlight VIDEO_FORMAT_* codecs to advertise for negotiation.
 * `preference` is an ML_CODEC_* value (default 0 = auto): auto sends every
 * codec the device can hardware-decode (or the software set when there is no
 * hardware decoder), a specific codec sends just that one when it is decodable.
 * 0 without a Moonlight client.
 */
export const moonlightVideoFormats: (preference?: number) => number;

/**
 * The negotiated codec token ("h264" / "hevc" / "av1"), or "" before a stream
 * starts. The toolbar pairs it with the resolution, e.g. "1920 x 1080 (HEVC)".
 */
export const moonlightVideoCodec: () => string;

/**
 * Decoder support for an ML_CODEC_* value (1 H.264 / 2 HEVC / 3 AV1):
 * 0 none, 1 software only, 2 hardware. Auto/unknown returns 0. The settings UI
 * greys unsupported codecs and warns on software-only ones.
 */
export const moonlightVideoCodecSupport: (codec: number) => number;

/**
 * Stable code for the current decode path, "" when fine:
 * "software-decode|<mime>" (only a software decoder exists for the negotiated
 * codec) or "no-decoder|<mime>". The UI owns the wording.
 */
export const moonlightVideoWarning: () => string;
