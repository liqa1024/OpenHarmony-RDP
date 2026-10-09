import { UIContext } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpUi';

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

export interface ConfirmDialogOptions {
  title: string;
  message: string;
  confirmLabel: string;
  cancelLabel?: string;
  danger?: boolean;
  onConfirm: () => void;
}

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
