import { display } from '@kit.ArkUI';
import { ML_HDR_MIN_NITS, ML_HDR_NITS_MAX, ML_HDR_NITS_MIN,
  MoonlightProfile } from '../moonlight/MoonlightModels';

// 本机面板的 HDR 峰值亮度（nits），0 表示检测不到。给出的是**吸附到标准档之后**的值，也就是真正会上报
// 的那个数（连接时是多次采样取中位再吸附，这里是单次读数吸附，见 snapPeakToTier）。
//
// 平台只给得出 `sdrNits`（当前 SDR 白点绝对亮度）与 `maxHeadroom`（相对它的最大余量倍率，不是亮度读数），
// 相乘才是面板最大亮度（小窗口激活峰值）。它是**当前**屏幕亮度下的值（随亮度条走），所以设置页订阅
// `brightnessInfoChange` 让显示的检测值跟着变；真正上报的是连接那一刻重新采样的值。
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
  return snapPeakToTier(Math.round(peak));
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

// ---------------------------------------------------------------------------
// 面板亮度测量（连接时上报用）
//
// 上报的三项（`/launch` 的 maxBrightness / minBrightness / maxAverageBrightness）描述这块屏的能力：
// 峰值 = `sdrNits × maxHeadroom`；满帧可持续亮度是面板 ABL 下的全屏亮度，与峰值**不是同一个量**，所以它
// **不能"跟随峰值"**——把峰值当满帧亮度上报等于告诉主机"这块屏没有 ABL 限制"，中高亮度内容会整体偏亮，
// 故按固定比例取（`HDR_FULL_FRAME_RATIO`）；黑位平台不给，用主机默认（见 MoonlightModels 的 `ML_HDR_MIN_NITS`）。
//
// 峰值要采样：`sdrNits` 随亮度条走、单次读数会抖，而主机对已建虚拟显示器的容差是 ±max(25 nits, 5%)，
// 超差就重建虚拟显示器、Windows 重读 EDID 并重算 SDR 白位（画面亮度会跳）。所以多次采样取中位、并吸附到
// 标准档；这是照 moonlight-harmony 的做法——moonlight-vplus（口径标准）的峰值直接读 EDID，天生稳定，
// 我们平台没有那个读数，只能靠采样补上。

const HDR_SAMPLE_COUNT = 5;
const HDR_SAMPLE_INTERVAL_MS = 250;
const HDR_MIN_SAMPLES = 3;
// 低于这些读数视为系统未就绪或瞬态干扰（`sdrNits` 的默认值是 500、headroom 的默认值是 1.0）。
const HDR_MIN_RELIABLE_SDR_NITS = 50;
const HDR_MIN_RELIABLE_PEAK_NITS = 100;
// 样本间 sdrNits 的最大波动比：超过说明自动亮度正在调整，这一轮不报。
const HDR_MAX_SDR_VARIANCE = 1.5;
// 峰值吸附的标准档（见 snapPeakToTier）。
const HDR_PEAK_TIERS: number[] = [400, 600, 800, 1000, 1200, 1600, 2000, 2600, 4000];
const HDR_TIER_TOLERANCE = 0.3;
// 全屏可持续亮度取峰值的这个比例：moonlight-vplus（安卓版，口径标准）在面板不报 EDID 的
// `desiredMaxAverageLuminance` 时就是这么取的（`max(黑位, peak × 0.2)`），我们没有那个读数，只有这一条路。
const HDR_FULL_FRAME_RATIO = 0.2;

// 自动档位的满帧亮度（= 峰值的固定比例，钳在 [黑位, 峰值] 内）。页面也用它显示预览。
export function autoHdrFullFrameNits(peak: number): number {
  return clampHdrFullFrameNits(Math.round(peak * HDR_FULL_FRAME_RATIO), peak);
}

export class BrightnessSample {
  sdrNits: number = 0;
  maxHeadroom: number = 0;
}

// 一次测量得出的面板口径：sdrNits 是样本中位数，peak 是要上报的峰值。
export class HdrPanelLuminance {
  sdrNits: number = 0;
  peak: number = 0;
}

