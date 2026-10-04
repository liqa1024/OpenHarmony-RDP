/*
 * HmRdp - shared ArkTS data model.
 */
import { RdpOptions } from 'libhmrdp.so';

/**
 * 超分 backend ids. Plain numbers because they are persisted with the connection
 * and cross the N-API boundary as an int; the native side maps 0/1 to
 * SuperResolutionBackend (see doc_agent/present-pipeline.md §4.5).
 *  - XENGINE: the platform's GPU spatial upscale (needs the device feature).
 *  - FSR: the presenter's own FidelityFX Super Resolution 1.0 (Vulkan only).
 */
export const SR_BACKEND_XENGINE: number = 0;
export const SR_BACKEND_FSR: number = 1;

/**
 * The wire protocol a saved connection uses. The connection list holds both
 * kinds and picks the editor form, the card icon and the session toolbar from
 * this value; RDP remains the default so every existing record stays an RDP
 * connection.
 */
export const PROTOCOL_RDP: number = 0;
export const PROTOCOL_MOONLIGHT: number = 1;

/** Moonlight (Sunshine / GameStream) default control port. */
export const ML_DEFAULT_PORT: number = 47989;

/**
 * How a Moonlight stream picks its resolution: follow the session window,
 * follow the host display, or a fixed width/height stored on the connection.
 */
export const ML_RES_FOLLOW_WINDOW: number = 0;
export const ML_RES_FOLLOW_SCREEN: number = 1;
export const ML_RES_FIXED: number = 2;

/** Preferred video codec for a Moonlight stream (0 = let the host decide). */
export const ML_CODEC_AUTO: number = 0;
export const ML_CODEC_H264: number = 1;
export const ML_CODEC_HEVC: number = 2;
export const ML_CODEC_AV1: number = 3;

/** Moonlight audio channel layout. */
export const ML_AUDIO_STEREO: number = 0;
export const ML_AUDIO_51: number = 1;
export const ML_AUDIO_71: number = 2;

/** Offered Moonlight stream frame rates. */
export const ML_FPS_PRESETS: number[] = [30, 60, 90, 120];
/** Moonlight stream bitrate bounds in kbps. */
export const ML_BITRATE_MIN_KBPS: number = 1000;
export const ML_BITRATE_MAX_KBPS: number = 200000;
export const ML_BITRATE_DEFAULT_KBPS: number = 20000;

/**
 * Effective display settings for a session: the output resolution, the session
 * (pre-super-resolution) resolution derived from it, and the DPI scale factor
 * (percentage) requested from the server.
 */
export class DisplayProfile {
  // Output (post-super-resolution) resolution: the user's setting or the current
  // display, and what the native side is given as width/height.
  width: number = 0;
  height: number = 0;
  // Session (pre-super-resolution) resolution: with 超分 on it is the output
  // divided by the ratio, and that is what the server is asked to render.
  sessionWidth: number = 0;
  sessionHeight: number = 0;
  // DPI scale factor of the *session* (pre-super-resolution) desktop.
  scalePercent: number = 100;
  // 超分: the session renders at output ÷ ratio and the presenter upscales it
  // back to the output resolution on the GPU.
  srEnabled: boolean = false;
  srRatioPercent: number = 0;
  // Which upscaler runs (see SR_BACKEND_*). Ignored when srEnabled is false.
  srBackend: number = SR_BACKEND_XENGINE;
  detected: boolean = false;

  valid(): boolean {
    return this.width > 0 && this.height > 0;
  }
}

/**
 * Effective super-resolution defaults for a connection: whether the session
 * resolution is reduced and upscaled locally, and by how much. Resolved from the
 * global defaults unless a connection opted into its own overrides.
 */
export class SrProfile {
  enabled: boolean = false;
  // Upscale ratio in percent (125 = 1.25×). Values <= 100 mean "no upscale".
  ratioPercent: number = 0;
  backend: number = SR_BACKEND_XENGINE;
}

/**
 * Effective per-connection protocol features (audio / certificate handling).
 * Resolved from the global defaults unless a connection opts into its own
 * overrides. Clipboard redirection and the GFX pipeline are not toggles: the
 * manual 复制/粘贴 toolbar actions are always available and the session always
 * uses RDPGFX.
 */
export class AdvancedProfile {
  enableAudio: boolean = true;
  ignoreCertificate: boolean = true;
}

/**
 * One application a paired Moonlight host advertises. Produced by the Moonlight
 * client when the app picker opens; `id` is the host's own app identifier and
 * `hdr` whether the app can output HDR.
 */
