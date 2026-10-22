import { display } from '@kit.ArkUI';
import { ML_HDR_MIN_NITS, ML_HDR_NITS_MAX, ML_HDR_NITS_MIN,
  MoonlightProfile } from '../moonlight/MoonlightModels';

// 本机面板的 HDR 峰值亮度（nits），0 表示检测不到。
//
// 平台只给得出这一个值，而且是推导出来的：`sdrNits` 是 SDR 白点的绝对亮度，`maxHeadroom` 是相对它的
// 最大余量倍率（不是亮度读数），相乘才是面板的最大亮度（小窗口激活亮度）。它的两个同伴——全屏可持续
// 亮度（ABL）与黑位——平台没有任何对应查询，只能由用户给。
//
// 不支持 HDR 的屏 maxHeadroom 恒为 1.0、虚拟屏 sdrNits 是默认值，所以这里的窗口/虚拟屏取值没有意义；
// 调用方只在 HDR 会话里用它。
export function displayHdrPeakNits(): number {
  try {
    const screen: display.Display | undefined = display.getDefaultDisplaySync();
    if (screen === undefined) {
      return 0;
    }
    return peakFromBrightness(display.getBrightnessInfo(screen.id));
  } catch (error) {
    return 0;
  }
}

function peakFromBrightness(info: display.BrightnessInfo): number {
  const peak: number = info.sdrNits * info.maxHeadroom;
  if (!Number.isFinite(peak) || peak < ML_HDR_NITS_MIN || peak > ML_HDR_NITS_MAX) {
    return 0;
  }
  return Math.round(peak);
}

// 跟随默认屏幕的亮度变化（检测值随亮度条走，设置界面显示的数值要跟着刷新）；
// 返回取消订阅的函数，页面在 aboutToDisappear 里调用它。
export function observeHdrPeak(onChange: (peak: number) => void): () => void {
  let displayId: number = -1;
  try {
    const screen: display.Display | undefined = display.getDefaultDisplaySync();
    displayId = screen === undefined ? -1 : screen.id;
  } catch (error) {
    return (): void => {};
  }
  const handler: (changedId: number, info: display.BrightnessInfo) => void =
    (changedId: number, info: display.BrightnessInfo): void => {
      if (changedId === displayId) {
        onChange(peakFromBrightness(info));
      }
    };
  try {
    display.on('brightnessInfoChange', handler);
  } catch (error) {
    // 平台不支持该事件（非 HDR 屏 / 低版本）：设置界面仍显示进入页面时的那一次检测值。
    return (): void => {};
  }
  return (): void => {
    try {
      display.off('brightnessInfoChange', handler);
    } catch (error) {
    }
  };
}

export function clampHdrPeakNits(nits: number): number {
  if (!Number.isFinite(nits) || nits < ML_HDR_NITS_MIN) {
    return ML_HDR_NITS_MIN;
  }
  return nits > ML_HDR_NITS_MAX ? ML_HDR_NITS_MAX : nits;
}

// 全屏平均亮度必须落在 [1, 峰值] 内：主机把三个值当作一次上报整体校验，越界会让整份作废。
export function clampHdrFullFrameNits(nits: number, peak: number): number {
  const upper: number = clampHdrPeakNits(peak);
  if (!Number.isFinite(nits) || nits < ML_HDR_NITS_MIN) {
    return upper;
  }
  return nits > upper ? upper : nits;
}

export class HdrLuminance {
  peak: number = 0;
  min: number = ML_HDR_MIN_NITS;
  fullFrame: number = 0;
}

// 把画质设置解析成要上报的三个亮度值。峰值解析不出来（自动且本机检测不到）时返回 undefined，
// 表示这一项不上报、交给主机自己的默认值——发一份不完整的上报会被主机整份拒绝，反而不如不发。
export function resolveHdrLuminance(profile: MoonlightProfile): HdrLuminance | undefined {
  const peak: number = profile.hdrPeakAuto
    ? displayHdrPeakNits()
    : clampHdrPeakNits(profile.hdrPeakNits);
  if (peak < ML_HDR_NITS_MIN) {
    return undefined;
  }
  const luminance: HdrLuminance = new HdrLuminance();
  luminance.peak = peak;
  luminance.min = ML_HDR_MIN_NITS;
  luminance.fullFrame = profile.hdrFullFrameAuto
    ? peak
    : clampHdrFullFrameNits(profile.hdrFullFrameNits, peak);
  return luminance;
}
