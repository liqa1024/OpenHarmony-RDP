import { ComponentContent, UIContext } from '@kit.ArkUI';
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PromptDialog } from '../components/PromptDialog';
import { DialogSession } from './DialogSession';
import { feedbackHost, ModalRequest } from './FeedbackHost';

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

class WolHelpRequest implements ModalRequest {
  private ui: UIContext;
  private params: WolHelpParams = new WolHelpParams();
  private session: DialogSession<WolHelpParams>;
  private resolve: () => void;
  private done: () => void = (): void => {};
  private onClosed: () => void = (): void => {};
  private settled: boolean = false;
  private finished: boolean = false;

  constructor(ui: UIContext, resolve: () => void) {
    this.ui = ui;
    this.resolve = resolve;
    this.session = new DialogSession<WolHelpParams>(ui);
  }

  start(done: () => void, closed: () => void): void {
    this.done = done;
    this.onClosed = closed;
    if (this.settled) {
      this.finishHost();
      return;
    }
    // Wire callbacks before creating the content node: the builder may capture
    // the parameter object at construction time.
    this.params.onOpenLink = (): void => {
      openHelpLink(this.ui);
    };
    this.params.onDismiss = (): void => {
      this.finish();
    };
    const content: ComponentContent<WolHelpParams> =
      new ComponentContent(this.ui, wrapBuilder(wolHelpContent), this.params);
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
    this.resolve();
  }

  private finish(): void {
    if (this.settled) {
      return;
    }
    this.settled = true;
    this.session.close((): void => {
      this.onClosed();
      this.resolve();
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
}

export function promptWolHelp(ui: UIContext): Promise<void> {
  return new Promise<void>((resolve: () => void) => {
    feedbackHost(ui).enqueue(new WolHelpRequest(ui, resolve));
  });
}
