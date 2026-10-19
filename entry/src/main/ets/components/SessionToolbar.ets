import { ClipboardButtons, ClipboardTransferStatus, SessionTitle, TelemetryField,
  TelemetryRow, ToolbarChip, ToolbarIconChip } from './SessionChrome';
import { TOOLBAR_RADIUS, TOOLBAR_ROW_HEIGHT, TOOLBAR_ZONE_BAR,
  TOOLBAR_ZONE_NONE } from '../services/ToolbarContract';

@Component
export struct SessionToolbar {
  @Prop titleName: string = '';
  @Prop titleDetail: string = '';
  @Prop isFullscreen: boolean = false;
  @Prop pinned: boolean = false;
  @Prop toolbarWidthPercent: number = 100;
  @Prop toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  @Prop showTelemetry: boolean = false;
  @Prop telemetryFields: TelemetryField[] = [];
  @Prop detailFields: TelemetryField[] = [];
  @Prop showTransfer: boolean = false;
  @Prop transferWaiting: boolean = false;
  @Prop transferPercent: number = 0;
  @Prop transferText: string = '';
  @Prop transferSpeed: string = '';
  @Prop transferFileIndex: number = 0;
  @Prop transferFileCount: number = 0;
  @Prop showClipboard: boolean = false;
  @Prop showCloseApp: boolean = false;
  @Prop disconnectTip: string = '';
  pasteCallback: PasteButtonCallback = () => {};
  onTogglePin: () => void = () => {};
  onCopy: () => void = () => {};
  onCancelTransfer: () => void = () => {};
  onToggleFullscreen: () => void = () => {};
  onMinimize: () => void = () => {};
  onDisconnect: () => void = () => {};
  onCloseApp: () => void = () => {};
  onPointerZone: (zone: number) => void = (zone: number) => {};
  private tipsOptions: TipsOptions = {};

  @Builder
  mainRow() {
    Row() {
      if (this.isFullscreen) {
        ToolbarIconChip({
          icon: $r('app.media.ic_pin'),
          iconSize: 20,
          iconAngle: 45,
          active: this.pinned,
          onTap: () => {
            this.onTogglePin();
          }
        })
          .margin({ right: 10 })
          // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
          .onTouch(() => {})
          .bindTips(this.pinned ? '取消固定工具栏' : '固定工具栏（不自动隐藏）', this.tipsOptions)
      }

      SessionTitle({
        name: this.titleName,
        detail: this.titleDetail
      })

      Blank()

      if (this.showTransfer) {
        ClipboardTransferStatus({
          waiting: this.transferWaiting,
          percent: this.transferPercent,
          text: this.transferText,
          speed: this.transferSpeed,
          fileIndex: this.transferFileIndex,
          fileCount: this.transferFileCount,
          onCancel: () => {
            this.onCancelTransfer();
          }
        })
      }

      if (this.showClipboard) {
        ClipboardButtons({
          onCopy: () => {
            this.onCopy();
          },
          pasteCallback: this.pasteCallback
        })
        Row().width(16)
      }

      ToolbarChip({
        label: this.isFullscreen ? '退出全屏' : '全屏',
        onTap: () => {
          this.onToggleFullscreen();
        }
      })
        .margin({ right: 8 })

      ToolbarChip({
        label: '最小化',
        onTap: () => {
          this.onMinimize();
        }
      })
        .margin({ right: 8 })

      if (this.disconnectTip.length > 0) {
        ToolbarChip({
          label: '断开',
          danger: true,
          onTap: () => {
            this.onDisconnect();
          }
        })
          .margin({ right: this.showCloseApp ? 8 : 0 })
          // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
          .onTouch(() => {})
          .bindTips(this.disconnectTip, this.tipsOptions)
      } else {
        ToolbarChip({
          label: '断开',
          danger: true,
          onTap: () => {
            this.onDisconnect();
          }
        })
          .margin({ right: this.showCloseApp ? 8 : 0 })
      }

      if (this.showCloseApp) {
        ToolbarChip({
          label: '关闭应用',
          danger: true,
          onTap: () => {
            this.onCloseApp();
          }
        })
          // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
          .onTouch(() => {})
          .bindTips('关闭主机应用并断开', this.tipsOptions)
      }
    }
    .width('100%')
    .height(TOOLBAR_ROW_HEIGHT)
    .padding({ left: 16, right: 16 })
  }

  build() {
    Row() {
      Column() {
        this.mainRow()
        if (this.showTelemetry) {
          TelemetryRow({ fields: this.telemetryFields })
          TelemetryRow({ fields: this.detailFields })
        }
      }
      .width(this.toolbarWidthPercent + '%')
      .borderRadius(TOOLBAR_RADIUS)
      .backgroundColor($r('app.color.chrome_bg'))
      .backgroundBlurStyle(BlurStyle.COMPONENT_THICK)
      .border({ width: 1, color: $r('app.color.chrome_border') })
      .shadow({ radius: 16, color: $r('app.color.shadow_2'), offsetX: 0, offsetY: 4 })
      .clip(true)
      .onHover((isHover: boolean) => {
        this.onPointerZone(isHover ? TOOLBAR_ZONE_BAR : TOOLBAR_ZONE_NONE);
      })
    }
    .width('100%')
    .justifyContent(FlexAlign.Center)
    .hitTestBehavior(HitTestMode.None)
    .transition(TransitionEffect.translate({ y: -this.toolbarHeight })
      .animation({ duration: 180, curve: Curve.EaseOut }))
  }
}
