import { ComponentContent, UIContext } from '@kit.ArkUI';
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PromptDialog } from '../components/PromptDialog';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpWolHelp';
const WOL_HELP_URL: string = 'https://www.msi.cn/support/technical_details/MB_Wake_On_LAN';

class WolHelpParams {
  onOpenLink: () => void = (): void => {};
  onDismiss: () => void = (): void => {};
}

@Builder
function wolHelpContent(params: WolHelpParams) {
  PromptDialog({
    title: '启用网络唤醒（Wake-on-LAN）',
    message: '需在主机与路由器上开启 Wake-on-LAN，并保持网卡关机 / 休眠供电。',
    confirmLabel: '知道了',
    onConfirm: () => {
      params.onDismiss();
    }
  }) {
    Flex({ wrap: FlexWrap.Wrap, alignItems: ItemAlign.Center }) {
      Text('设置方法见')
        .fontSize(14)
        .fontColor($r('app.color.text_secondary'))
      Text('这篇教程')
        .fontSize(14)
        .fontColor($r('app.color.brand_text'))
        .decoration({ type: TextDecorationType.Underline })
        .onClick(() => {
          params.onOpenLink();
        })
      Text('。')
        .fontSize(14)
        .fontColor($r('app.color.text_secondary'))
    }
    .margin({ top: 12 })
  }
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
    let contentNode: ComponentContent<WolHelpParams> | undefined = undefined;
    const disposeContent = (): void => {
      const node: ComponentContent<WolHelpParams> | undefined = contentNode;
      if (node === undefined) {
        return;
      }
      try {
        node.dispose();
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'dispose WoL help dialog failed: %{public}s', JSON.stringify(error));
      }
    };
    const dismiss = (): void => {
      if (closed) {
        return;
      }
      closed = true;
      const node: ComponentContent<WolHelpParams> | undefined = contentNode;
      if (node === undefined) {
        resolve();
        return;
      }
      ui.getPromptAction().closeCustomDialog(node)
        .then(() => {
          disposeContent();
          resolve();
        })
        .catch((error: BusinessError) => {
          hilog.error(DOMAIN, TAG, 'close WoL help dialog failed: %{public}s', JSON.stringify(error));
          disposeContent();
          resolve();
        });
    };
    // Wire callbacks before creating the content node: the builder may
    // capture the parameter object at construction time.
    params.onOpenLink = (): void => {
      openHelpLink(ui);
    };
    params.onDismiss = dismiss;
    contentNode = new ComponentContent(ui, wrapBuilder(wolHelpContent), params);
    ui.getPromptAction().openCustomDialog(contentNode,
      { isModal: true, autoCancel: false })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'WoL help dialog failed: %{public}s', JSON.stringify(error));
        dismiss();
      });
  });
}


