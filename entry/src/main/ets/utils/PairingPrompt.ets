import { ComponentContent, UIContext } from '@kit.ArkUI';
import { PairResult } from '../moonlight/MoonlightModels';
import { PromptDialog } from '../components/PromptDialog';
import { DialogSession } from './DialogSession';
import { feedbackHost, ModalRequest } from './FeedbackHost';

const PAIR_PROMPT_GRACE_MS: number = 300;

class PairingDialogParams {
  digits: string[] = [];
  onCancel: () => void = (): void => {};
}

@Builder
function pairingDialogContent(params: PairingDialogParams) {
  PromptDialog({
    title: '与主机配对',
    message: '请在主机（Sunshine）上输入以下 PIN',
    cancelLabel: '取消',
    onCancel: () => {
      params.onCancel();
    }
  }) {
    Column() {
      Row({ space: 12 }) {
        ForEach(params.digits, (digit: string, index: number) => {
          Text(digit)
            .fontSize(20)
            .fontWeight(600)
            .fontColor($r('app.color.text_primary'))
            .textAlign(TextAlign.Center)
            .width(56)
            .height(64)
            .backgroundColor($r('app.color.control_bg'))
            .border({ width: 1, color: $r('app.color.control_border') })
            .borderRadius(12)
        }, (digit: string, index: number) => `${index}-${digit}`)
      }

      Row({ space: 6 }) {
        LoadingProgress()
          .width(16)
          .height(16)
          .color($r('app.color.brand_fill'))
        Text('输入后自动完成配对')
          .fontSize(12)
          .fontColor($r('app.color.text_tertiary'))
      }
      .justifyContent(FlexAlign.Center)
      .margin({ top: 16 })
    }
    .width('100%')
    .alignItems(HorizontalAlign.Center)
    .margin({ top: 20 })
  }
}

class PairingRequest implements ModalRequest {
  private ui: UIContext;
  private params: PairingDialogParams = new PairingDialogParams();
  private session: DialogSession<PairingDialogParams>;
  private resolve: (value: boolean) => void;
  private cancel: () => void;
  private done: () => void = (): void => {};
  private onClosed: () => void = (): void => {};
  private timer: number = -1;
  private started: boolean = false;
  private settled: boolean = false;
  private finished: boolean = false;

  constructor(ui: UIContext, pin: string, resolve: (value: boolean) => void,
    cancel: () => void) {
    this.ui = ui;
    this.resolve = resolve;
    this.cancel = cancel;
    this.session = new DialogSession<PairingDialogParams>(ui);
    this.params.digits = [pin.substring(0, 1), pin.substring(1, 2),
      pin.substring(2, 3), pin.substring(3, 4)];
  }

  start(done: () => void, closed: () => void): void {
    this.done = done;
    this.onClosed = closed;
    this.started = true;
    if (this.settled) {
      this.finishHost();
      return;
    }
    this.timer = setTimeout((): void => {
      this.timer = -1;
      if (this.settled) {
        return;
      }
      this.params.onCancel = (): void => {
        this.cancelFromUser();
      };
      const content: ComponentContent<PairingDialogParams> =
        new ComponentContent(this.ui, wrapBuilder(pairingDialogContent), this.params);
      this.session.open(content, (): void => {
        this.cancelFromUser();
      });
    }, PAIR_PROMPT_GRACE_MS);
  }

  complete(): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.clearTimer();
    this.resolve(true);
    if (this.started) {
      this.session.close((): void => {
        this.onClosed();
        this.finishHost();
      });
    }
  }

  abort(): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.clearTimer();
    this.cancel();
    this.session.dispose();
    this.resolve(false);
  }

  private cancelFromUser(): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.clearTimer();
    this.cancel();
    this.session.close((): void => {
      this.onClosed();
      this.resolve(false);
      this.finishHost();
    });
  }

  private finishHost(): void {
    if (this.finished) {
      return;
    }
    this.finished = true;
    this.done();
  }

  private clearTimer(): void {
    if (this.timer !== -1) {
      clearTimeout(this.timer);
      this.timer = -1;
    }
  }
}

export function promptPairingPin(ui: UIContext, pin: string,
  pairing: Promise<PairResult>, cancel: () => void): Promise<boolean> {
  return new Promise<boolean>((resolve: (value: boolean) => void) => {
    const request: PairingRequest = new PairingRequest(ui, pin, resolve, cancel);
    pairing.then((): void => {
      request.complete();
    }, (): void => {
      request.complete();
    });
    feedbackHost(ui).enqueue(request);
  });
}
