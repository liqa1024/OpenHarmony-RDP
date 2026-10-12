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

/**
 * One native session event's payload, as named fields. Every field is optional;
 * a reader takes the ones its event carries (the `event` code discriminates).
 * Native owns the encoding and hands over structured values, so the UI never
 * re-parses a delimited string. Fields, by event:
 *  - Connected / FirstFrame / CursorDefault / CursorHidden: none.
 *  - Disconnected / Error: `code` (FreeRDP error, 0 = internal) + `message`.
 *  - Resize: `width` + `height`.
 *  - CursorShape: `width` / `height` / `hotX` / `hotY` (all as decoded),
 *    `magnificationMilli` (desktop magnification × 1000), `shapeHash` (content
 *    key for dedup) + `imageHandle`. Scaling policy and dedup live in ArkTS
 *    (services/RemoteCursor.ets).
 *  - ClipboardText: `text`.
 *  - ClipboardHtml: `dataHandle` (a raw CF_HTML; ArkTS parses it).
 *  - ClipboardRtf: `dataHandle` (a raw RTF payload; ArkTS converts it).
 *  - RemoteFileListRaw: `dataHandle` (a raw FILEGROUPDESCRIPTORW; ArkTS parses
 *    it and owns the download from there).
 *  - RemoteFileChunk: `listIndex` / `isSize` / `sizeValue` / `failed`, plus
 *    `dataHandle` for a range response (absent for a size response or an
 *    end-of-file marker). ArkTS writes the bytes and drives the next request.
 *  - RemoteClipboardChanged: none. The server's clipboard changed; ArkTS aborts
 *    any download it has running and refreshes its cached descriptors.
 *  - ClipboardImage: `imageHandle` (a raw CF_DIB; ArkTS decodes it).
 *  - FileTransferProgress: `direction` / `doneBytes` / `totalBytes` /
 *    `fileIndex` / `fileCount`.
 *  - FileTransferFailed: `reason`. FileTransferDone: `fileCount`.
 *  - KeyIntercept: `keyCode` + `down`.
 */
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
  /**
   * Handle of a native byte buffer for the event's image: raw BGRA for a cursor
   * shape, raw CF_DIB for a clipboard image. Read it with bufferRead(), then
   * bufferRelease() it.
   */
  imageHandle?: number;
  /**
   * Handle of a native byte buffer for a raw clipboard format payload that is
   * neither an image nor text (currently CF_HTML). Read it with bufferRead(),
   * then bufferRelease() it.
   */
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
  /**
   * RemoteFormatList: the supported clipboard candidates as "kind:id,kind:id"
   * (one entry per kind). ArkTS picks one and calls requestRemoteFormat().
   */
  formatCandidates?: string;
  /** RemoteFormatResponse: the kind of the request that settled. */
  clipKind?: number;
  /** RemoteLockState: the server's KBD_SYNC_* toggle-key bits. */
  lockFlags?: number;
}

export type RdpEventCallback = (handle: number, event: number, payload: RdpEventPayload) => void;

/**
 * One raw telemetry sample of a live session (the native SessionTelemetry): the
 * RDP-thread tick (~1s) reads the transport / frame deltas and drains the work /
 * audio / GPU sources, and computes none of the displayed rates. ArkTS pulls it
 * with sessionTelemetry() and turns it into the toolbar figures itself (rates
 * from the window deltas, per-frame averages from the window sums / windowFrames,
 * duty and audio loss rate). `rttMs` is -1 until the server reports network
 * characteristics; `audioRateHz` is 0 while no audio plays. `sampleSeq` is
 * monotonic, so a reader that sees the same id twice holds the same window and
 * must not fold it into its rolling figures again. No field is a pre-summed
 * total: the reader adds the phases it shows (`syncUs` / `presentWaitUs` are
 * blocked time, the trailing three are GPU time).
 */
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
 * Unconditional Client Synchronize PDU (no dedup). ArkTS uses it to put a server
 * that disagrees with the local toggle keys back on the local state, after it
 * decided the re-assert itself (see the RemoteLockState event / sendLockState for
 * the deduplicated input-path send).
 */
export const sendSynchronize: (handle: number, flags: number) => boolean;

