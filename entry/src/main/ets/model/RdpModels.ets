import { RdpOptions } from 'libhmrdp.so';
import { SR_BACKEND_XENGINE } from './Profiles';

export const RDP_DEFAULT_PORT: number = 3389;

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
  RemoteLockState = 22,
  // Remote clipboard image received as PNG (registered "PNG" format).
  ClipboardImagePng = 23
}

export class RdpConnectOptions implements RdpOptions {
  host: string = '';
  port: number = RDP_DEFAULT_PORT;
  name: string = '';
  username: string = '';
  password: string = '';
  domain: string = '';
  width: number = 1920;
  height: number = 1080;
  // 超分: the desktop size the server renders (pre-upscale), 0 = width/height; the launcher resolves
  // it from the display profile. `srBackend` drives the presenter; `srRatioPercent` labels the
  // session and drives nothing.
  streamWidth: number = 0;
  streamHeight: number = 0;
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
    const target: string = this.port === RDP_DEFAULT_PORT ? this.host : `${this.host}:${this.port}`;
    return this.name.length > 0 ? `${this.name} · ${target}` : target;
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
  // Must match Session::LocalClipKind::kImagePng.
  static readonly PNG: number = 6;
}

export class RdpTouchFlags {
  static readonly DOWN: number = 0x01;
  static readonly UP: number = 0x02;
  static readonly MOTION: number = 0x04;
  // FreeRDP keeps pressure only when this bit is set (FREERDP_TOUCH_HAS_PRESSURE).
  static readonly HAS_PRESSURE: number = 0x100;
}