export class MoonlightApp {
  id: string = '';
  name: string = '';
  hdr: boolean = false;
}

/**
 * Effective Moonlight stream settings for a connection: the global defaults
 * unless the connection opted into its own overrides (same shape as
 * DisplayProfile / AdvancedProfile for RDP).
 */
export class MoonlightProfile {
  resMode: number = ML_RES_FOLLOW_WINDOW;
  width: number = 0;
  height: number = 0;
  fps: number = 60;
  bitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  hdr: boolean = false;
  codec: number = ML_CODEC_AUTO;
  yuv444: boolean = false;
  audioConfig: number = ML_AUDIO_STEREO;
  gamepad: boolean = true;
}

export enum RdpEvent {
  Connected = 0,
  Disconnected = 1,
  Error = 2,
  FirstFrame = 3,
  ClipboardText = 4,
  Resize = 5,
  // Remote cursor: Shape carries "<w>,<h>,<hotX>,<hotY>|<base64 BGRA>", while
  // Default/Hidden carry no payload.
  CursorShape = 6,
  CursorDefault = 7,
  CursorHidden = 8,
  // Rich remote clipboard payloads. Html carries a raw CF_HTML payload and
  // Image a raw CF_DIB (both as a native buffer handle); ClipboardCodec decodes
  // them. Text arrives already decoded on ClipboardText.
  ClipboardHtml = 9,
  ClipboardImage = 10,
  // Reserved. Per-second telemetry is not an event: raw readings are pulled with
  // sessionTelemetry() and aggregated in ArkTS (see services/SessionTelemetry), so
  // nothing emits this. The id is kept only to preserve the numbering of the
  // events that follow it.
  Metrics = 11,
  // Reserved. The remote file list now arrives as RemoteFileListRaw (a raw
  // FILEGROUPDESCRIPTORW) that ArkTS parses; this id is kept for numbering.
  ClipboardFileList = 12,
  // "<direction>|<done>|<total>|<fileIndex>|<fileCount>", direction 0 = remote ->
  // local, 1 = local -> remote.
  FileTransferProgress = 13,
  // Download finished: the absolute sandbox paths, one per line.
  ClipboardFilesReady = 14,
  // Transfer aborted before completing; payload is the reason.
  FileTransferFailed = 15,
  // Upload finished (every advertised byte served); payload is the file count.
  FileTransferDone = 16,
  // 快捷键穿透: an intercepted key event, payload "<keyCode>|<down>".
  KeyIntercept = 17,
  // RTF clipboard content; payload is a raw RTF buffer the codec converts.
  ClipboardRtf = 18,
  // Raw FILEGROUPDESCRIPTORW (a native buffer handle); ArkTS parses it and owns
  // the download state machine (FileDownload).
  RemoteFileListRaw = 19,
  // One FILECONTENTS response relayed from native: listIndex / isSize /
  // sizeValue / failed, plus a data buffer handle for a range response. ArkTS
  // writes the bytes and issues the next request.
  RemoteFileChunk = 20,
  // The server's clipboard changed; ArkTS aborts a running download and refreshes
  // its cached remote descriptors.
  RemoteClipboardChanged = 21,
  // The session's audio format changed (first packet or a rate/channel switch):
  // audioRate / audioChannels name the PCM the ArkTS AudioRenderer must open.
  AudioFormat = 22,
  // The server offered a clipboard format list; formatCandidates carries the
  // supported kinds (see RdpClipKind). ArkTS owns the pick and the request.
  RemoteFormatList = 23,
  // One clipboard data request settled (clipKind / failed): ArkTS issues whatever
  // it deferred while the request was in flight.
  RemoteFormatResponse = 24,
  // The remote session's toggle-key state (lockFlags); ArkTS compares it with its
  // own local state and re-asserts through RdpNative.reassertLockState.
  RemoteLockState = 25
}

export class RdpConnectOptions implements RdpOptions {
  host: string = '';
  port: number = 3389;
  // Optional user-set name for this target. Empty means "no custom name", and
  // every display falls back to host / host:port.
  name: string = '';
  username: string = '';
  password: string = '';
  domain: string = '';
  width: number = 1920;
  height: number = 1080;
  scalePercent: number = 0;
  ignoreCertificate: boolean = true;
  enableAudio: boolean = true;
  // 超分: width/height are the *output* resolution; when this is on the native
  // side asks the server for output ÷ srRatioPercent and upscales each frame
  // back to the output resolution on the GPU (Vulkan presenter only).
  srEnabled: boolean = false;
  srRatioPercent: number = 0;
  // Which upscaler runs: SR_BACKEND_XENGINE or SR_BACKEND_FSR.
  srBackend: number = SR_BACKEND_XENGINE;
  gatewayHost: string = '';
  gatewayPort: number = 443;
  gatewayUsername: string = '';
  gatewayPassword: string = '';
  gatewayDomain: string = '';

