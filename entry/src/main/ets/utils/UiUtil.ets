/*
 * HmRdp - shared page-level UI helpers.
 *
 * Small, page-agnostic helpers every page would otherwise re-declare: a themed
 * toast and a Length-to-number conversion. They carry no page state.
 */
import { UIContext } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpUi';

/** Toast carrying the overlay material on systems that support one. */
export function showToast(ui: UIContext, message: string, durationMs: number = 2000): void {
  try {
    ui.getPromptAction().showToast({
      message: message,
      duration: durationMs,
      systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY)
    });
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'show toast failed: %{public}s', JSON.stringify(error));
  }
}

/** A Length (number | string | Resource) as a vp number; 0 when not numeric. */
export function lengthToNumber(value: Length): number {
  if (typeof value === 'number') {
    return value;
  }
  if (typeof value === 'string') {
    const parsed: number = Number.parseFloat(value);
    return Number.isNaN(parsed) ? 0 : parsed;
  }
  return 0;
}
