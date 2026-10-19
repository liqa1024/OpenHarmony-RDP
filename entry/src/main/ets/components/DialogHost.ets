import { DialogRequest, DialogResult, feedbackHost } from '../utils/FeedbackHost';
import { MaterialSupport } from '../utils/MaterialSupport';
const DIALOG_RADIUS: number = 20;
const DIALOG_WIDTH: number = 360;

// 确认/通知弹窗外壳。内容经 @Link 绑定（官方口径：自定义弹窗外壳的动态数据只能用
// @Link / @Consume）。底色/描边/阴影由弹窗容器负责——支持沉浸光感时是系统材质，
// 否则由 DialogHost 写在 options 里的回退样式提供，所以内容层不再铺底色。
@CustomDialog
struct DialogShell {
  controller?: CustomDialogController;
  @Link title: string;
  @Link message: string;
  @Link cancelLabel: string;
  @Link confirmLabel: string;
  @Link danger: boolean;
  // 弹窗容器是否已由系统材质铺底。由 DialogHost 与 options 用**同一次**判定结果传入，
  // 避免"这里判支持、那边判不支持"造成透明空壳。
  glass: boolean = false;
  onCancel: () => void = () => {};
  onConfirm: () => void = () => {};

  private panelShadow(): ShadowOptions {
    if (this.glass) {
      const none: ShadowOptions = { radius: 0, color: Color.Transparent };
      return none;
    }
    const normal: ShadowOptions = {
      radius: 16, color: $r('app.color.shadow_2'), offsetX: 0, offsetY: 4
    };
    return normal;
  }

  build() {
    Column() {
      if (this.title.length > 0) {
        Text(this.title)
          .fontSize(20)
          .fontWeight(600)
          .fontColor($r('app.color.text_primary'))
          .textAlign(TextAlign.Center)
          .width('100%')
      }
      if (this.message.length > 0) {
        Text(this.message)
          .fontSize(14)
          .lineHeight(20)
          .fontColor($r('app.color.text_secondary'))
          .textAlign(TextAlign.Center)
          .width('100%')
          .margin({ top: 8 })
      }

      if (this.cancelLabel.length > 0 || this.confirmLabel.length > 0) {
        Row({ space: 12 }) {
          if (this.cancelLabel.length > 0) {
            Button(this.cancelLabel)
              .layoutWeight(1)
              .height(44)
              .fontSize(16)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
              .backgroundColor($r('app.color.control_bg'))
              .border({ width: 1, color: $r('app.color.control_border') })
              .borderRadius(12)
              .onClick(() => {
                this.onCancel();
              })
          }
          if (this.confirmLabel.length > 0) {
            Button(this.confirmLabel)
              .layoutWeight(1)
              .height(44)
              .fontSize(16)
              .fontWeight(FontWeight.Medium)
              .fontColor(this.danger
                ? $r('app.color.danger') : $r('app.color.on_brand'))
              .backgroundColor(this.danger
                ? $r('app.color.danger_tint') : $r('app.color.brand_fill'))
              .borderRadius(12)
              .onClick(() => {
                this.onConfirm();
              })
          }
        }
        .width('100%')
        .margin({ top: 24 })
      }
    }
    .width('100%')
    .padding(24)
    .backgroundColor(this.glass ? Color.Transparent : $r('app.color.surface'))
    .border({ width: this.glass ? 0 : 1, color: $r('app.color.border') })
    .shadow(this.panelShadow())
    .borderRadius(DIALOG_RADIUS)
    .alignItems(HorizontalAlign.Center)
  }
}

// 每个窗口根节点挂一个（0 尺寸占位）。它把确认/通知弹窗用系统 CustomDialog 承载
// （customStyle:false），这是唯一能吃沉浸光感的弹窗通道；openCustomDialog 的材质不生效。
@Component
export struct DialogHost {
  @State title: string = '';
  @State message: string = '';
  @State cancelLabel: string = '';
  @State confirmLabel: string = '';
  @State danger: boolean = false;
  private pending: DialogRequest | undefined = undefined;
  // 必须在 controller 之前求值：材质判定只做一次，options 与 DialogShell 共用同一结果。
  private materialOk: boolean = MaterialSupport.dialogGlass();

  // 控制器在成员初始化处创建、builder 的 struct 调用直接写在 options 字面量里——
  // 这是官方要求的形态，只有这样才能被编译成 new。
  // 材质厚度由设置决定；跟随官方默认时不设 systemMaterial（容器自动开启）。
  private controller: CustomDialogController = new CustomDialogController({
    builder: DialogShell({
      title: this.title,
      message: this.message,
      cancelLabel: this.cancelLabel,
      confirmLabel: this.confirmLabel,
      danger: this.danger,
      glass: this.materialOk,
      onCancel: (): void => {
        this.finish(DialogResult.Cancel);
      },
      onConfirm: (): void => {
        this.finish(DialogResult.Confirm);
      }
    }),
    customStyle: false,
    cornerRadius: DIALOG_RADIUS,
    width: DIALOG_WIDTH,
    isModal: true,
    autoCancel: false,
    alignment: DialogAlignment.Center,
    systemMaterial: this.materialOk ? MaterialSupport.dialogMaterial() : undefined,
    onWillDisappear: (): void => {
      this.notifyDismissed();
    }
  });

  private onPresent: (request: DialogRequest) => void = (request: DialogRequest): void => {
    this.present(request);
  };
  private onDismiss: () => void = (): void => {
    this.close();
  };

  aboutToAppear(): void {
    feedbackHost(this.getUIContext()).attachDialog(this.onPresent, this.onDismiss);
  }

  aboutToDisappear(): void {
    feedbackHost(this.getUIContext()).detachDialog(this.onPresent);
    this.close();
  }

  build() {
    Row() {
    }
    .width(0)
    .height(0)
  }

  private present(request: DialogRequest): void {
    this.close();
    this.pending = request;
    this.title = request.title;
    this.message = request.message;
    this.cancelLabel = request.cancelLabel;
    this.confirmLabel = request.confirmLabel;
    this.danger = request.danger;
    this.controller.open();
  }

  private finish(result: DialogResult): void {
    const request: DialogRequest | undefined = this.pending;
    this.pending = undefined;
    this.close();
    if (request !== undefined) {
      request.onResult(result);
    }
  }

  // 被返回键/ESC 等外部方式关闭时，也要结算，避免模态队列卡住。
  private notifyDismissed(): void {
    const request: DialogRequest | undefined = this.pending;
    if (request === undefined) {
      return;
    }
    this.pending = undefined;
    request.onResult(DialogResult.Dismissed);
  }

  private close(): void {
    this.controller.close();
  }
}