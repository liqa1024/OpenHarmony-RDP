import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PairResult } from '../moonlight/MoonlightModels';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpPairing';

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
      
    Text('请在主机（Sunshine）上输入以下 PIN')
      .fontSize(13)
      
      .margin({ top: 8 })

    Row({ space: 12 }) {
      ForEach(params.digits, (digit: string, index: number) => {
        Text(digit)
          .fontSize(32)
          .fontWeight(FontWeight.Bold)
          
          .textAlign(TextAlign.Center)
          .width(56)
          .height(64)
          
          .borderRadius(12)
      }, (digit: string, index: number) => `${index}-${digit}`)
    }
    .margin({ top: 20 })

    Row({ space: 8 }) {
      LoadingProgress().width(16).height(16)
      Text('输入后自动完成配对')
        .fontSize(12)
        
    }
    .justifyContent(FlexAlign.Center)
    .margin({ top: 16 })

    Button('取消')
      .width('100%')
      .height(44)
      .fontSize(16)
      
      
      .borderRadius(12)
      .margin({ top: 20 })
      .onClick(() => {
        params.onCancel();
      })
  }
  .width('90%')
  .constraintSize({ maxWidth: 300 })
  .padding(24)
  .alignItems(HorizontalAlign.Center)
  
  .borderRadius(20)
}

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

    ui.getPromptAction().openCustomDialog(contentNode,
      { isModal: true, autoCancel: false })
      .then(() => {
        if (settled) {
          dismiss();
        }
      })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'pairing dialog failed: %{public}s', JSON.stringify(error));
        finish(false);
      });
    pairing.then(() => {
      finish(true);
    }, () => {
      finish(true);
    });
  });
}


