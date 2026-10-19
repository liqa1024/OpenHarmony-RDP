export interface RdpOptions {
  host: string;
  port: number;
  username: string;
  password: string;
  domain: string;
  width: number;
  height: number;
  /** 超分: the session size the server should render, 0 = width/height; the caller resolves it. */
  streamWidth?: number;
  streamHeight?: number;
  scalePercent: number;
  ignoreCertificate: boolean;
  enableAudio: boolean;
  /** 0 = XEngine (platform spatial upscale), 1 = the presenter's own FSR 1.0; used only when streamWidth/Height differ from width/height. */
  srBackend: number;
  gatewayHost: string;
  gatewayPort: number;
  gatewayUsername: string;
  gatewayPassword: string;
  gatewayDomain: string;
}

export interface RdpEventPayload {
  code?: number;
  message?: string;
  width?: number;
  height?: number;
  hotX?: number;
  hotY?: number;
  /** CursorShape: the desktop magnification × 1000 the bitmap is sized against. */
  magnificationMilli?: number;
  /** CursorShape: native content hash of the bitmap (dedup key). */
  shapeHash?: number;
  /** Handle of a native byte buffer: raw BGRA for a cursor shape, raw CF_DIB for a clipboard image. Read with bufferRead(), then bufferRelease(). */
  imageHandle?: number;
  /** Handle of a native byte buffer for a raw clipboard payload that is neither an image nor text. Read with bufferRead(), then bufferRelease(). */
  dataHandle?: number;
  text?: string;
  html?: string;
  fileCount?: number;
  folderCount?: number;
  totalBytes?: number;
  direction?: number;
  doneBytes?: number;
  fileIndex?: number;
  reason?: string;
  keyCode?: number;
  down?: boolean;
  /** RemoteFileChunk: the server-advertised file index this response is for. */
  listIndex?: number;
  /** RemoteFileChunk: true when the response answers a FILECONTENTS_SIZE query. */
  isSize?: boolean;
  /** RemoteFileChunk: the file size for a size response. */
  sizeValue?: number;
  /** RemoteFileChunk: true when the server refused the request. */
  failed?: boolean;
  /** AudioFormat: the sample rate (Hz) and channel count the renderer must use. */
  audioRate?: number;
  audioChannels?: number;
  /** RemoteFormatList: the supported clipboard candidates as "kind:id,kind:id". */
  formatCandidates?: string;
  /** RemoteFormatResponse: the kind of the request that settled. */
  clipKind?: number;
  /** RemoteLockState: the server's KBD_SYNC_* toggle-key bits. */
  lockFlags?: number;
}

export type RdpEventCallback = (handle: number, event: number, payload: RdpEventPayload) => void;

/** One raw telemetry sample; the reader derives its rates from the window deltas. sampleSeq is monotonic (skip a repeated id); rttMs is -1 until known. */
export interface RdpTelemetry {
  sampleSeq: number;
  windowElapsedMs: number;
  rxBytesWindow: number;
  txBytesWindow: number;
  framesWindow: number;
  rttMs: number;
  windowFrames: number;
  zgxParseUs: number;
  decodeUs: number;
  composeUs: number;
  presentUs: number;
  presentWaitUs: number;
  syncUs: number;
  windowBytes: number;
  windowCommands: number;
  audioRateHz: number;
  audioLostBytes: number;
  audioTotalBytes: number;
  gpuCopyUs: number;
  gpuSrUs: number;
  gpuBlitUs: number;
}

export const createSession: () => number;
export const destroySession: (handle: number) => void;
export const connect: (handle: number, options: RdpOptions) => boolean;
export const disconnect: (handle: number) => void;
export const setSurface: (handle: number, surfaceId: string, width: number, height: number) => void;
export const updateSurface: (handle: number, width: number, height: number) => void;
export const clearSurface: (handle: number) => void;
export const sendMouse: (handle: number, flags: number, x: number, y: number) => boolean;

/** Client Suppress Output: while true the server stops updates and the client drops surface commands; the server repaints when cleared. */
export const setSuppressed: (handle: number, suppressed: boolean) => boolean;
export const sendTouch: (handle: number, flags: number, finger: number, pressure: number,
  x: number, y: number) => boolean;
export const sendKey: (handle: number, scancode: number, down: boolean, extended: boolean) => boolean;
export const sendUnicode: (handle: number, codepoint: number, down: boolean) => boolean;