function median(values: number[]): number {
  const sorted: number[] = values.slice().sort((a: number, b: number) => a - b);
  const mid: number = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 1 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

function maxOf(values: number[]): number {
  let result: number = values[0];
  for (const value of values) {
    if (value > result) {
      result = value;
    }
  }
  return result;
}

function minOf(values: number[]): number {
  let result: number = values[0];
  for (const value of values) {
    if (value < result) {
      result = value;
    }
  }
  return result;
}

function snapToTier(value: number, tiers: number[]): number {
  let best: number = tiers[0];
  for (const tier of tiers) {
    if (Math.abs(tier - value) < Math.abs(best - value)) {
      best = tier;
    }
  }
  return best;
}

// 吸附到标准档；偏离超过 `HDR_TIER_TOLERANCE` 就保留原值（低峰值不该被拉高太多）。上报与设置页显示共用
// 这一条，否则页面显示的是原始读数、上报的是档位值，两边对不上。
function snapPeakToTier(value: number): number {
  const snapped: number = snapToTier(value, HDR_PEAK_TIERS);
  return Math.abs(snapped - value) <= value * HDR_TIER_TOLERANCE ? snapped : value;
}

// 聚合采样并校验可靠性；不可靠返回 undefined，调用方按"不上报"处理（见 resolveHdrLuminance）。
export function aggregateBrightnessSamples(samples: BrightnessSample[]): HdrPanelLuminance | undefined {
  const valid: BrightnessSample[] = samples.filter((sample: BrightnessSample): boolean =>
    Number.isFinite(sample.sdrNits) && Number.isFinite(sample.maxHeadroom) &&
    sample.sdrNits >= HDR_MIN_RELIABLE_SDR_NITS && sample.maxHeadroom > 0);
  if (valid.length < HDR_MIN_SAMPLES) {
    return undefined;
  }

  const sdrValues: number[] = valid.map((sample: BrightnessSample): number => sample.sdrNits);
  const minSdr: number = minOf(sdrValues);
  const maxSdr: number = maxOf(sdrValues);
  if (minSdr <= 0 || maxSdr / minSdr > HDR_MAX_SDR_VARIANCE) {
    return undefined;
  }

  const sdrNits: number = median(sdrValues);
  const peak: number = sdrNits * maxOf(valid.map((sample: BrightnessSample): number => sample.maxHeadroom));
  if (!(peak >= HDR_MIN_RELIABLE_PEAK_NITS) || peak < sdrNits) {
    return undefined;
  }

  const clampedPeak: number = Math.round(Math.min(Math.max(peak, HDR_MIN_RELIABLE_PEAK_NITS),
    ML_HDR_NITS_MAX));
  const measured: HdrPanelLuminance = new HdrPanelLuminance();
  measured.sdrNits = Math.round(sdrNits);
  measured.peak = snapPeakToTier(clampedPeak);
  return measured;
}

function sleep(ms: number): Promise<void> {
  return new Promise<void>((resolve: () => void): void => {
    setTimeout(resolve, ms);
  });
}

// 连接时采样：count 次读取、间隔 intervalMs（默认 5×250ms）。单次失败跳过，聚合不可靠时回 undefined。
export async function sampleDisplayHdrLuminance(count: number = HDR_SAMPLE_COUNT,
  intervalMs: number = HDR_SAMPLE_INTERVAL_MS): Promise<HdrPanelLuminance | undefined> {
  let displayId: number = -1;
  try {
    const screen: display.Display | undefined = display.getDefaultDisplaySync();
    displayId = screen === undefined ? -1 : screen.id;
  } catch (error) {
    return undefined;
  }
  if (displayId < 0) {
    return undefined;
  }
  const samples: BrightnessSample[] = [];
  for (let i = 0; i < count; i++) {
    if (i > 0) {
      await sleep(intervalMs);
    }
    try {
      const info: display.BrightnessInfo = display.getBrightnessInfo(displayId);
      const sample: BrightnessSample = new BrightnessSample();
      sample.sdrNits = info.sdrNits;
      sample.maxHeadroom = info.maxHeadroom;
      samples.push(sample);
    } catch (error) {
    }
  }
  if (samples.length === 0) {
    return undefined;
  }
  return aggregateBrightnessSamples(samples);
}

// ---------------------------------------------------------------------------
// 上报值解析

export class HdrLuminance {
  peak: number = 0;
  min: number = ML_HDR_MIN_NITS;
  fullFrame: number = 0;
}

// 把画质设置解析成要上报的三个亮度值（口径见本节开头）。峰值解析不出来（自动且本机测量不可靠）时返回
// undefined：主机把三个值当一份整体校验，发一份不完整的反而不如不发。只有峰值自动时才需要采样。
export async function resolveHdrLuminance(profile: MoonlightProfile): Promise<HdrLuminance | undefined> {
  let panel: HdrPanelLuminance | undefined = undefined;
  if (profile.hdrPeakAuto) {
    panel = await sampleDisplayHdrLuminance();
  }
  const peak: number = profile.hdrPeakAuto
    ? (panel === undefined ? 0 : panel.peak)
    : clampHdrPeakNits(profile.hdrPeakNits);
  if (peak < ML_HDR_NITS_MIN) {
    return undefined;
  }
  const luminance: HdrLuminance = new HdrLuminance();
  luminance.peak = peak;
  luminance.min = ML_HDR_MIN_NITS;
  luminance.fullFrame = profile.hdrFullFrameAuto
    ? autoHdrFullFrameNits(peak)
    : clampHdrFullFrameNits(profile.hdrFullFrameNits, peak);
  return luminance;
}
