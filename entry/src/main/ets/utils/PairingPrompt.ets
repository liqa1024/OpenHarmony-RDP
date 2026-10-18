import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PairResult } from '../moonlight/MoonlightModels';
import { PromptDialog } from '../components/PromptDialog';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpPairing';
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

export async function promptPairingPin(ui: UIContext, pin: string,
  pairing: Promise<PairResult>, cancel: () => void): Promise<boolean> {
  return new Promise<boolean>((resolve: (value: boolean) => void) => {
    let settled: boolean = false;
    let closed: boolean = false;
    let opened: boolean = false;
    let graceTimer: number = -1;
    const params: PairingDialogParams = new PairingDialogParams();
    params.digits = [pin.substring(0, 1), pin.substring(1, 2),
      pin.substring(2, 3), pin.substring(3, 4)];
    let contentNode: ComponentContent<PairingDialogParams> | undefined = undefined;

    const disposeContent = (): void => {
      const node: ComponentContent<PairingDialogParams> | undefined = contentNode;
      if (node === undefined) {
        return;
      }
      try {
        node.dispose();
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'dispose pairing content failed: %{public}s', JSON.stringify(error));
      }
    };
    const dismiss = (): void => {
      if (closed) {
        return;
      }
      closed = true;
      const node: ComponentContent<PairingDialogParams> | undefined = contentNode;
      if (node === undefined) {
        return;
      }
      ui.getPromptAction().closeCustomDialog(node)
        .then(() => {
          disposeContent();
        })
        .catch((error: BusinessError) => {
          hilog.error(DOMAIN, TAG, 'close pairing dialog failed: %{public}s', JSON.stringify(error));
          disposeContent();
        });
    };
    const finish = (proceed: boolean): void => {
      if (settled) {
        return;
      }
      settled = true;
      if (graceTimer >= 0) {
        clearTimeout(graceTimer);
        graceTimer = -1;
      }
      if (opened) {
        dismiss();
      } else {
        disposeContent();
      }
      resolve(proceed);
    };
    // Wire the callback before creating the content node: the builder may
    // capture the parameter object at construction time.
    params.onCancel = (): void => {
      cancel();
      finish(false);
    };
    contentNode = new ComponentContent(ui, wrapBuilder(pairingDialogContent), params);

    graceTimer = setTimeout((): void => {
      graceTimer = -1;
      if (settled) {
        return;
      }
      const node: ComponentContent<PairingDialogParams> | undefined = contentNode;
      if (node === undefined) {
        finish(false);
        return;
      }
      opened = true;
      ui.getPromptAction().openCustomDialog(node,
        { isModal: true, autoCancel: false })
        .then(() => {
          if (settled) {
            dismiss();
          }
        })
        .catch((error: BusinessError) => {
          hilog.error(DOMAIN, TAG, 'pairing dialog failed: %{public}s', JSON.stringify(error));
          opened = false;
          cancel();
          finish(false);
        });
    }, PAIR_PROMPT_GRACE_MS);

    pairing.then(() => {
      finish(true);
    }, () => {
      finish(true);
    });
  });
}


