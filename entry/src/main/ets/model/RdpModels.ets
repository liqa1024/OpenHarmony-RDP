import { MoonlightSessionConfig, RdpOptions } from 'libhmrdp.so';
import { PairResult } from '../moonlight/MoonlightModels';

export const SR_BACKEND_XENGINE: number = 0;
export const SR_BACKEND_FSR: number = 1;

export const PROTOCOL_RDP: number = 0;
export const PROTOCOL_MOONLIGHT: number = 1;

export const ML_DEFAULT_PORT: number = 47989;

// ML_RES_FOLLOW_WINDOW 是退役值，保留仅让旧记录能解析（按 FOLLOW_SCREEN 处理）。
export const ML_RES_FOLLOW_WINDOW: number = 0;
export const ML_RES_FOLLOW_SCREEN: number = 1;
export const ML_RES_FIXED: number = 2;

export const ML_CODEC_AUTO: number = 0;
export const ML_CODEC_H264: number = 1;
export const ML_CODEC_HEVC: number = 2;
export const ML_CODEC_AV1: number = 3;

export const ML_FPS_PRESETS: number[] = [30, 60, 90, 120];
export const ML_BITRATE_MIN_KBPS: number = 1000;
export const ML_BITRATE_MAX_KBPS: number = 200000;
export const ML_BITRATE_DEFAULT_KBPS: number = 20000;

export class DisplayProfile {
  width: number = 0;
  height: number = 0;
  sessionWidth: number = 0;
  sessionHeight: number = 0;
  scalePercent: number = 100;
  srEnabled: boolean = false;
  srRatioPercent: number = 0;
  srBackend: number = SR_BACKEND_XENGINE;
  detected: boolean = false;

  valid(): boolean {
    return this.width > 0 && this.height > 0;
  }
}

export class SrProfile {
  enabled: boolean = false;
  ratioPercent: number = 0;
  backend: number = SR_BACKEND_XENGINE;
}

export class AdvancedProfile {
  enableAudio: boolean = true;
  ignoreCertificate: boolean = true;
}

export class MoonlightProfile {
  resMode: number = ML_RES_FOLLOW_SCREEN;
  width: number = 0;
  height: number = 0;
  fps: number = 60;
  bitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  codec: number = ML_CODEC_AUTO;
}

export class MoonlightStreamConfig implements MoonlightSessionConfig {
  host: string = '';
  serverInfoAppVersion: string = '';
  serverInfoGfeVersion: string = '';
  sessionUrl: string = '';
  serverCodecModeSupport: number = 0;
  width: number = 1920;
  height: number = 1080;
  fps: number = 60;
  bitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  packetSize: number = 1024;
  streamingRemotely: boolean = false;
  audioConfiguration: number = 0;
  supportedVideoFormats: number = 0;
  clientRefreshRateX100: number = 0;
  colorSpace: number = 0;
  colorRange: number = 0;
  encryptionFlags: number = 0;
  remoteInputAesKeyHex: string = '';
  remoteInputAesIvHex: string = '';
  localCursorMode: boolean = false;
}

export enum RdpEvent {
  Connected = 0,
  Disconnected = 1,
  Error = 2,
  FirstFrame = 3,
  ClipboardText = 4,
  Resize = 5,
  CursorShape = 6,
  CursorDefault = 7,
  CursorHidden = 8,
  ClipboardHtml = 9,
  ClipboardImage = 10,
  FileTransferProgress = 11,
  FileTransferFailed = 12,
  FileTransferDone = 13,
  KeyIntercept = 14,
  ClipboardRtf = 15,
  RemoteFileListRaw = 16,
  RemoteFileChunk = 17,
  RemoteClipboardChanged = 18,
  AudioFormat = 19,
  RemoteFormatList = 20,
  RemoteFormatResponse = 21,
  RemoteLockState = 22
}

export class RdpConnectOptions implements RdpOptions {
  host: string = '';
  port: number = 3389;
  name: string = '';
  username: string = '';
  password: string = '';
  domain: string = '';
  width: number = 1920;
  height: number = 1080;
  scalePercent: number = 0;
  ignoreCertificate: boolean = true;
  enableAudio: boolean = true;
  srEnabled: boolean = false;
  srRatioPercent: number = 0;
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

  displayName(): string {
    return this.name.length > 0 ? this.name : this.host;
  }

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
  protocol: number = PROTOCOL_RDP;
  savedConnection: SavedConnection | undefined = undefined;
  stream: MoonlightProfile = new MoonlightProfile();
  moonlightConfig: MoonlightStreamConfig | undefined = undefined;
  moonlightHttpsPort: number = 47984;
  filesDir: string = '';
  moonlightAppId: number = 0;
  moonlightPinProvider: ((pin: string, pairing: Promise<PairResult>) => Promise<boolean>)
    | undefined = undefined;
  onConnected: (() => void) | undefined = undefined;
}

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

  static discard(key: string): void {
    const request: PendingConnection | undefined = SessionRequests.entries.get(key);
    if (request !== undefined) {
      request.options.password = '';
      request.options.gatewayPassword = '';
      request.onConnected = undefined;
      request.moonlightPinProvider = undefined;
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

// KBD_SYNC_*，双向同值：客户端 Synchronize PDU 上报本机锁定键，服务器的 Set Keyboard Indicators PDU 回报远端状态。
export class RdpLockFlags {
  static readonly SCROLL: number = 0x01;
  static readonly NUM: number = 0x02;
  static readonly CAPS: number = 0x04;
}

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
  useGlobalDisplay: boolean = true;
  useGlobalAdvanced: boolean = true;
  useGlobalSr: boolean = true;
  // 仅 useGlobalDisplay 为 false 时有意义；两个标志都为 false = 照用存储值（旧记录即如此）。
  resolutionAuto: boolean = false;
  scaleAuto: boolean = false;

  protocol: number = PROTOCOL_RDP;
  mlDefaultAppId: string = '';
  mlDefaultAppName: string = '';
  // 主机自身身份（/serverinfo 的 uniqueid）：Moonlight 以此区分主机，故为唯一性检查的首选键；首次 serverinfo 成功后填入，此前为空。
  mlHostId: string = '';
  useGlobalStream: boolean = true;
  mlResMode: number = ML_RES_FOLLOW_SCREEN;
  mlWidth: number = 0;
  mlHeight: number = 0;
  mlFps: number = 60;
  mlBitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  mlCodec: number = ML_CODEC_AUTO;

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
  // id 指向已有连接时忽略（已有连接以自己的协议为准）。
  protocol: number;
}

