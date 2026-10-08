/*
 * HmRdp - Moonlight pairing prompt.
 *
 * The pairing PIN is a lightweight popup over the calling window: the client
 * generated the PIN and the user types it on the host (Sunshine). The host
 * entering the PIN is the confirmation, so the dialog closes on its own once the
 * pairing handshake settles — no manual confirm button. Only cancellation is a
 * user action. Shared by every entry point that can start a pairing (the list,
 * the editor and the app page).
 */
import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PairResult } from '../moonlight/MoonlightModels';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpPairing';

/** Content of the pairing dialog: the four PIN digits and the cancel action. */
class PairingDialogParams {
  digits: string[] = [];
  onCancel: () => void = (): void => {};
}

@Builder
function pairingDialogContent(params: PairingDialogParams) {
  Column() {
    Text('与主机配对')
      .fontSize(18)
      .fontWeight(FontWeight.Medium)
      .fontColor($r('app.color.text_primary'))
    Text('请在主机（Sunshine）上输入以下 PIN')
      .fontSize(13)
      .fontColor($r('app.color.text_secondary'))
      .margin({ top: 8 })

    Row({ space: 12 }) {
      ForEach(params.digits, (digit: string, index: number) => {
        Text(digit)
          .fontSize(32)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.accent'))
          .textAlign(TextAlign.Center)
          .width(56)
          .height(64)
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(12)
      }, (digit: string, index: number) => `${index}-${digit}`)
    }
    .margin({ top: 20 })

    Row({ space: 8 }) {
      LoadingProgress().width(16).height(16).color($r('app.color.text_tertiary'))
      Text('输入后自动完成配对')
        .fontSize(12)
        .fontColor($r('app.color.text_tertiary'))
    }
    .justifyContent(FlexAlign.Center)
    .margin({ top: 16 })

    Button('取消')
      .width('100%')
      .height(44)
      .fontSize(16)
      .fontColor($r('app.color.text_secondary'))
      .backgroundColor($r('app.color.input_bg'))
      .borderRadius(12)
      .margin({ top: 20 })
      .onClick(() => {
        params.onCancel();
      })
  }
  .width(300)
  .padding(24)
  .alignItems(HorizontalAlign.Center)
  .backgroundColor($r('app.color.card_bg'))
  .borderRadius(20)
}

/**
 * Shows the PIN and resolves once the wait ends: `false` when the user cancels,
 * `true` when the pairing handshake settles (the dialog then closes itself).
 */
export async function promptPairingPin(ui: UIContext, pin: string,
  pairing: Promise<PairResult>): Promise<boolean> {
  return new Promise<boolean>((resolve: (value: boolean) => void) => {
    let settled: boolean = false;
    let closed: boolean = false;
    const params: PairingDialogParams = new PairingDialogParams();
    params.digits = [pin.substring(0, 1), pin.substring(1, 2),
      pin.substring(2, 3), pin.substring(3, 4)];
    const contentNode: ComponentContent<PairingDialogParams> =
      new ComponentContent(ui, wrapBuilder(pairingDialogContent), params);

    const dismiss = (): void => {
      if (closed) {
        return;
      }
      closed = true;
      ui.getPromptAction().closeCustomDialog(contentNode)
        .then(() => {
          contentNode.dispose();
        })
        .catch((error: BusinessError) => {
          hilog.error(DOMAIN, TAG, 'close pairing dialog failed: %{public}s', JSON.stringify(error));
        });
    };
    const finish = (proceed: boolean): void => {
      if (settled) {
        return;
      }
      settled = true;
      dismiss();
      resolve(proceed);
    };
    params.onCancel = (): void => {
      finish(false);
    };

    ui.getPromptAction().openCustomDialog(contentNode, { isModal: true, autoCancel: false })
      .then(() => {
        if (settled) {
          dismiss();
        }
      })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'pairing dialog failed: %{public}s', JSON.stringify(error));
        finish(false);
      });
    // Auto-dismiss as soon as the handshake settles: the host entering the PIN
    // is the confirmation. A failure also closes the dialog and is reported by
    // the awaited PairResult afterwards.
    pairing.then(() => {
      finish(true);
    }, () => {
      finish(true);
    });
  });
}
