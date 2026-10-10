import { MoonlightSessionConfig } from 'libhmrdp.so';
import { PairResult } from '../moonlight/MoonlightModels';
import { RdpConnectOptions, SR_BACKEND_XENGINE } from './RdpModels';

// Shared connection / session-request model, independent of the protocol. The
// abstract base holds only what both protocols share; RDP-only and Moonlight-only
// fields live on their subclasses, and each subclass owns its slice of the
// position-separated record / export so the codec services stay protocol-neutral.

// Connection action vocabulary plus its display labels.
export enum ConnectAction {
  Connect = 0,
  Resume = 1,
  Switch = 2,
  Terminate = 3,
  Cancel = 4,
  Disconnect = 5,
  Busy = 6
}

export function connectActionLabel(action: ConnectAction): string {
  if (action === ConnectAction.Cancel) {
    return '取消连接';
  }
  if (action === ConnectAction.Busy) {
    return '正在断开';
  }
  if (action === ConnectAction.Terminate) {
    return '终止';
  }
  if (action === ConnectAction.Disconnect) {
    return '断开连接';
  }
  if (action === ConnectAction.Resume) {
    return '恢复';
  }
  if (action === ConnectAction.Switch) {
    return '切换';
  }
  return '连接';
}

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

// Position-separated record length; slots are fixed and append-only (see
// settings-and-storage.md §1). Shared slots are written by the base, each
// protocol's slots by its subclass.
const RECORD_LENGTH: number = 45;

function encodeField(text: string): string {
  try {
    return encodeURIComponent(text);
  } catch (error) {
    return text;
  }
}

function decodeField(text: string): string {
  try {
    return decodeURIComponent(text);
  } catch (error) {
    return text;
  }
}

function parseFieldNumber(text: string, fallback: number): number {
  const value: number = Number.parseInt(text, 10);
  return Number.isNaN(value) ? fallback : value;
}