/** Reports the local toggle keys (KBD_SYNC_*); deduplicated natively, safe to call on every key event. */
export const sendLockState: (handle: number, flags: number) => boolean;

/** Unconditional Client Synchronize PDU (no dedup), to put a disagreeing server back on the local toggle state. */
export const sendSynchronize: (handle: number, flags: number) => boolean;

/** 快捷键穿透: route the system-intercepted key events to this session while enabled; each arrives as a KeyIntercept event. */
export const setShortcutPassthrough: (handle: number, enabled: boolean) => boolean;

export const setClipboardText: (handle: number, text: string) => boolean;

/** Pushes a local HTML clipboard as a ready CF_HTML payload plus its plain-text fallback. */
export const setClipboardHtmlRaw: (handle: number, cfHtml: ArrayBuffer,
  plainText: string) => boolean;

/** Pushes a local image: `dib` is a ready CF_DIB, `png` the same image as a registered "PNG" payload (may be empty). */
export const setClipboardImage: (handle: number, dib: ArrayBuffer,
  png: ArrayBuffer) => boolean;

/** Advertises a local file offer (FileGroupDescriptorW + FileContents); `paths` are absolute sandbox paths, one per line. */
export const setClipboardFileOffer: (handle: number, descriptor: ArrayBuffer,
  paths: string) => boolean;

/** One step of the ArkTS-driven download: a size query when `sizeOnly`, else the byte range; one request in flight. */
export const requestRemoteFileContents: (handle: number, listIndex: number,
  sizeOnly: boolean, offset: number, length: number) => boolean;

/** Pulls one remote clipboard format chosen from a RemoteFormatList event; only one request may be in flight. */
export const requestRemoteFormat: (handle: number, formatId: number,
  kind: number) => boolean;

/** Aborts an in-flight upload; downloads are cancelled on the ArkTS side. */
export const cancelFileTransfer: (handle: number) => boolean;

export const onEvent: (callback: RdpEventCallback) => void;

/** The raw telemetry reading of `handle`; an unknown handle yields the zeroed shape. */
export const sessionTelemetry: (handle: number) => RdpTelemetry;

/** Native byte buffer (a cursor / clipboard bitmap): read once with bufferRead(), then release the handle. */
export const bufferSize: (handle: number) => number;
/** A copy of the buffer's bytes as a JS ArrayBuffer; the handle stays valid. */
export const bufferRead: (handle: number) => ArrayBuffer;
/** Frees the native buffer. Call once after the bytes have been copied out. */
export const bufferRelease: (handle: number) => void;

/** Resamples one native BGRA handle-to-handle (area downscale / Lanczos3 + `sharpen` upscale); the source handle is consumed. */
export const scaleBgraBitmap: (handle: number, srcW: number, srcH: number,
  dstW: number, dstH: number, sharpen: number) => number;

/** Pulls PCM into `dest` (fills every byte: queued audio plus underrun silence); returns real bytes copied. */
export const audioRead: (handle: number, dest: ArrayBuffer) => number;

/** The session's current PCM format (0/0 before the first packet). */
export interface RdpAudioFormat {
  rate: number;
  channels: number;
}

/** The session's current audio format, for a renderer that starts after the AudioFormat event. */
export const audioFormat: (handle: number) => RdpAudioFormat;

/** Process-global remote cursor switch; applied when a session connects. */
export const setRdpCursor: (enabled: boolean) => boolean;

/** "硬件加速": present through Vulkan (on) or the GLES presenter only (off); applied when a session connects. */
export const setHardwareAccel: (enabled: boolean) => boolean;
/**
 * The refresh rate of the display holding the session window, resolved by the UI layer (`window`
 * properties -> that display): the shared display-rate authority uses it instead of the default
 * display's, so multi-monitor 2in1 keeps the present gate and the refresh hint on the right screen.
 * 0 hands the reading back to the default display.
 */
export const setDisplayRefreshRateHz: (hz: number) => boolean;

/** Dev-only: capture the raw (still ZGX-compressed) GFX stream to `<dir>/hmrdp_gfx.bin`. */
export const setGfxCapture: (enabled: boolean, dir: string) => boolean;

/** Dev/test: replay a recorded hmrdp_gfx.bin; returns a handle (>0) or -1. `realtime` paces at recorded arrival times; `refMode` 0 off / 1 record / 2 compare. */
export const startReplay: (surfaceId: string, surfaceW: number, surfaceH: number,
  gfxPath: string, realtime: number, refMode: number) => number;

