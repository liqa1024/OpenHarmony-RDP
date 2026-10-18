import { ComponentContent, UIContext } from '@kit.ArkUI';
import { PromptDialog } from '../components/PromptDialog';
import { DialogSession } from './DialogSession';
import { feedbackHost, ModalRequest, ToastTier } from './FeedbackHost';

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

class PromptTask implements ModalRequest {
  private ui: UIContext;
  private params: PromptDialogParams;
  private session: DialogSession<PromptDialogParams>;
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
    this.session = new DialogSession<PromptDialogParams>(ui);
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
    // Wire callbacks before creating the content node: the builder may capture
    // the parameter object at construction time.
    this.params.onCancel = (): void => {
      this.dismiss(this.onCancel);
    };
    this.params.onConfirm = (): void => {
      this.dismiss(this.onConfirm);
    };
    const content: ComponentContent<PromptDialogParams> =
      new ComponentContent(this.ui, wrapBuilder(promptDialogContent), this.params);
    this.session.open(content, (): void => {
      this.finish();
    });
  }

  abort(): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.session.dispose();
  }

  private dismiss(after: () => void): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.session.close((): void => {
      this.onClosed();
      after();
      this.finish();
    });
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