/**
 * 快捷键穿透: route the system-intercepted key events (OH_Input_AddKeyEventInterceptor,
 * INTERCEPT_INPUT_EVENT ACL) to this session while enabled, so Win / Alt+Tab / Esc
 * reach the remote. Each arrives as a KeyIntercept event carrying `keyCode` and
 * `down`. Returns whether the state is in effect (false when registration failed).
 */
export const setShortcutPassthrough: (handle: number, enabled: boolean) => boolean;

export const setClipboardText: (handle: number, text: string) => boolean;

/**
 * Pushes a local HTML clipboard to the session as a ready CF_HTML payload plus
 * its plain-text fallback. ArkTS builds both (ClipboardCodec.buildCfHtml); the
 * native side advertises CF_HTML and serves the fallback for CF_UNICODETEXT.
 */
export const setClipboardHtmlRaw: (handle: number, cfHtml: ArrayBuffer,
  plainText: string) => boolean;

/**
 * Pushes a local image to the session. `dib` is a ready CF_DIB
 * (ClipboardCodec.buildDib); `png` is the same image as a registered "PNG"
 * payload (may be empty). Native advertises PNG first when present and CF_DIB as
 * fallback, then serves whichever format the server requests.
 */
export const setClipboardImage: (handle: number, dib: ArrayBuffer,
  png: ArrayBuffer) => boolean;

/**
 * Advertises a local file offer (FileGroupDescriptorW + FileContents).
 * `descriptor` is a ready FILEGROUPDESCRIPTORW built by ArkTS
 * (FileTransferCodec) and `paths` the absolute sandbox paths, one per line, the
 * bytes are streamed from.
 */
export const setClipboardFileOffer: (handle: number, descriptor: ArrayBuffer,
  paths: string) => boolean;

/**
 * One step of the ArkTS-driven file download. Sends a FILECONTENTS request for
 * the server-advertised `listIndex`: a size query when `sizeOnly` (offset /
 * length ignored), else the byte range at `offset` for `length` bytes. Callers
 * serialize the requests (only one may be in flight). Responses arrive as the
 * RemoteFileChunk event. Recommended range size: 256 KiB.
 */
export const requestRemoteFileContents: (handle: number, listIndex: number,
  sizeOnly: boolean, offset: number, length: number) => boolean;

/**
 * Pulls one remote clipboard format, chosen by ArkTS from a RemoteFormatList
 * event. `formatId` is the server's id for that format and `kind` the content
 * kind (matching the RemoteFormatResponse event). The channel does not tag data
 * responses with their format, so only one request may be in flight; ArkTS
 * serialises them. Returns false when the channel is unavailable.
 */
export const requestRemoteFormat: (handle: number, formatId: number,
  kind: number) => boolean;

/**
 * Aborts an in-flight upload (drops partial state and stops serving the
 * advertised file list). Downloads are cancelled entirely on the ArkTS side.
 */
export const cancelFileTransfer: (handle: number) => boolean;

export const onEvent: (callback: RdpEventCallback) => void;

/**
 * The raw telemetry reading of `handle` (see RdpTelemetry). An unknown handle
 * yields the zeroed shape, so the caller keeps its current figures.
 */
export const sessionTelemetry: (handle: number) => RdpTelemetry;

/**
 * Native byte buffer (a cursor / clipboard bitmap). The bytes stay native; ArkTS
 * reads them once with bufferRead() and then releases the handle. An unknown
 * handle reads as size 0 / an empty ArrayBuffer; bufferRelease is a no-op then.
 */
export const bufferSize: (handle: number) => number;
/** A copy of the buffer's bytes as a JS ArrayBuffer; the handle stays valid. */
export const bufferRead: (handle: number) => ArrayBuffer;
/** Frees the native buffer. Call once after the bytes have been copied out. */
export const bufferRelease: (handle: number) => void;

/**
 * Resamples one native BGRA buffer handle-to-handle (premultiplied area
 * average downscale / Lanczos3 + `sharpen` upscale; the bitmap never crosses
 * to ArkTS for the transform). The source handle is consumed either way;
 * returns the new handle, or 0 when the input was unusable. The target size
 * and sharpen strength are the caller's policy (services/RemoteCursor.ets).
 */