/** Stops one replay instance and releases its surface. */
export const stopReplay: (handle: number) => void;

/** Stops every replay instance. */
export const stopAllReplays: () => void;

export const resizeReplay: (handle: number, width: number, height: number) => void;

/** Multi-line stats of one instance (the dev panel text). */
export const replayStats: (handle: number) => string;

/** One compact line per live instance ("count=N\nhandle=<id> <stats>"). */
export const listReplays: () => string;

/** Dev/test: Vulkan capability report (versions, extensions, memory, queues), one field per line. */
export const vulkanInfo: () => string;

/** Verdict of a capability probe: usable, plus a stable reason code ("" when supported). */
export interface RdpVerdict {
  supported: boolean;
  code: string;
}

/** Whether frames can be presented through Vulkan; `code` is a stable token (no-vulkan / no-instance / no-device / no-host-memory / no-surface / emulator). */
export const vulkanAccelSupport: () => RdpVerdict;

/** Whether the device can upscale a frame at all (at least one 超分 backend); 超分 needs the Vulkan presenter. */
export const superResolutionSupport: () => RdpVerdict;

/** Which 超分 backends this build and device can run. */
export interface RdpSrBackends {
  xengine: boolean;
  fsr: boolean;
  /** The device can import an AVCodec NV12 buffer into the presenter (needed by the Moonlight path). */
  videoImport: boolean;
}
export const superResolutionBackends: () => RdpSrBackends;

/** Always "1": asserts the Moonlight static archives resolved into the module. */
export const moonlightProbe: () => string;

/** The single Moonlight VIDEO_FORMAT_* the session will use (0 = none); `choice` is ML_CODEC_* (0 = auto). */
export const moonlightVideoFormats: (choice: number, serverCodecModeSupport: number) => number;

/** The negotiated codec token ("h264" / "hevc" / "av1"), or "" before a stream starts. */
export const moonlightVideoCodec: () => string;

/** Decoder support for an ML_CODEC_* (1 H.264 / 2 HEVC / 3 AV1): 0 none, 1 software, 2 hardware. */
export const moonlightVideoCodecSupport: (codec: number) => number;

/** Highest frame rate the decoder for a VIDEO_FORMAT_* reports at that picture size (0 = not stated). */
export const moonlightVideoMaxFps: (videoFormat: number, width: number, height: number) => number;

/** Stable decode-path codes, comma separated when several apply: "software-decode|<mime>",
 *  "sr-unavailable", "sr-inactive", "video-present"; "" when fine. */
export const moonlightVideoWarning: () => string;

/** Everything LiStartConnection needs; assembled by the ArkTS control layer. */
export interface MoonlightSessionConfig {
  host: string;
  serverInfoAppVersion: string;
  serverInfoGfeVersion: string;
  sessionUrl: string;
  serverCodecModeSupport: number;
  width: number;
  height: number;
  /** 超分: the stream size the host should encode, 0 = width/height; the caller resolves it. */
  streamWidth?: number;
  streamHeight?: number;
  fps: number;
  bitrateKbps: number;
  packetSize: number;
  /** STREAM_CFG_*: 0 local, 1 remote, 2 auto. */
  streamingRemotely: number;
  audioConfiguration: number;
  supportedVideoFormats: number;
  clientRefreshRateX100: number;
  colorSpace: number;
  colorRange: number;
  encryptionFlags: number;
  /** 16-byte remote-input AES key / IV as lowercase hex. */
  remoteInputAesKeyHex: string;
  remoteInputAesIvHex: string;
  /** foundation-sunshine local cursor mode: the host forwards the pointer on the cursor channel; a no-op when unsupported. */
  localCursorMode?: boolean;
  /** Play the host's Opus audio on this device; false (or no output device) stays silent. */
  enableAudio?: boolean;
  /** Play audio through the native OHAudio low-latency path instead of the ArkTS renderer. */
  lowLatencyAudio?: boolean;
  /** 超分 backend: 0 = XEngine, 1 = FSR; used only when streamWidth/Height differ from width/height. */
  srBackend?: number;
}

