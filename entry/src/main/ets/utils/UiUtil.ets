/*
 * HmRdp - shared page-level UI helpers.
 *
 * Small, page-agnostic helpers every page would otherwise re-declare: a themed
 * toast, the two alert-dialog shapes (confirm / notice) and a Length-to-number
 * conversion. They carry no page state.
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

/** A two-button confirmation: a no-op cancel on the left, the action on the right. */
export interface ConfirmDialogOptions {
  title: string;
  message: string;
  confirmLabel: string;
  /** Left button label; defaults to "取消". Its action is always a no-op. */
  cancelLabel?: string;
  /** Renders the confirm button in the danger colour (default: accent). */
  danger?: boolean;
  onConfirm: () => void;
}

/** Confirmation dialog with the overlay material (see ConfirmDialogOptions). */
export function showConfirm(ui: UIContext, options: ConfirmDialogOptions): void {
  try {
    ui.showAlertDialog({
      title: options.title,
      message: options.message,
      systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY),
      primaryButton: {
        value: options.cancelLabel !== undefined ? options.cancelLabel : '取消',
        action: () => {
        }
      },
      secondaryButton: {
        value: options.confirmLabel,
        fontColor: options.danger === true ? $r('app.color.danger') : $r('app.color.accent'),
        action: () => {
          options.onConfirm();
        }
      }
    });
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'show confirm failed: %{public}s', JSON.stringify(error));
  }
}

/** Single-button notice ("知道了") with the overlay material. */
export function showNotice(ui: UIContext, title: string, message: string,
  confirmLabel: string = '知道了'): void {
  try {
    ui.showAlertDialog({
      title: title,
      message: message,
      systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY),
      confirm: {
        value: confirmLabel,
        action: () => {
        }
      }
    });
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'show notice failed: %{public}s', JSON.stringify(error));
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