export const scaleBgraBitmap: (handle: number, srcW: number, srcH: number,
  dstW: number, dstH: number, sharpen: number) => number;

/**
 * Pulls PCM from the session's native audio ring into `dest`, the buffer the
 * ArkTS AudioRenderer hands to its `writeData` callback on the audio worker.
 * Fills every byte - queued audio plus silence on an underrun - so the callback
 * can always report a full, valid buffer; returns the number of real bytes
 * copied. Synchronous and allocation-free. An unknown handle reads as 0 and
 * silences the buffer.
 */
export const audioRead: (handle: number, dest: ArrayBuffer) => number;

/** The session's current PCM format (0/0 before the first packet). */
export interface RdpAudioFormat {
  rate: number;
  channels: number;
}

/**
 * The session's current audio format, so a renderer that starts after the
 * AudioFormat event can seed itself instead of staying silent.
 */
export const audioFormat: (handle: number) => RdpAudioFormat;

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
 * Verdict of a capability probe: whether the feature is usable, plus a stable
 * reason code when it is not ("" when supported). The UI owns the wording, so a
 * new reason code does not need a native string change.
 */
export interface RdpVerdict {
  supported: boolean;
  code: string;
}

/**
 * Whether frames can be presented through Vulkan on this device - the capability
 * behind the "硬件加速" setting. `code` is a stable token: `no-vulkan` /
 * `no-instance` / `no-device` / `no-host-memory` / `no-surface` / `emulator`.
 */
export const vulkanAccelSupport: () => RdpVerdict;

/**
 * Whether this device can upscale a presented frame at all (at least one 超分
 * backend usable) - the capability behind the "超分" setting. `code` is one of
 * the `vulkanAccelSupport` codes when the Vulkan presenter itself is unusable,
 * else `no-xengine` (no usable backend). 超分 needs the Vulkan presenter, so the
 * caller must also check `vulkanAccelSupport`.
 */
export const superResolutionSupport: () => RdpVerdict;

/**
 * Which 超分 backends this build and device can run. The UI greys out the ones
 * that are false.
 */
export interface RdpSrBackends {
  xengine: boolean;
  fsr: boolean;
}
export const superResolutionBackends: () => RdpSrBackends;

/**
 * Always "1": the Moonlight (GameStream) client is a required build product, so
 * this only asserts that its static archives really resolved into the module.
 */
export const moonlightProbe: () => string;

/**
 * The single Moonlight VIDEO_FORMAT_* the session will use; 0 when the device
 * and host share no codec. `choice` is an ML_CODEC_* value (0 = auto) and
 * `serverCodecModeSupport` the host's SCM_* mask.
 */
export const moonlightVideoFormats: (choice: number, serverCodecModeSupport: number) => number;

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

/** Everything LiStartConnection needs; assembled by the ArkTS control layer. */
export interface MoonlightSessionConfig {
  host: string;
  serverInfoAppVersion: string;
  serverInfoGfeVersion: string;
  sessionUrl: string;
  serverCodecModeSupport: number;
  width: number;
  height: number;
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
  /**
   * Request foundation-sunshine local cursor mode: the host stops compositing
   * the pointer into the video and forwards it on the cursor channel instead.
   * A no-op when the host does not advertise the capability.
   */
  localCursorMode?: boolean;
  /**
   * Play the host's Opus audio on this device. False (or no output device)
   * leaves the native audio renderer unset so the stream stays silent.
   */
  enableAudio?: boolean;
  /**
   * Play audio through the native OHAudio low-latency path instead of the ArkTS
   * renderer. Only meaningful when enableAudio is true.
   */
  lowLatencyAudio?: boolean;
}

/**
 * Extra fields carried alongside the event tuple. Only the local cursor event
 * (ML_EVENT_CURSOR_SHAPE) fills them; `imageHandle` is a native BGRA buffer to
 * read with bufferRead() and release with bufferRelease().
 */
export interface MoonlightEventExtra {
  width?: number;
  height?: number;
  hotX?: number;
  hotY?: number;
  /** The host-assigned shape identity (protocol-native dedup key). */
  shapeId?: number;
  visible?: boolean;
  hasShape?: boolean;
  imageHandle?: number;
}