function parseFieldPositive(text: string, fallback: number): number {
  const value: number = Number.parseInt(text, 10);
  return Number.isNaN(value) || value <= 0 ? fallback : value;
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

export enum ConnectOrigin {
  List = 0,
  Editor = 1
}

export class PendingConnection {
  options: RdpConnectOptions = new RdpConnectOptions();
  connectionId: string = '';
  origin: ConnectOrigin = ConnectOrigin.List;
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

export class RowMenuAction {
  static readonly Toggle: number = 0;
  static readonly Terminate: number = 1;
  static readonly Edit: number = 2;
  static readonly Delete: number = 3;
}

// One connection-row menu entry as data, so the shared row renders it without
// knowing the protocol; the page maps `action` back to its callbacks.
export class RowMenuEntry {
  action: number = RowMenuAction.Toggle;
  content: string = '';

  static of(action: number, content: string): RowMenuEntry {
    const entry: RowMenuEntry = new RowMenuEntry();
    entry.action = action;
    entry.content = content;
    return entry;
  }
}

// Flat, password-free shape used by config export/import. Kept on the model side
// so the (de)serialization stays polymorphic and the transfer service stays
// protocol-neutral.
export class ConnectionExport {
  id: string = '';
  host: string = '';
  port: number = 0;
  name: string = '';
  protocol: number = PROTOCOL_RDP;
  // RDP-only.
  username: string = '';
  domain: string = '';
  width: number = 0;
  height: number = 0;
  scalePercent: number = 0;
  ignoreCertificate: boolean = true;
  enableAudio: boolean = true;
  srEnabled: boolean = false;
  srRatioPercent: number = 0;
  srBackend: number = SR_BACKEND_XENGINE;
  gatewayHost: string = '';
  gatewayPort: number = 443;
  gatewayUsername: string = '';
  gatewayDomain: string = '';
  useGlobalDisplay: boolean = true;
  useGlobalAdvanced: boolean = true;
  useGlobalSr: boolean = true;
  resolutionAuto: boolean = false;
  scaleAuto: boolean = false;
  // Moonlight-only.
  mlDefaultAppId: string = '';
  mlDefaultAppName: string = '';
  mlHostId: string = '';
  mlResMode: number = ML_RES_FOLLOW_SCREEN;
  mlWidth: number = 0;
  mlHeight: number = 0;
  mlFps: number = 60;
  mlBitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  mlCodec: number = ML_CODEC_AUTO;
  useGlobalStream: boolean = true;
}

// Shared abstract base: only the fields both protocols share. Each protocol
// subclass owns its own fields and its own slice of the record / export.
export abstract class SavedConnection {
  id: string = '';
  updatedAt: number = 0;
  host: string = '';
  port: number = 0;
  name: string = '';
  protocol: number = PROTOCOL_RDP;

  static create(protocol: number): SavedConnection {
    return protocol === PROTOCOL_MOONLIGHT ? new MoonlightConnection() : new RdpConnection();
  }

  static fromExport(item: ConnectionExport): SavedConnection {
    const conn: SavedConnection = SavedConnection.create(item.protocol);
    conn.applyExport(item);
    conn.updatedAt = Date.now();
    return conn;
  }

  abstract protocolIcon(): Resource;
  abstract subtitle(): string;
  abstract primaryAction(action: ConnectAction): ConnectAction;
  abstract showsAppsButton(): boolean;
  abstract hostTargetId(targets: Map<string, number>): number;
  abstract defaultTargetId(): number;
  abstract menuEntries(action: ConnectAction, hostTargetId: number): RowMenuEntry[];

  protected defaultPort(): number {
    return 3389;
  }

  label(): string {
    return this.name.length > 0 ? this.name : `${this.host}:${this.port}`;
  }

  displayName(): string {
    return this.name.length > 0 ? this.name : this.host;
  }

  targetLabel(): string {
    const target: string = `${this.host}:${this.port}`;
    return this.name.length > 0 ? `${this.name} · ${target}` : target;
  }

  // Position-separated record codec: shared + retired slots here, protocol slots
  // in the subclass override. The storage service stays protocol-neutral.
  toRecord(): string[] {
    const slots: string[] = new Array<string>(RECORD_LENGTH).fill('');
    slots[0] = encodeField(this.host);
    slots[1] = this.port.toString();
    // 退役槽位写固定值，保证位置布局（含老记录）稳定。
    slots[6] = '32'; slots[8] = '1'; slots[10] = '1'; slots[11] = '0';
    slots[12] = '1'; slots[13] = '0'; slots[30] = '0'; slots[36] = '0';
    slots[38] = '0'; slots[39] = '0'; slots[40] = '0';
    slots[18] = this.updatedAt.toString();
    slots[26] = encodeField(this.name);
    slots[27] = this.protocol.toString();
    this.writeProtocolRecord(slots);
    return slots;
  }

  readRecord(parts: string[]): void {
    this.host = decodeField(parts[0]);
    this.port = parseFieldPositive(parts[1], this.defaultPort());
    this.updatedAt = parseFieldNumber(parts[18], 0);
    this.name = parts.length > 26 ? decodeField(parts[26]) : '';
    this.protocol = parts.length > 27 ? parseFieldNumber(parts[27], PROTOCOL_RDP) : PROTOCOL_RDP;
    this.readProtocolRecord(parts);
  }

  toExport(): ConnectionExport {
    const item: ConnectionExport = new ConnectionExport();
    item.id = this.id;
    item.host = this.host;
    item.port = this.port;
    item.name = this.name;
    item.protocol = this.protocol;
    this.writeProtocolExport(item);
    return item;
  }

  applyExport(item: ConnectionExport): void {
    this.id = item.id;
    this.host = item.host;
    this.port = item.port > 0 ? item.port : this.defaultPort();
    this.name = item.name;
    this.protocol = item.protocol;
    this.readProtocolExport(item);
  }

  protected abstract writeProtocolRecord(slots: string[]): void;
  protected abstract readProtocolRecord(parts: string[]): void;
  protected abstract writeProtocolExport(item: ConnectionExport): void;
  protected abstract readProtocolExport(item: ConnectionExport): void;
}

export class RdpConnection extends SavedConnection {
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
  useGlobalDisplay: boolean = true;
  useGlobalAdvanced: boolean = true;
  useGlobalSr: boolean = true;
  // 仅 useGlobalDisplay 为 false 时有意义；两个标志都为 false = 照用存储值（旧记录即如此）。
  resolutionAuto: boolean = false;
  scaleAuto: boolean = false;

  protected defaultPort(): number {
    return 3389;
  }

  protocolIcon(): Resource {
    return $r('app.media.ic_remote_desktop');
  }

  subtitle(): string {
    const target: string = `${this.host}:${this.port}`;
    if (this.name.length === 0) {
      return this.username;
    }
    return this.username.length > 0 ? `${target} · ${this.username}` : target;
  }

  primaryAction(action: ConnectAction): ConnectAction {
    return action;
  }

  showsAppsButton(): boolean {
    return false;
  }

  hostTargetId(_targets: Map<string, number>): number {
    return 0;
  }

  defaultTargetId(): number {
    return 0;
  }

  menuEntries(action: ConnectAction, _hostTargetId: number): RowMenuEntry[] {
    return [
      RowMenuEntry.of(RowMenuAction.Toggle, connectActionLabel(action)),
      RowMenuEntry.of(RowMenuAction.Edit, '编辑'),
      RowMenuEntry.of(RowMenuAction.Delete, '删除')
    ];
  }

  protected writeProtocolRecord(slots: string[]): void {
    slots[2] = encodeField(this.username);
    slots[3] = encodeField(this.domain);
    slots[4] = this.width.toString();
    slots[5] = this.height.toString();
    slots[7] = this.ignoreCertificate ? '1' : '0';
    slots[9] = this.enableAudio ? '1' : '0';
    slots[14] = encodeField(this.gatewayHost);
    slots[15] = this.gatewayPort.toString();
    slots[16] = encodeField(this.gatewayUsername);
    slots[17] = encodeField(this.gatewayDomain);
    slots[19] = this.useGlobalDisplay ? '1' : '0';
    slots[20] = this.scalePercent.toString();
    slots[21] = this.useGlobalAdvanced ? '1' : '0';
    slots[22] = this.useGlobalSr ? '1' : '0';
    slots[23] = this.srEnabled ? '1' : '0';
    slots[24] = this.srRatioPercent.toString();
    slots[25] = this.srBackend.toString();
    slots[43] = this.resolutionAuto ? '1' : '0';
    slots[44] = this.scaleAuto ? '1' : '0';
  }

  protected readProtocolRecord(parts: string[]): void {
    this.username = decodeField(parts[2]);
    this.domain = decodeField(parts[3]);
    this.width = parseFieldPositive(parts[4], 1920);
    this.height = parseFieldPositive(parts[5], 1080);
    this.ignoreCertificate = parts[7] === '1';
    this.enableAudio = parts[9] === '1';
    this.gatewayHost = decodeField(parts[14]);
    this.gatewayPort = parseFieldPositive(parts[15], 443);
    this.gatewayUsername = decodeField(parts[16]);
    this.gatewayDomain = decodeField(parts[17]);
    this.useGlobalDisplay = parts.length > 19 ? parts[19] === '1' : true;
    this.scalePercent = parts.length > 20 ? parseFieldNumber(parts[20], 0) : 0;
    this.useGlobalAdvanced = parts.length > 21 ? parts[21] === '1' : true;
    this.useGlobalSr = parts.length > 22 ? parts[22] === '1' : true;
    this.srEnabled = parts.length > 23 ? parts[23] === '1' : false;
    this.srRatioPercent = parts.length > 24 ? parseFieldNumber(parts[24], 0) : 0;
    this.srBackend = parts.length > 25
      ? parseFieldNumber(parts[25], SR_BACKEND_XENGINE) : SR_BACKEND_XENGINE;
    this.resolutionAuto = parts.length > 43 ? parts[43] === '1' : false;
    this.scaleAuto = parts.length > 44 ? parts[44] === '1' : false;
  }

  protected writeProtocolExport(item: ConnectionExport): void {
    item.username = this.username;
    item.domain = this.domain;
    item.width = this.width;
    item.height = this.height;
    item.scalePercent = this.scalePercent;
    item.ignoreCertificate = this.ignoreCertificate;
    item.enableAudio = this.enableAudio;
    item.srEnabled = this.srEnabled;
    item.srRatioPercent = this.srRatioPercent;
    item.srBackend = this.srBackend;
    item.gatewayHost = this.gatewayHost;
    item.gatewayPort = this.gatewayPort;
    item.gatewayUsername = this.gatewayUsername;
    item.gatewayDomain = this.gatewayDomain;
    item.useGlobalDisplay = this.useGlobalDisplay;
    item.useGlobalAdvanced = this.useGlobalAdvanced;
    item.useGlobalSr = this.useGlobalSr;
    item.resolutionAuto = this.resolutionAuto;
    item.scaleAuto = this.scaleAuto;
  }

  protected readProtocolExport(item: ConnectionExport): void {
    this.username = item.username;
    this.domain = item.domain;
    this.width = item.width;
    this.height = item.height;
    this.scalePercent = item.scalePercent;
    this.ignoreCertificate = item.ignoreCertificate;
    this.enableAudio = item.enableAudio;
    this.srEnabled = item.srEnabled;
    this.srRatioPercent = item.srRatioPercent;
    this.srBackend = item.srBackend;
    this.gatewayHost = item.gatewayHost;
    this.gatewayPort = item.gatewayPort > 0 ? item.gatewayPort : 443;
    this.gatewayUsername = item.gatewayUsername;
    this.gatewayDomain = item.gatewayDomain;
    this.useGlobalDisplay = item.useGlobalDisplay;
    this.useGlobalAdvanced = item.useGlobalAdvanced;
    this.useGlobalSr = item.useGlobalSr;
    this.resolutionAuto = item.resolutionAuto;
    this.scaleAuto = item.scaleAuto;
  }

  static fromOptions(id: string, options: RdpConnectOptions): RdpConnection {
    const conn: RdpConnection = new RdpConnection();
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

export class MoonlightConnection extends SavedConnection {
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

  protected defaultPort(): number {
    return ML_DEFAULT_PORT;
  }

  protocolIcon(): Resource {
    return $r('app.media.ic_proto_moonlight');
  }

  subtitle(): string {
    const target: string = `${this.host}:${this.port}`;
    const appName: string = this.mlDefaultAppName.length > 0 ? this.mlDefaultAppName : '桌面';
    return this.name.length > 0 ? `${target} · ${appName}` : appName;
  }

  primaryAction(action: ConnectAction): ConnectAction {
    if (action === ConnectAction.Resume || action === ConnectAction.Switch
      || action === ConnectAction.Disconnect) {
      return ConnectAction.Terminate;
    }
    return action;
  }

  showsAppsButton(): boolean {
    return true;
  }

  hostTargetId(targets: Map<string, number>): number {
    const value: number | undefined = targets.get(this.id);
    return value === undefined ? 0 : value;
  }

  defaultTargetId(): number {
    return this.mlDefaultAppIdValue();
  }

  menuEntries(action: ConnectAction, hostTargetId: number): RowMenuEntry[] {
    const entries: RowMenuEntry[] = [];
    if (action === ConnectAction.Cancel) {
      entries.push(RowMenuEntry.of(RowMenuAction.Toggle, '取消连接'));
    } else if (action === ConnectAction.Connect) {
      entries.push(RowMenuEntry.of(RowMenuAction.Toggle, '连接'));
    } else if (action === ConnectAction.Resume) {
      entries.push(RowMenuEntry.of(RowMenuAction.Toggle, '恢复'));
    } else if (action === ConnectAction.Switch) {
      entries.push(RowMenuEntry.of(RowMenuAction.Toggle, '切换'));
    }
    if (hostTargetId !== 0) {
      entries.push(RowMenuEntry.of(RowMenuAction.Terminate, '终止'));
    }
    entries.push(RowMenuEntry.of(RowMenuAction.Edit, '编辑'));
    entries.push(RowMenuEntry.of(RowMenuAction.Delete, '删除'));
    return entries;
  }

  mlDefaultAppIdValue(): number {
    if (this.mlDefaultAppId.length === 0) {
      return 0;
    }
    const parsed: number = Number.parseInt(this.mlDefaultAppId, 10);
    return Number.isNaN(parsed) ? 0 : parsed;
  }

  protected writeProtocolRecord(slots: string[]): void {
    slots[28] = encodeField(this.mlDefaultAppId);
    slots[29] = encodeField(this.mlDefaultAppName);
    slots[31] = this.mlResMode.toString();
    slots[32] = this.mlWidth.toString();
    slots[33] = this.mlHeight.toString();
    slots[34] = this.mlFps.toString();
    slots[35] = this.mlBitrateKbps.toString();
    slots[37] = this.mlCodec.toString();
    slots[41] = this.useGlobalStream ? '1' : '0';
    slots[42] = encodeField(this.mlHostId);
  }

  protected readProtocolRecord(parts: string[]): void {
    this.mlDefaultAppId = parts.length > 28 ? decodeField(parts[28]) : '';
    this.mlDefaultAppName = parts.length > 29 ? decodeField(parts[29]) : '';
    this.mlResMode = parts.length > 31
      ? parseFieldNumber(parts[31], ML_RES_FOLLOW_SCREEN) : ML_RES_FOLLOW_SCREEN;
    this.mlWidth = parts.length > 32 ? parseFieldNumber(parts[32], 0) : 0;
    this.mlHeight = parts.length > 33 ? parseFieldNumber(parts[33], 0) : 0;
    this.mlFps = parts.length > 34 ? parseFieldPositive(parts[34], 60) : 60;
    this.mlBitrateKbps = parts.length > 35
      ? parseFieldPositive(parts[35], ML_BITRATE_DEFAULT_KBPS) : ML_BITRATE_DEFAULT_KBPS;
    this.mlCodec = parts.length > 37
      ? parseFieldNumber(parts[37], ML_CODEC_AUTO) : ML_CODEC_AUTO;
    this.useGlobalStream = parts.length > 41 ? parts[41] === '1' : true;
    this.mlHostId = parts.length > 42 ? decodeField(parts[42]) : '';
  }

  protected writeProtocolExport(item: ConnectionExport): void {
    item.mlDefaultAppId = this.mlDefaultAppId;
    item.mlDefaultAppName = this.mlDefaultAppName;
    item.mlHostId = this.mlHostId;
    item.mlResMode = this.mlResMode;
    item.mlWidth = this.mlWidth;
    item.mlHeight = this.mlHeight;
    item.mlFps = this.mlFps;
    item.mlBitrateKbps = this.mlBitrateKbps;
    item.mlCodec = this.mlCodec;
    item.useGlobalStream = this.useGlobalStream;
  }

  protected readProtocolExport(item: ConnectionExport): void {
    this.mlDefaultAppId = item.mlDefaultAppId;
    this.mlDefaultAppName = item.mlDefaultAppName;
    this.mlHostId = item.mlHostId;
    this.mlResMode = item.mlResMode;
    this.mlWidth = item.mlWidth;
    this.mlHeight = item.mlHeight;
    this.mlFps = item.mlFps;
    this.mlBitrateKbps = item.mlBitrateKbps;
    this.mlCodec = item.mlCodec;
    this.useGlobalStream = item.useGlobalStream;
  }
}

export interface EditConnectionParams {
  id: string;
  // id 指向已有连接时忽略（已有连接以自己的协议为准）。
  protocol: number;
}
