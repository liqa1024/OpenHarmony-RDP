import { ComponentContent, UIContext } from '@kit.ArkUI';
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpWolHelp';
const WOL_HELP_URL: string = 'https://www.msi.cn/support/technical_details/MB_Wake_On_LAN';

class WolHelpParams {
  onOpenLink: () => void = (): void => {};
  onDismiss: () => void = (): void => {};
}

@Builder
function wolHelpContent(params: WolHelpParams) {
  Column() {
    Text('启用网络唤醒（Wake-on-LAN）')
      .fontSize(18)
      .fontWeight(FontWeight.Medium)
      
    Text('需先在主机与路由器上开启 Wake-on-LAN，并允许网卡在关机 / 休眠时保持供电，否则唤醒包会被忽略。')
      .fontSize(13)
      
      .margin({ top: 10 })
    Flex({ wrap: FlexWrap.Wrap, alignItems: ItemAlign.Center }) {
      Text('可参考')
        .fontSize(13)
        
      Text('这篇教程')
        .fontSize(13)
        
        .decoration({ type: TextDecorationType.Underline })
        .onClick(() => {
          params.onOpenLink();
        })
      Text('进行设置。')
        .fontSize(13)
        
    }
    .margin({ top: 10 })

    Button('知道了')
      .width('100%')
      .height(44)
      .fontSize(16)
      
      
      .borderRadius(12)
      .margin({ top: 20 })
      .onClick(() => {
        params.onDismiss();
      })
  }
  .width(320)
  .padding(24)
  .alignItems(HorizontalAlign.Start)
  
  .borderRadius(20)
}

function openHelpLink(ui: UIContext): void {
  const context = ui.getHostContext();
  if (context === undefined) {
    return;
  }
  const ability: common.UIAbilityContext = context as common.UIAbilityContext;
  const want: Want = {
    action: 'ohos.want.action.viewData',
    entities: ['entity.system.browsable'],
    uri: WOL_HELP_URL
  };
  ability.startAbility(want).catch((error: BusinessError): void => {
    hilog.warn(DOMAIN, TAG, 'open WoL help link failed: %{public}s', JSON.stringify(error));
  });
}

export function promptWolHelp(ui: UIContext): Promise<void> {
  return new Promise<void>((resolve: () => void) => {
    let closed: boolean = false;
    const params: WolHelpParams = new WolHelpParams();
    const contentNode: ComponentContent<WolHelpParams> =
      new ComponentContent(ui, wrapBuilder(wolHelpContent), params);
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
          hilog.error(DOMAIN, TAG, 'close WoL help dialog failed: %{public}s', JSON.stringify(error));
        });
      resolve();
    };
    params.onOpenLink = (): void => {
      openHelpLink(ui);
    };
    params.onDismiss = dismiss;
    ui.getPromptAction().openCustomDialog(contentNode,
      { isModal: true, autoCancel: false })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'WoL help dialog failed: %{public}s', JSON.stringify(error));
        dismiss();
      });
  });
}