/**
 * Registers the process-global session event listener; `event` is an
 * ML_EVENT_* id, `value` the stage or error code, `text` the stage name, `extra`
 * the cursor geometry/bitmap for ML_EVENT_CURSOR_SHAPE.
 */
export const moonlightSetSessionListener: (listener: (event: number, value: number,
  text: string, extra: MoonlightEventExtra) => void) => void;

/** Binds the session window (before start); false when refused. */
export const moonlightSessionSetSurface: (surfaceId: string, width: number,
  height: number) => boolean;

/** Releases the session window; call after moonlightSessionStop. */
export const moonlightSessionClearSurface: () => void;

/** Starts the session; "" on success, else "busy" / "config" / "thread". */
export const moonlightSessionStart: (config: MoonlightSessionConfig) => string;

/** Interrupts/stops the session and waits for its thread to finish. */
export const moonlightSessionStop: () => void;

export const moonlightSessionActive: () => boolean;

/**
 * Pulls decoded PCM from the Moonlight session's native audio ring into `dest`,
 * the buffer the ArkTS AudioRenderer hands to its `writeData` callback on the
 * audio worker. Fills every byte (queued audio plus silence on an underrun) and
 * returns the number of real bytes copied.
 */
export const moonlightAudioRead: (dest: ArrayBuffer) => number;

/**
 * The Moonlight session's decoded PCM format (0/0 before the stream negotiates
 * audio, i.e. when audio is disabled). Known from the Opus decoder config, so a
 * renderer can seed itself even while the host is still silent.
 */
export const moonlightAudioFormat: () => RdpAudioFormat;

/** Sunshine extended query string to append to /launch and /resume. */
export const moonlightLaunchQueryParameters: () => string;

/** Input: relative mouse move. */
export const moonlightMouseMove: (deltaX: number, deltaY: number) => void;

/** Input: absolute mouse position in a reference frame (touch/pointer mapping). */
export const moonlightMousePosition: (x: number, y: number, refWidth: number,
  refHeight: number) => void;

/** Input: mouse button action / button (LiSendMouseButtonEvent constants). */
export const moonlightMouseButton: (action: number, button: number) => void;

/** Input: key code / action / modifiers (LiSendKeyboardEvent constants). */
export const moonlightKey: (keyCode: number, action: number, modifiers: number) => void;

/** Input: wheel clicks (LiSendScrollEvent). */
export const moonlightScroll: (clicks: number) => void;

/** Input: native touchscreen event (LI_TOUCH_EVENT_*), x/y normalized 0..1, pressure 0..1. */
export const moonlightTouch: (eventType: number, pointerId: number, x: number, y: number,
  pressure: number) => void;

/** Input: native precision-touchpad frame (foundation-sunshine); parallel contact arrays. */
export const moonlightTouchpad: (eventTypes: number[], pointerIds: number[], x: number[],
  y: number[], pressure: number[], widthMm: number, heightMm: number,
  buttonState: number) => void;

/** Input: high-resolution vertical wheel in WHEEL_DELTA (120) units. */
export const moonlightHighResScroll: (amount: number) => void;

/** Input: high-resolution horizontal wheel in WHEEL_DELTA (120) units. */
export const moonlightHighResHScroll: (amount: number) => void;

/** Input: composed text sent as one UTF-8 string (LiSendUtf8TextEvent). */
export const moonlightText: (text: string) => void;

/** LI_FF_* host feature bits (0 before a session is established). */
export const moonlightHostFeatures: () => number;

/** Shortcut passthrough for Moonlight; returns the requested state in effect. */
export const moonlightSetShortcutPassthrough: (enabled: boolean) => boolean;

/**
 * One raw telemetry sample of a live Moonlight session. Counters are cumulative:
 * the caller diffs consecutive samples into rates (frames/s, bitrate) and
 * divides the sums by their frame counts for per-frame means (host encode,
 * local decode round trip). RTT and packet counters are read from the library
 * at sample time.
 */
export interface MoonlightTelemetry {
  /** Decoded frames submitted to the decoder, cumulative. */
  frames: number;
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
