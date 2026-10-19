import { UIContext } from '@kit.ArkUI';
import { DialogRequest, DialogResult, feedbackHost, ModalRequest, ToastTier }
  from './FeedbackHost';

export function showToast(ui: UIContext, message: string,
  tier: ToastTier = ToastTier.Default): void {
  feedbackHost(ui).toast(message, tier);
}

class PromptDialogParams {
  title: string = '';
  message: string = '';
  cancelLabel: string = '';
  confirmLabel: string = '';
  danger: boolean = false;
}

class PromptTask implements ModalRequest {
  private ui: UIContext;
  private params: PromptDialogParams;
  private onCancel: () => void;
  private onConfirm: () => void;
  private done: () => void = (): void => {};
  private onClosed: () => void = (): void => {};
  private settled: boolean = false;
  private finished: boolean = false;

  constructor(ui: UIContext, params: PromptDialogParams,
    onCancel: () => void, onConfirm: () => void) {
    this.ui = ui;
    this.params = params;
    this.onCancel = onCancel;
    this.onConfirm = onConfirm;
  }

  start(done: () => void, closed: () => void): void {
    this.done = done;
    this.onClosed = closed;
    if (this.settled) {
      this.finish();
      return;
    }
    const request: DialogRequest = {
      title: this.params.title,
      message: this.params.message,
      cancelLabel: this.params.cancelLabel,
      confirmLabel: this.params.confirmLabel,
      danger: this.params.danger,
      onResult: (result: DialogResult): void => {
        this.settle(result);
      }
    };
    // 该窗口没有 DialogHost 时直接结算，避免模态队列被卡住。
    if (!feedbackHost(this.ui).presentDialog(request)) {
      this.settle(DialogResult.Dismissed);
    }
  }

  abort(): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    feedbackHost(this.ui).dismissDialog();
  }

  private settle(result: DialogResult): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    if (result === DialogResult.Confirm) {
      this.onConfirm();
    } else if (result === DialogResult.Cancel) {
      this.onCancel();
    }
    this.finish();
  }

  private finish(): void {
    if (this.finished) {
      return;
    }
    this.finished = true;
    this.settled = true;
    this.onClosed();
    this.done();
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
  const params: PromptDialogParams = new PromptDialogParams();
  params.title = options.title;
  params.message = options.message;
  params.cancelLabel = options.cancelLabel !== undefined ? options.cancelLabel : '取消';
  params.confirmLabel = options.confirmLabel;
  params.danger = options.danger === true;
  feedbackHost(ui).enqueue(new PromptTask(ui, params, (): void => {}, options.onConfirm));
}

export function showNotice(ui: UIContext, title: string, message: string,
  confirmLabel: string = '知道了'): void {
  const params: PromptDialogParams = new PromptDialogParams();
  params.title = title;
  params.message = message;
  params.confirmLabel = confirmLabel;
  feedbackHost(ui).enqueue(new PromptTask(ui, params, (): void => {}, (): void => {}));
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