  static of(host: string, port: number, username: string, password: string,
    domain: string): RdpConnectOptions {
    const options: RdpConnectOptions = new RdpConnectOptions();
    options.host = host;
    options.port = port;
    options.username = username;
    options.password = password;
    options.domain = domain;
    return options;
  }

  /** Name to show for this target: the custom name when set, else the host. */
  displayName(): string {
    return this.name.length > 0 ? this.name : this.host;
  }

  /** "name · host:port" when a custom name is set, else just "host:port". */
  targetLabel(): string {
    const target: string = `${this.host}:${this.port}`;
    return this.name.length > 0 ? `${this.name} · ${target}` : target;
  }
}

export enum RdpConnectOrigin {
  List = 0,
  Editor = 1
}

export class PendingConnection {
  options: RdpConnectOptions = new RdpConnectOptions();
  connectionId: string = '';
  origin: RdpConnectOrigin = RdpConnectOrigin.List;
  // Protocol of this launch (PROTOCOL_RDP / PROTOCOL_MOONLIGHT); decides which
  // SessionBackend the manager builds.
  protocol: number = PROTOCOL_RDP;
  // The saved record a Moonlight launch resolves its stream settings from; RDP
  // keeps everything it needs in `options`.
  savedConnection: SavedConnection | undefined = undefined;
  // Effective Moonlight stream settings (SettingsStore.resolveStream), used when
  // the native Moonlight session is built.
  stream: MoonlightProfile = new MoonlightProfile();
  // Invoked once when a session launched from the editor connects successfully.
  // Cleared afterwards so it never fires twice or leaks across launches.
  onConnected: (() => void) | undefined = undefined;
}

/**
 * Launch requests are keyed by a per-window key that travels through the Want,
 * so concurrent session launches never overwrite each other.
 */
export class SessionRequests {
  private static entries: Map<string, PendingConnection> = new Map<string, PendingConnection>();
  private static counter: number = 0;

  static newKey(): string {
    SessionRequests.counter++;
    return `${Date.now().toString()}#${SessionRequests.counter.toString()}`;
  }

  static put(key: string, request: PendingConnection): void {
    SessionRequests.entries.set(key, request);
  }

  static get(key: string): PendingConnection | undefined {
    return SessionRequests.entries.get(key);
  }

  /** Scrubs the transient credentials and drops the request for a session. */
  static discard(key: string): void {
    const request: PendingConnection | undefined = SessionRequests.entries.get(key);
    if (request !== undefined) {
      request.options.password = '';
      request.options.gatewayPassword = '';
      request.onConnected = undefined;
      SessionRequests.entries.delete(key);
    }
  }
}

export class RdpPointerFlags {
  static readonly MOVE: number = 0x0800;
  static readonly DOWN: number = 0x8000;
  static readonly BUTTON1: number = 0x1000;
  static readonly BUTTON2: number = 0x2000;
  static readonly BUTTON3: number = 0x4000;
  static readonly WHEEL: number = 0x0200;
  static readonly WHEEL_NEGATIVE: number = 0x0100;
  static readonly HWHEEL: number = 0x0400;
}

/**
 * Toggle-key bits of the RDP synchronize/indicator PDUs (KBD_SYNC_*). They are
 * the same values in both directions: a Client Synchronize PDU tells the server
 * which lock keys the local keyboard has on, and the server's Set Keyboard
 * Indicators PDU reports the remote session's state.
 */
export class RdpLockFlags {
  static readonly SCROLL: number = 0x01;
  static readonly NUM: number = 0x02;
  static readonly CAPS: number = 0x04;
}

/**
 * Content kind of a clipboard format, mirroring the native Session::LocalClipKind.
 * Native classifies the server's formats into these and reports the candidates;
 * ArkTS picks which kind to pull and passes it back on the request.
 */
export class RdpClipKind {
  static readonly NONE: number = 0;
  static readonly TEXT: number = 1;
  static readonly HTML: number = 2;
  static readonly IMAGE: number = 3;
  static readonly RTF: number = 4;
  static readonly FILES: number = 5;
}

