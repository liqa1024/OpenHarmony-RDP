/*
 * HmRdp - shared page-level UI helpers.
 *
 * Small, page-agnostic utilities that every page would otherwise re-declare:
 * a fault-tolerant default-display query and a themed toast. They carry no
 * page state, so they live as plain exported functions.
 */
import { display, UIContext } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpUi';

/** The default display, or undefined when the query fails (never throws). */
export function defaultDisplay(): display.Display | undefined {
  try {
    return display.getDefaultDisplaySync();
  } catch (error) {
    return undefined;
  }
}

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