/** Extra fields alongside the event tuple; only the local cursor event (ML_EVENT_CURSOR_SHAPE) fills them. */
export interface MoonlightEventExtra {
  width?: number;
  height?: number;
  hotX?: number;
  hotY?: number;
  /** Cursor shape scale in thousandths (1000 = stream pixels); mirrors RDP's pointer magnification. */
  magnificationMilli?: number;
  /** The host-assigned shape identity (protocol-native dedup key). */
  shapeId?: number;
  visible?: boolean;
  hasShape?: boolean;
  imageHandle?: number;
  /** Clipboard frame bytes (ML_EVENT_CLIPBOARD): read with bufferRead(), release with bufferRelease(). */
  dataHandle?: number;
}

/** Registers the process-global session event listener; `event` is an ML_EVENT_* id, `extra` the cursor geometry/bitmap. */
export const moonlightSetSessionListener: (listener: (event: number, value: number,
  text: string, extra: MoonlightEventExtra) => void) => void;

/** Binds the session window (before start); false when refused. */
export const moonlightSessionSetSurface: (surfaceId: string, width: number,
  height: number) => boolean;

/** Releases the session window; call after moonlightSessionStop. */
export const moonlightSessionClearSurface: () => void;

/** 超分: forwards a window resize to the session's presenter swapchain; a no-op on the direct path. */
export const moonlightSessionResizeSurface: (width: number, height: number) => void;

/** Starts the session; "" on success, else "busy" / "config" / "thread". */
export const moonlightSessionStart: (config: MoonlightSessionConfig) => string;

/** Interrupts/stops the session and waits for its thread to finish. */
export const moonlightSessionStop: () => void;

export const moonlightSessionActive: () => boolean;

/** Pulls decoded PCM into `dest` (fills every byte: queued audio plus underrun silence); returns real bytes copied. */
export const moonlightAudioRead: (dest: ArrayBuffer) => number;

/** The decoded PCM format (0/0 before negotiation, i.e. when audio is disabled). */
export const moonlightAudioFormat: () => RdpAudioFormat;

/** Sunshine extended query string to append to /launch and /resume. */
export const moonlightLaunchQueryParameters: () => string;

/**
 * Input entry points below return the library's own result code: 0 when the event was taken,
 * -5501 (`LI_ERR_UNSUPPORTED`) when the host lacks the feature (a by-design refusal), any other
 * non-zero when the event was refused, and 0 when no session is active (there is no transport to
 * refuse it, and the session failure is signalled elsewhere).
 */

/** Input: relative mouse move. */
export const moonlightMouseMove: (deltaX: number, deltaY: number) => number;

/** Input: absolute mouse position in a reference frame (touch/pointer mapping). */
export const moonlightMousePosition: (x: number, y: number, refWidth: number,
  refHeight: number) => number;

/** Input: mouse button action / button (LiSendMouseButtonEvent constants). */
export const moonlightMouseButton: (action: number, button: number) => number;

/** Input: key code / action / modifiers (LiSendKeyboardEvent constants). */
export const moonlightKey: (keyCode: number, action: number, modifiers: number) => number;

/** Input: wheel clicks (LiSendScrollEvent). */
export const moonlightScroll: (clicks: number) => number;

/** Input: native touchscreen event (LI_TOUCH_EVENT_*), x/y normalized 0..1, pressure 0..1. */
export const moonlightTouch: (eventType: number, pointerId: number, x: number, y: number,
  pressure: number) => number;

/** Input: native precision-touchpad frame (foundation-sunshine); parallel contact arrays. */
export const moonlightTouchpad: (eventTypes: number[], pointerIds: number[], x: number[],
  y: number[], pressure: number[], widthMm: number, heightMm: number,
  buttonState: number) => number;

/** Input: high-resolution vertical wheel in WHEEL_DELTA (120) units. */
export const moonlightHighResScroll: (amount: number) => number;

/** Input: high-resolution horizontal wheel in WHEEL_DELTA (120) units. */
export const moonlightHighResHScroll: (amount: number) => number;

/** Input: composed text sent as one UTF-8 string (LiSendUtf8TextEvent). */
export const moonlightText: (text: string) => number;

/**
 * Input: one controller slot's whole state (LiSendMultiControllerEvent); `activeGamepadMask` has a
 * bit per attached controller, triggers are 0..255 and sticks -32767..32767 with +Y up.
 */
export const moonlightController: (controllerNumber: number, activeGamepadMask: number,
  buttonFlags: number, leftTrigger: number, rightTrigger: number, leftStickX: number,
  leftStickY: number, rightStickX: number, rightStickY: number) => number;

