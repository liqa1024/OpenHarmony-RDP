import { MoonlightSessionConfig } from 'libhmrdp.so';
import { SR_BACKEND_XENGINE } from '../model/Profiles';

export class NvServerInfo {
  hostname: string = '';
  uniqueId: string = '';
  httpsPort: number = 47984;
  externalPort: number = 47989;
  mac: string = '';
  state: string = '';
  gpuType: string = '';
  gfeVersion: string = '';
  paired: boolean = false;
  // First component of appversion; >= 7 selects SHA-256.
  majorVersion: number = 0;
  appVersion: string = '';
  serverCodecModeSupport: number = 0;
  currentGame: number = 0;
}

export class NvApp {
  id: number = 0;
  name: string = '';
}

export class ServerInfoResult {
  ok: boolean = false;
  error: string = '';
  info: NvServerInfo = new NvServerInfo();
}

export class AppListResult {
  ok: boolean = false;
  error: string = '';
  apps: NvApp[] = [];
}

export class PairResult {
  ok: boolean = false;
  error: string = '';
}

export class MoonlightLaunchOptions {
  appId: number = 0;
  width: number = 1920;
  height: number = 1080;
  fps: number = 60;
  bitrateKbps: number = 20000;
  rikeyHex: string = '';
  rikeyId: number = 0;
  // Client audio on: host plays locally off. Drives the /launch localAudioPlayMode.
  audioEnabled: boolean = true;
  // Attached controllers, one bit each (gcmap / remoteControllersBitmap); 0 when none.
  gamepadMask: number = 0;
}

export class LaunchResult {
  ok: boolean = false;
  error: string = '';
  sessionUrl: string = '';
  rikeyHex: string = '';
  rikeyId: number = 0;
}

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
export const ML_FPS_MIN: number = 10;
export const ML_FPS_MAX: number = 240;
export const ML_BITRATE_MIN_KBPS: number = 1000;
export const ML_BITRATE_MAX_KBPS: number = 200000;
export const ML_BITRATE_DEFAULT_KBPS: number = 20000;

// SDR color space, sent as encoderCscMode = (space<<1)|range.
export const ML_COLORSPACE_REC601: number = 0;
export const ML_COLORSPACE_REC709: number = 1;

// Moonlight 缩放模式：写基地版虚拟显示器的 Windows 显示缩放（AUTO 按分辨率换算 / MANUAL 用固定档 / NONE 不动）。
export const ML_SCALE_AUTO: number = 0;
export const ML_SCALE_MANUAL: number = 1;
export const ML_SCALE_NONE: number = 2;
export const ML_COLOR_RANGE_LIMITED: number = 0;
// STREAM_CFG_* for STREAM_CONFIGURATION.streamingRemotely; AUTO uses the RFC1918 heuristic.
export const ML_STREAM_CFG_AUTO: number = 2;

export class MoonlightProfile {
  resMode: number = ML_RES_FOLLOW_SCREEN;
  width: number = 0;
  height: number = 0;
  fps: number = 60;
  bitrateAuto: boolean = true;
  bitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  codec: number = ML_CODEC_AUTO;
  colorSpace: number = ML_COLORSPACE_REC709;
  scaleMode: number = ML_SCALE_AUTO;
  scalePercent: number = 100;
}

export class MoonlightStreamConfig implements MoonlightSessionConfig {
  host: string = '';
  serverInfoAppVersion: string = '';
  serverInfoGfeVersion: string = '';
  sessionUrl: string = '';
  serverCodecModeSupport: number = 0;
  width: number = 1920;
  height: number = 1080;
  // 超分: the session (pre-upscale) size the host encodes; 0 = the same as width/height. `srBackend`
  // and `srRatioPercent` describe the setting — the backend drives the presenter, the ratio only
  // labels the session ("FSR 1.75x").
  streamWidth: number = 0;
  streamHeight: number = 0;
  srBackend: number = SR_BACKEND_XENGINE;
  srRatioPercent: number = 0;
  fps: number = 60;
  bitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  packetSize: number = 1024;
  streamingRemotely: number = ML_STREAM_CFG_AUTO;
  audioConfiguration: number = 0;
  supportedVideoFormats: number = 0;
  clientRefreshRateX100: number = 0;
  colorSpace: number = 0;
  colorRange: number = 0;
  encryptionFlags: number = 0;
  remoteInputAesKeyHex: string = '';
  remoteInputAesIvHex: string = '';
  localCursorMode: boolean = false;
  enableAudio: boolean = true;
  lowLatencyAudio: boolean = true;
}



