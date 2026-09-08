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

export enum RdpEvent {
  Connected = 0,
  Disconnected = 1,
  Error = 2,
  FirstFrame = 3,
  ClipboardText = 4,
  Resize = 5,
  Certificate = 6
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
  enableClipboard: boolean = true;
  enableAudio: boolean = true;
  enableGfx: boolean = true;
  enableH264: boolean = true;
  enableRemoteFx: boolean = true;
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
  static current: PendingConnection = new PendingConnection();
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
}

export class SavedConnection extends RdpConnectOptions {
  id: string = '';
  updatedAt: number = 0;
  // When true the connection follows the global display settings and ignores
  // width / height / scalePercent; when false the stored values are used.
  useGlobalDisplay: boolean = true;

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
    conn.enableClipboard = options.enableClipboard;
    conn.enableAudio = options.enableAudio;
    conn.enableGfx = options.enableGfx;
    conn.enableH264 = options.enableH264;
    conn.enableRemoteFx = options.enableRemoteFx;
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
    options.enableClipboard = this.enableClipboard;
    options.enableAudio = this.enableAudio;
    options.enableGfx = this.enableGfx;
    options.enableH264 = this.enableH264;
    options.enableRemoteFx = this.enableRemoteFx;
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