/** Input: declares one attached controller (LiSendControllerArrivalEvent: LI_CTYPE_* / LI_CCAP_*). */
export const moonlightControllerArrival: (controllerNumber: number, activeGamepadMask: number,
  type: number, supportedButtonFlags: number, capabilities: number) => number;

/** Sends an opaque clipboard v1 wire frame (control message 0x5508); returns LiSendClipboardData's code (0 ok, -1 invalid, -2 unsupported, -3 not connected, -4 send failed). */
export const moonlightSendClipboardData: (data: ArrayBuffer) => number;

/** LI_FF_* host feature bits (0 before a session is established). */
export const moonlightHostFeatures: () => number;

/** Shortcut passthrough for Moonlight; returns the requested state in effect. */
export const moonlightSetShortcutPassthrough: (enabled: boolean) => boolean;

/** One online controller as the platform reports it. */
export interface GamepadKitDevice {
  /** The platform's device key for this controller (opaque string). */
  key: string;
  /** Device name, as the platform reports it. */
  name: string;
}

/** Extra fields of a Game Controller Kit event (`event` is GAMEPAD_KIT_EVENT_*, `source` GAMEPAD_KIT_AXIS_*). */
export interface GamepadKitEventExtra {
  /** Device key of the controller the event belongs to. */
  deviceKey?: string;
  /** Device name (filled on a device event). */
  name?: string;
  /** Axis values for the event's source: one for a trigger, two for a stick or the D-pad. */
  value1?: number;
  value2?: number;
}

/**
 * Subscribes to the platform's Game Controller Kit (axis events carry the control they came from,
 * so no axis layout has to be inferred). Returns false when the Kit is unavailable — the caller then
 * keeps the multimodal input path.
 */
export const gamepadKitSetListener: (listener: (event: number, source: number,
  extra: GamepadKitEventExtra) => void) => boolean;

/** Releases the Kit subscription. */
export const gamepadKitStop: () => void;

/** Online controllers, as the platform reports them. */
export const gamepadKitDevices: () => GamepadKitDevice[];

/** One raw telemetry sample; counters are cumulative (the caller diffs consecutive samples into rates). */
export interface MoonlightTelemetry {
  /** Pictures decoded out of the codec, cumulative. */
  frames: number;
  /** Pictures actually presented to the display, cumulative. */
  presentedFrames: number;
  /** Access units handed to the decoder, cumulative. */
  submittedFrames: number;
  /** Access units dropped because they were already too late to show, cumulative. */
  droppedFrames: number;
  /** Frames dropped by the display gate in the decoder, cumulative. */
  presentDroppedFrames: number;
  /** Pictures replaced in the hand-off before the sink took them, cumulative. */
  handoffReplacedFrames: number;
  /**
   * The part of the total loss given up while the display's gear was below the stream rate: dropped
   * because the screen cannot show it, which nothing on the client can fix. Cumulative.
   */
  refreshDroppedFrames: number;
  /** The display's current refresh rate gear, 0 when the platform states none. */
  displayRefreshRateHz: number;
  /** Encoded video payload bytes (sum of decoded-unit lengths), cumulative. */
  bytes: number;
  /** Sum of host frame processing latency over frames that carried it (1/10 ms). */
  hostLatencyTenthMs: number;
  /** Number of frames whose host provided a processing latency. */
  hostLatencyFrames: number;
  /** Sum of local decode round-trip (submit to decoded output), microseconds. */
  decodeRoundTripUs: number;
  /** Number of frames paired into the decode round-trip sum. */
  decodeRoundTripFrames: number;
  /** GPU microseconds of the last sampling window: convert, 超分, letterbox. */
  gpuCopyUs: number;
  gpuSrUs: number;
  gpuBlitUs: number;
  /** Display wait of the last sampling window, microseconds. */
  presentWaitUs: number;
  /** Video frames waiting to be decoded right now. */
  pendingVideoFrames: number;
  /** Estimated RTT in ms, or -1 when unavailable. */
  rttMs: number;
  /** RTT variance (jitter) in ms, or -1 when unavailable. */
  rttVarianceMs: number;
  /** Cumulative RTP video packet counts for loss/FEC accounting. */
  packetVideo: number;
  packetFec: number;
  packetFecRecovered: number;
  packetFecFailed: number;
  packetOos: number;
  packetInvalid: number;
}

/** Raw telemetry sample of the single process-global Moonlight session. */
export const moonlightTelemetry: () => MoonlightTelemetry;