export class RdpTouchFlags {
  static readonly DOWN: number = 0x01;
  static readonly UP: number = 0x02;
  static readonly MOTION: number = 0x04;
  // FreeRDP keeps pressure only when this bit is set (FREERDP_TOUCH_HAS_PRESSURE).
  static readonly HAS_PRESSURE: number = 0x100;
}

export class SavedConnection extends RdpConnectOptions {
  id: string = '';
  updatedAt: number = 0;
  // When true the connection follows the global display settings and ignores
  // width / height / scalePercent; when false the stored values are used.
  useGlobalDisplay: boolean = true;
  // When true the connection follows the global advanced settings (audio /
  // certificate); when false its own stored values win.
  useGlobalAdvanced: boolean = true;
  // When true the connection follows the global super-resolution settings; when
  // false its own srEnabled / srRatioPercent win. Kept separate from the display
  // group so a LAN connection can stay native-resolution while the same machine's
  // gateway (frp) connection turns 超分 on.
  useGlobalSr: boolean = true;

  // Protocol this record connects with (PROTOCOL_RDP / PROTOCOL_MOONLIGHT).
  protocol: number = PROTOCOL_RDP;
  // --- Moonlight (Sunshine) stream settings. ---
  // Default application the row's connect button opens; empty means the host's
  // desktop. The app picker connects to another app once without changing it.
  mlDefaultAppId: string = '';
  mlDefaultAppName: string = '';
  // Last known pairing state; the authoritative pairing is the client certificate
  // the native Moonlight client keeps, so this is only a hint for the UI. A
  // connection can be unpaired even when this is true (host reinstalled, config
  // imported), which is why pairing is re-checked on every connect.
  mlPaired: boolean = false;
  // When true the connection follows the global Moonlight stream defaults; when
  // false its own ml* stream values win (same pattern as useGlobalDisplay).
  useGlobalStream: boolean = true;
  mlResMode: number = ML_RES_FOLLOW_WINDOW;
  mlWidth: number = 0;
  mlHeight: number = 0;
  mlFps: number = 60;
  mlBitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  mlHdr: boolean = false;
  mlCodec: number = ML_CODEC_AUTO;
  mlYuv444: boolean = false;
  mlAudioConfig: number = ML_AUDIO_STEREO;
  mlGamepad: boolean = true;

  isMoonlight(): boolean {
    return this.protocol === PROTOCOL_MOONLIGHT;
  }

  label(): string {
    return this.name.length > 0 ? this.name : `${this.host}:${this.port}`;
  }

  static fromOptions(id: string, options: RdpConnectOptions): SavedConnection {
    const conn: SavedConnection = new SavedConnection();
    conn.id = id;
    conn.host = options.host;
    conn.port = options.port;
    conn.name = options.name;
    conn.username = options.username;
    conn.domain = options.domain;
    conn.width = options.width;
    conn.height = options.height;
    conn.scalePercent = options.scalePercent;
    conn.ignoreCertificate = options.ignoreCertificate;
    conn.enableAudio = options.enableAudio;
    conn.srEnabled = options.srEnabled;
    conn.srRatioPercent = options.srRatioPercent;
    conn.srBackend = options.srBackend;
    conn.gatewayHost = options.gatewayHost;
    conn.gatewayPort = options.gatewayPort;
    conn.gatewayUsername = options.gatewayUsername;
    conn.gatewayDomain = options.gatewayDomain;
    return conn;
  }

  toOptions(): RdpConnectOptions {
    const options: RdpConnectOptions =
      RdpConnectOptions.of(this.host, this.port, this.username, '', this.domain);
    options.name = this.name;
    options.width = this.width;
    options.height = this.height;
    options.scalePercent = this.scalePercent;
    options.ignoreCertificate = this.ignoreCertificate;
    options.enableAudio = this.enableAudio;
    options.srEnabled = this.srEnabled;
    options.srRatioPercent = this.srRatioPercent;
    options.srBackend = this.srBackend;
    options.gatewayHost = this.gatewayHost;
    options.gatewayPort = this.gatewayPort;
    options.gatewayUsername = this.gatewayUsername;
    options.gatewayPassword = this.gatewayPassword;
    options.gatewayDomain = this.gatewayDomain;
    return options;
  }
}

export interface EditConnectionParams {
  id: string;
  // Protocol a brand-new connection starts on (PROTOCOL_RDP / PROTOCOL_MOONLIGHT).
  // Ignored when `id` names an existing connection, whose own protocol wins.
  protocol: number;
}

