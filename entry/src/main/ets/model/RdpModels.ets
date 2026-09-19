/*
 * HmRdp - shared ArkTS data model.
 */
import { RdpOptions } from 'libhmrdp.so';

/**
 * Effective display settings for a session: the remote desktop resolution plus
 * the DPI scale factor (percentage) requested from the server.
 */
export class DisplayProfile {
  width: number = 0;
  height: number = 0;
  scalePercent: number = 100;
  detected: boolean = false;

  valid(): boolean {
    return this.width > 0 && this.height > 0;
  }
}

/**
 * Effective per-connection protocol features (audio / graphics pipeline /
 * H.264 / certificate handling). Resolved from the global defaults unless a
 * connection opts into its own overrides. Clipboard redirection is not a
 * feature toggle: the manual 复制/粘贴 toolbar actions are always available.
 */
export class AdvancedProfile {
  enableAudio: boolean = true;
  enableGfx: boolean = true;
  enableH264: boolean = true;
  ignoreCertificate: boolean = true;
  // Frame-rate cap in frames per second; 0 leaves the remote frame rate to the
  // server. Frames above the cap are never decoded (see RdpOptions.maxFps).
  maxFps: number = 0;
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
  // Rich remote clipboard payloads. Html carries an HTML fragment; Image
  // carries "<w>,<h>|<base64 BGRA>".
  ClipboardHtml = 9,
  ClipboardImage = 10,
  // Per-second telemetry, payload
  // "<rttMs>|<rxBps>|<txBps>|<fps>|<audioRateHz>|<audioLossBp>|<zgxParseUs>|
  //  <decodeUs>|<composeUs>|<presentUs>|<bytesPerFrame>|<dutyPermille>|
  //  <cmdsPerFrame>|<syncUs>|<presentWaitUs>".
  // rttMs is -1 while the server has not reported network characteristics;
  // audioRateHz and audioLossBp (0.01% units) describe the recent audio playback
  // health, both -1 when audio is not playing. The fields from zgxParseUs on are
  // the per-frame work phase by phase, averaged over the frames the server ended,
  // plus the normalization (GFX commands and raw GFX bytes per frame, duty cycle
  // in 0.1% units), and the two blocked sub-items at the end (`syncUs`: waiting
  // for the desktop buffer's GPU read; `presentWaitUs`: waiting for the display).
  // There is no pre-summed client-work total: the toolbar sums the four work
  // phases and leaves the blocked ones out.
  Metrics = 11,
  // Remote clipboard holds files: payload "<count>|<totalBytes>".
  ClipboardFileList = 12,
  // "<direction>|<doneBytes>|<totalBytes>", direction 0 = remote -> local,
  // 1 = local -> remote.
  FileTransferProgress = 13,
  // Download finished: the absolute sandbox paths, one per line.
  ClipboardFilesReady = 14,
  // Transfer aborted before completing; payload is the reason.
  FileTransferFailed = 15,
  // Upload finished (every advertised byte served); payload is the file count.
  FileTransferDone = 16
}

export class RdpConnectOptions implements RdpOptions {
  host: string = '';
  port: number = 3389;
  username: string = '';
  password: string = '';
  domain: string = '';
  width: number = 1920;
  height: number = 1080;
  scalePercent: number = 0;
  colorDepth: number = 32;
  ignoreCertificate: boolean = true;
  enableAudio: boolean = true;
  enableGfx: boolean = true;
  enableH264: boolean = true;
  enableRemoteFx: boolean = true;
  // Frame-rate cap in frames per second (0 = uncapped).
  maxFps: number = 0;
  performanceFlags: number = 0;
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
}

export enum RdpConnectOrigin {
  List = 0,
  Editor = 1
}

export class PendingConnection {
  options: RdpConnectOptions = new RdpConnectOptions();
  connectionId: string = '';
  origin: RdpConnectOrigin = RdpConnectOrigin.List;
  // Invoked once when a session launched from the editor connects successfully.
  // Cleared afterwards so it never fires twice or leaks across launches.
  onConnected: (() => void) | undefined = undefined;
}

/**
 * Launch requests are keyed by a per-window key that travels through the Want,
 * so concurrent session launches never overwrite each other (the old
 * PendingConnection.current singleton raced when two windows opened at once).
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

export enum RdpConnectionState {
  Idle = 0,
  Connecting = 1,
  Connected = 2,
  Failed = 3,
  Closed = 4
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
  // When true the connection follows the global advanced settings (clipboard /
  // audio / GFX / H.264 / certificate); when false its own stored values win.
  useGlobalAdvanced: boolean = true;

  label(): string {
    return `${this.host}:${this.port}`;
  }

  static fromOptions(id: string, options: RdpConnectOptions): SavedConnection {
    const conn: SavedConnection = new SavedConnection();
    conn.id = id;
    conn.host = options.host;
    conn.port = options.port;
    conn.username = options.username;
    conn.domain = options.domain;
    conn.width = options.width;
    conn.height = options.height;
    conn.scalePercent = options.scalePercent;
    conn.colorDepth = options.colorDepth;
    conn.ignoreCertificate = options.ignoreCertificate;
    conn.enableAudio = options.enableAudio;
    conn.enableGfx = options.enableGfx;
    conn.enableH264 = options.enableH264;
    conn.enableRemoteFx = options.enableRemoteFx;
    conn.maxFps = options.maxFps;
    conn.performanceFlags = options.performanceFlags;
    conn.gatewayHost = options.gatewayHost;
    conn.gatewayPort = options.gatewayPort;
    conn.gatewayUsername = options.gatewayUsername;
    conn.gatewayDomain = options.gatewayDomain;
    return conn;
  }

  toOptions(): RdpConnectOptions {
    const options: RdpConnectOptions =
      RdpConnectOptions.of(this.host, this.port, this.username, '', this.domain);
    options.width = this.width;
    options.height = this.height;
    options.scalePercent = this.scalePercent;
    options.colorDepth = this.colorDepth;
    options.ignoreCertificate = this.ignoreCertificate;
    options.enableAudio = this.enableAudio;
    options.enableGfx = this.enableGfx;
    options.enableH264 = this.enableH264;
    options.enableRemoteFx = this.enableRemoteFx;
    options.maxFps = this.maxFps;
    options.performanceFlags = this.performanceFlags;
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
}

