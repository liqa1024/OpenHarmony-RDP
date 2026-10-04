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
 *  - CursorShape: `width` / `height` / `hotX` / `hotY` + `imageHandle`.
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
 *  - ClipboardFilesReady: reserved (downloads are assembled in ArkTS, which
 *    knows its own destination paths).
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
  paths?: string[];
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
 * 快捷键穿透: route the system-intercepted key events (OH_Input_AddKeyEventInterceptor,
 * INTERCEPT_INPUT_EVENT ACL) to this session while enabled, so Win / Alt+Tab / Esc
 * reach the remote. Each arrives as a KeyIntercept event, payload "<keyCode>|<down>".
 * Returns whether the state is in effect (false when registration failed).
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
 * Whether the Moonlight credential store (self-signed client identity + pinned
 * server certificates) is built into this module. "1", or "0|<code>".
 */
export const moonlightCredentialProbe: () => string;

/**
 * Ensures the self-signed Moonlight client identity exists under `dir` (the app
 * sandbox files directory), creating it on first use, and returns
 * "1|<certPem>" or "0|<code>" with a stable code. The private key is stored
 * AES-256-GCM encrypted under a non-exportable HUKS key and never returned.
 */
export const moonlightEnsureCredential: (dir: string) => string;

/**
 * Deletes the Moonlight client identity and every pinned server certificate, so
 * the next connect creates a fresh identity and re-pairs. Returns false when
 * `dir` is empty or the layer is not built.
 */
export const moonlightClearCredential: (dir: string) => boolean;

/** One Moonlight control request (see moonlightHttpRequest). */
export interface MoonlightHttpOptions {
  /** Plain HTTP when false, TLS when true. */
  useTls: boolean;
  host: string;
  port: number;
  /** Path plus query, e.g. "/serverinfo?uniqueid=...". */
  path: string;
  method?: string;
  /** Request body; omit for GET. */
  body?: string;
  contentType?: string;
  /** Overrides the stored per-host pin (hex DER), e.g. during pairing. */
  pinServerCertHex?: string;
  /**
   * Request timeout in ms (default 10000). The pairing `getservercert` step
   * waits for the user to type the PIN on the host, so it passes a long value.
   */
  timeoutMs?: number;
}

export interface MoonlightHttpResult {
  ok: boolean;
  status?: number;
  /** Response body as a UTF-8 string. */
  body?: string;
  /** Leaf certificate (hex DER) of the TLS peer, for pinning. */
  serverCertDer?: string;
  /** Stable error code when ok is false. */
  error?: string;
}

/**
 * Performs one Moonlight control request for the ArkTS protocol layer. For TLS
 * the stored client certificate is attached and the stored per-host server
 * certificate pins the peer (unless pinServerCertHex overrides it). The body
 * comes back as a UTF-8 string; binary responses are not supported yet.
 */
export const moonlightHttpRequest: (dir: string, options: MoonlightHttpOptions) =>
  MoonlightHttpResult;

/** Pins a host's server certificate (hex DER), e.g. after pairing. */
export const moonlightStorePinnedServerCert: (dir: string, host: string, derHex: string) =>
  boolean;

/** The pinned server certificate (hex DER) for a host, or "". */
export const moonlightPinnedServerCertHex: (dir: string, host: string) => string;

export const moonlightClearPinnedServerCert: (dir: string, host: string) => boolean;

// --- pairing crypto primitives (orchestration and XML live in ArkTS) ---

/** AES key = hash(salt || pin)[0:16]; hex in, hex out. */
export const moonlightPairDeriveKey: (serverMajorVersion: number, saltHex: string,
  pin: string) => string;

/** AES-128-ECB, no padding (input zero-padded); hex in, hex out. */
export const moonlightAesEcb: (encrypt: boolean, keyHex: string, dataHex: string) => string;

/** serverchallengeresp value (hash padded to 32 then AES-ECB); hex out. */
export const moonlightPairChallengeResponse: (serverMajorVersion: number, keyHex: string,
  serverChallengeHex: string, clientCertSignatureHex: string, clientSecretHex: string) => string;

/** Expected server response hash for the PIN check; hex out. */
export const moonlightPairExpectedResponse: (serverMajorVersion: number,
  randomChallengeHex: string, serverCertSignatureHex: string, serverSecretHex: string) => string;

/** Raw signature bytes of a certificate (DER or PEM hex in, hex out). */
export const moonlightCertSignature: (certHex: string) => string;

/** Signs the client secret with the stored client key (SHA256withRSA); hex out. */
export const moonlightSignClientSecret: (dir: string, clientSecretHex: string) => string;

/** Verifies a server secret against the server certificate. */
export const moonlightVerifyServerSecret: (serverCertHex: string, serverSecretHex: string,
  serverSignatureHex: string) => boolean;

/** `length` random bytes as hex (1..256). */
export const moonlightRandomHex: (length: number) => string;

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
