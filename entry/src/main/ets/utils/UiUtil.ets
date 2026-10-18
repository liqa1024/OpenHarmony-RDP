import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PromptDialog } from '../components/PromptDialog';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpUi';

export function showToast(ui: UIContext, message: string, durationMs: number = 2000): void {
  try {
    ui.getPromptAction().showToast({
      message: message,
      duration: durationMs,
      backgroundColor: $r('app.color.chrome_bg'),
      textColor: $r('app.color.chrome_text_primary'),
      backgroundBlurStyle: BlurStyle.NONE,
      shadow: {
        radius: 16,
        color: $r('app.color.shadow_2'),
        offsetX: 0,
        offsetY: 4
      }
    });
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'show toast failed: %{public}s', JSON.stringify(error));
  }
}

class PromptDialogParams {
  title: string = '';
  message: string = '';
  cancelLabel: string = '';
  confirmLabel: string = '';
  danger: boolean = false;
  onCancel: () => void = () => {};
  onConfirm: () => void = () => {};
}

@Builder
function promptDialogContent(params: PromptDialogParams) {
  PromptDialog({
    title: params.title,
    message: params.message,
    cancelLabel: params.cancelLabel,
    confirmLabel: params.confirmLabel,
    danger: params.danger,
    onCancel: params.onCancel,
    onConfirm: params.onConfirm
  })
}

function openPrompt(ui: UIContext, params: PromptDialogParams,
  onCancel: () => void, onConfirm: () => void): void {
  let closed: boolean = false;
  let contentNode: ComponentContent<PromptDialogParams> | undefined = undefined;
  const disposeContent = (): void => {
    const node: ComponentContent<PromptDialogParams> | undefined = contentNode;
    if (node === undefined) {
      return;
    }
    try {
      node.dispose();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'dispose prompt failed: %{public}s', JSON.stringify(error));
    }
  };
  const dismiss = (after: () => void): void => {
    if (closed) {
      return;
    }
    closed = true;
    const node: ComponentContent<PromptDialogParams> | undefined = contentNode;
    if (node === undefined) {
      after();
      return;
    }
    ui.getPromptAction().closeCustomDialog(node)
      .then(() => {
        disposeContent();
        after();
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'close prompt failed: %{public}s', JSON.stringify(error));
        disposeContent();
        after();
      });
  };
  // Wire callbacks before creating the content node: the builder may capture
  // the parameter object at construction time.
  params.onCancel = (): void => {
    dismiss(onCancel);
  };
  params.onConfirm = (): void => {
    dismiss(onConfirm);
  };
  contentNode = new ComponentContent(ui, wrapBuilder(promptDialogContent), params);
  ui.getPromptAction().openCustomDialog(contentNode,
    { isModal: true, autoCancel: false })
    .catch((error: BusinessError) => {
      hilog.warn(DOMAIN, TAG, 'open prompt failed: %{public}s', JSON.stringify(error));
      if (!closed) {
        closed = true;
        disposeContent();
      }
    });
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
  const params: PromptDialogParams = new PromptDialogParams();
  params.title = options.title;
  params.message = options.message;
  params.cancelLabel = options.cancelLabel !== undefined ? options.cancelLabel : '取消';
  params.confirmLabel = options.confirmLabel;
  params.danger = options.danger === true;
  openPrompt(ui, params, (): void => {}, options.onConfirm);
}

export function showNotice(ui: UIContext, title: string, message: string,
  confirmLabel: string = '知道了'): void {
  const params: PromptDialogParams = new PromptDialogParams();
  params.title = title;
  params.message = message;
  params.confirmLabel = confirmLabel;
  openPrompt(ui, params, (): void => {}, (): void => {});
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
