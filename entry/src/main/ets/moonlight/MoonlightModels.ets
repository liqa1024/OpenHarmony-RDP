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
  // Ask the host for HDR: drives the /launch hdrMode parameter and the 10-bit codec choice.
  hdr: boolean = false;
  // HDR base transfer when hdr is true: ML_HDR_MODE_PQ (HDR10) or ML_HDR_MODE_HLG.
  hdrMode: number = ML_HDR_MODE_PQ;
  // HDR 亮度上报（仅 hdr 为真时随 /launch 发出，三个一组缺一不可）。peak 为 0 表示不上报，
  // 主机用自己的默认值。
  hdrPeakNits: number = 0;
  hdrMinNits: number = ML_HDR_MIN_NITS;
  hdrFullFrameNits: number = 0;
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

// HDR 亮度上报：/launch 的 maxBrightness / minBrightness / maxAverageBrightness（基地版扩展）。主机把它们
// 写进虚拟显示器的 HDR 亮度范围，Windows 据此把 SDR 桌面映射进 HDR——不上报就用主机的 1000 nit 默认值，
// SDR 内容会偏亮。平台只给得出峰值，另两个的口径与取法见 utils/MoonlightHdr.ets。
export const ML_HDR_NITS_MIN: number = 1;
export const ML_HDR_NITS_MAX: number = 10000;
// 黑位：平台测不到面板黑位，固定用主机自己的默认值（极小，等于不设黑位下限）。
export const ML_HDR_MIN_NITS: number = 0.001;
export const ML_HDR_PEAK_DEFAULT_NITS: number = 1000;

// HDR **内容**的显示亮度（0–100%，落到 ArkUI 组件级的 `XComponent.hdrBrightness`）：0% = 按 SDR 亮度显示、
// 100% = 按当前允许的最高 HDR 亮度显示，而 100% 就是平台默认值，故默认取 100（等于不动默认观感，需要时才
// 下调）。它作用在本机显示的 HDR 内容上，与上面那组上报给主机的亮度（`ML_HDR_*_NITS`）无关。
export const ML_HDR_WHITE_POINT_DEFAULT_PERCENT: number = 100;

// The colour protocol the session asks the host to encode with, sent as
// encoderCscMode = (space<<1)|range and mirrored by the values of COLORSPACE_* in Limelight.h.
// HDR is folded into the REC2020 entry (HDR always uses Rec.2020), so this is one choice of
// protocol, not a statement about what this device can display: REC2020 also selects a 10-bit codec
// and hdrMode, and whether the picture can actually be shown as HDR is a separate, runtime fact.
export const ML_COLORSPACE_REC601: number = 0;
export const ML_COLORSPACE_REC709: number = 1;
export const ML_COLORSPACE_REC2020_HDR: number = 2;

// True for the colour protocol that carries HDR (10-bit codec + HDR requested).
export function mlColorSpaceIsHdr(space: number): boolean {
  return space === ML_COLORSPACE_REC2020_HDR;
}

// HDR base transfer when the colour protocol is Rec2020 (the /launch hdrMode value): 1 = HDR10 (PQ),
// 2 = HLG (ARIB STD-B67). Mirrors HDR_MODE_* / the stream's dynamicRangeMode on the wire.
export const ML_HDR_MODE_PQ: number = 1;
export const ML_HDR_MODE_HLG: number = 2;

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
  // HDR base transfer used when colorSpace is Rec2020 (see ML_HDR_MODE_*).
  hdrMode: number = ML_HDR_MODE_PQ;
  scaleMode: number = ML_SCALE_AUTO;
  scalePercent: number = 100;
  // HDR 亮度上报（HDR 会话才有意义）。峰值自动 = 读本机屏幕；手动 = hdrPeakNits。
  // 全屏平均自动 = 跟随峰值；手动 = hdrFullFrameNits（上报时按峰值封顶）。
  hdrPeakAuto: boolean = true;
  hdrPeakNits: number = ML_HDR_PEAK_DEFAULT_NITS;
  hdrFullFrameAuto: boolean = true;
  hdrFullFrameNits: number = ML_HDR_PEAK_DEFAULT_NITS;
  // HDR 内容的显示亮度（见 ML_HDR_WHITE_POINT_DEFAULT_PERCENT）；非 HDR 会话不参与。
  hdrWhitePointPercent: number = ML_HDR_WHITE_POINT_DEFAULT_PERCENT;
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
  hdrMode: number = 0;
  // HDR 内容的显示亮度（0–100，见 ML_HDR_WHITE_POINT_DEFAULT_PERCENT）：会话页把它交给画面组件的
  // hdrBrightness（ArkUI 侧生效），不下到原生；只有会话真的在放 HDR（主机已确认）时才交给组件，非 HDR 用平台默认。
  hdrWhitePointPercent: number = ML_HDR_WHITE_POINT_DEFAULT_PERCENT;
  encryptionFlags: number = 0;
  remoteInputAesKeyHex: string = '';
  remoteInputAesIvHex: string = '';
  localCursorMode: boolean = false;
  enableAudio: boolean = true;
  lowLatencyAudio: boolean = true;
}



