import { ClipboardButtons, ClipboardTransferStatus, SessionTitle, TelemetryField,
  TelemetryRow, ToolbarChip, ToolbarIconChip } from './SessionChrome';
import { TOOLBAR_RADIUS, TOOLBAR_ROW_HEIGHT, TOOLBAR_SNAP_RATIO,
  TOOLBAR_TELEMETRY_HEIGHT, TOOLBAR_UNSNAP_RATIO, TOOLBAR_ZONE_BAR,
  TOOLBAR_ZONE_NONE } from '../services/ToolbarContract';
import { lengthToNumber } from '../utils/UiUtil';

// Both rows carry this on each side; the pill has to add it on top of the
// measured content width, the bar gets it from its own full width.
const ROW_SIDE_PAD: number = 16;
// Smallest breathing room between the title group and the action group; the
// measured natural width adds it back on top of the two group widths.
const TITLE_ACTIONS_GAP: number = 24;

@Component
export struct SessionToolbar {
  @Prop titleName: string = '';
  @Prop titleDetail: string = '';
  @Prop isFullscreen: boolean = false;
  @Prop pinned: boolean = false;
  @Prop minWidthPercent: number = 50;
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

  // Measured widths of the three natural-width groups (no padding): the two
  // ends of the main row, and the wider of the two telemetry rows. Each group
  // measures its own content whether or not its parent is stretched, so the
  // natural width stays readable in the full-bleed form too — no latch, no
  // second measuring pass.
  private leadWidth: number = 0;
  private trailWidth: number = 0;
  private teleWidth: number = 0;
  private detailWidth: number = 0;
  @State naturalWidth: number = 0;
  @State availableWidth: number = 0;
  @State snapped: boolean = false;

  private contentWidth(): number {
    const main: number = this.leadWidth + TITLE_ACTIONS_GAP + this.trailWidth;
    const tele: number = this.showTelemetry
      ? Math.max(this.teleWidth, this.detailWidth) : 0;
    return Math.max(main, tele);
  }

  private pushNatural(): void {
    const content: number = this.contentWidth();
    const natural: number = content > 0 ? content + ROW_SIDE_PAD * 2 : 0;
    if (natural !== this.naturalWidth) {
      this.naturalWidth = natural;
    }
    this.updateSnap(natural);
  }

  private appliedWidth(natural: number): number {
    const available: number = this.availableWidth;
    if (available <= 0) {
      return 0;
    }
    const floor: number = available * this.minWidthPercent / 100;
    return Math.min(Math.max(natural, floor), available);
  }

  // Snap on the width that would actually be drawn (the floor can be what
  // brings it near full width), with hysteresis so a content change at the
  // threshold cannot make the bar flip back and forth.
  private updateSnap(natural: number): void {
    const available: number = this.availableWidth;
    if (available <= 0 || natural <= 0) {
      return;
    }
    const ratio: number = this.appliedWidth(natural) / available;
    if (this.snapped) {
      if (ratio < TOOLBAR_UNSNAP_RATIO) {
        this.snapped = false;
      }
    } else if (ratio >= TOOLBAR_SNAP_RATIO) {
      this.snapped = true;
    }
  }

  private onAvailableArea(area: Area): void {
    this.availableWidth = lengthToNumber(area.width);
    this.pushNatural();
  }

  private onLeadArea(area: Area): void {
    this.leadWidth = lengthToNumber(area.width);
    this.pushNatural();
  }

  private onTrailArea(area: Area): void {
    this.trailWidth = lengthToNumber(area.width);
    this.pushNatural();
  }

  private onTeleArea(area: Area): void {
    this.teleWidth = lengthToNumber(area.width);
    this.pushNatural();
  }

  private onDetailArea(area: Area): void {
    this.detailWidth = lengthToNumber(area.width);
    this.pushNatural();
  }

  // The bar form is the full-bleed one: windowed sessions always use it, a
  // fullscreen bar snaps into it.
  private isBar(): boolean {
    return !this.isFullscreen || this.snapped;
  }

  private hasWidth(): boolean {
    return this.naturalWidth > 0 && this.availableWidth > 0;
  }

  // Never 'auto': the rows size themselves against this width, and the floor is
  // a correct-looking pill on the frames before the content has been measured
  // (and stays one if a measurement never lands).
  private pillWidth(): Length {
    if (!this.hasWidth()) {
      return this.minWidthPercent + '%';
    }
    return this.appliedWidth(this.naturalWidth);
  }

  @Builder
  titleGroup() {
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
          .margin({ right: 8 })
          // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
          .onTouch(() => {})
          .bindTips(this.pinned ? '取消固定工具栏' : '固定工具栏（不自动隐藏）', this.tipsOptions)
      }

      SessionTitle({
        name: this.titleName,
        detail: this.titleDetail
      })
    }
    .onAreaChange((_old: Area, area: Area) => {
      this.onLeadArea(area);
    })
  }

  @Builder
  actionGroup() {
    Row() {
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
        Row().width(12)
      }

      ToolbarChip({
        label: this.isFullscreen ? '退出全屏' : '全屏',
        onTap: () => {
          this.onToggleFullscreen();
        }
      })
        .margin({ right: 6 })

      ToolbarChip({
        label: '最小化',
        onTap: () => {
          this.onMinimize();
        }
      })
        .margin({ right: 6 })

      if (this.disconnectTip.length > 0) {
        ToolbarChip({
          label: '断开',
          danger: true,
          onTap: () => {
            this.onDisconnect();
          }
        })
          .margin({ right: this.showCloseApp ? 6 : 0 })
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
          .margin({ right: this.showCloseApp ? 6 : 0 })
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
    .onAreaChange((_old: Area, area: Area) => {
      this.onTrailArea(area);
    })
  }

  @Builder
  mainRow() {
    Row() {
      this.titleGroup()
      // A plain fixed spacer, not Blank(min): Blank stops absorbing the
      // remaining space as soon as a size/min is set on it. SpaceBetween does
      // the spread, and both forms give this row a definite width, so it never
      // has to fall back on a measured value.
      Row().width(TITLE_ACTIONS_GAP)
      this.actionGroup()
    }
    .width('100%')
    .justifyContent(FlexAlign.SpaceBetween)
    .height(TOOLBAR_ROW_HEIGHT)
    .padding({ left: ROW_SIDE_PAD, right: ROW_SIDE_PAD })
  }

  @Builder
  telemetryFieldRow() {
    Row() {
      Row() {
        TelemetryRow({ fields: this.telemetryFields })
      }
      .onAreaChange((_old: Area, area: Area) => {
        this.onTeleArea(area);
      })
    }
    .width('100%')
    .height(TOOLBAR_TELEMETRY_HEIGHT)
    .padding({ left: ROW_SIDE_PAD, right: ROW_SIDE_PAD })
  }

  @Builder
  telemetryDetailRow() {
    Row() {
      Row() {
        TelemetryRow({ fields: this.detailFields })
      }
      .onAreaChange((_old: Area, area: Area) => {
        this.onDetailArea(area);
      })
    }
    .width('100%')
    .height(TOOLBAR_TELEMETRY_HEIGHT)
    .padding({ left: ROW_SIDE_PAD, right: ROW_SIDE_PAD })
  }

  @Builder
  toolbarContent() {
    this.mainRow()
    if (this.showTelemetry) {
      this.telemetryFieldRow()
      this.telemetryDetailRow()
    }
  }

  @Builder
  pillForm() {
    Column() {
      this.toolbarContent()
    }
    .width(this.pillWidth())
    .alignItems(HorizontalAlign.Start)
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

  @Builder
  barContent() {
    this.toolbarContent()
    Divider()
      .strokeWidth(1)
      .color(this.isFullscreen ? $r('app.color.chrome_border') : $r('app.color.divider'))
  }

  // Full-bleed form over the picture: session chrome, only a bottom hairline.
  @Builder
  overlayBar() {
    Column() {
      this.barContent()
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
    .backgroundColor($r('app.color.chrome_bg'))
    .backgroundBlurStyle(BlurStyle.COMPONENT_THICK)
    .onHover((isHover: boolean) => {
      this.onPointerZone(isHover ? TOOLBAR_ZONE_BAR : TOOLBAR_ZONE_NONE);
    })
  }

  // Full-bleed form in a window: the bar sits on the page background above the
  // picture, so it takes the page bar material (same as PageHeader).
  @Builder
  windowedBar() {
    Column() {
      this.barContent()
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
    .backgroundColor($r('app.color.header_glass'))
    .backgroundEffect(this.pageBarEffect())
  }

  private pageBarEffect(): BackgroundEffectOptions {
    const options: BackgroundEffectOptions = {
      radius: 24,
      saturation: 1.1,
      brightness: 1
    };
    return options;
  }

  build() {
    Row() {
      if (this.isBar()) {
        if (this.isFullscreen) {
          this.overlayBar()
        } else {
          this.windowedBar()
        }
      } else {
        this.pillForm()
      }
    }
    .width('100%')
    .justifyContent(FlexAlign.Center)
    .hitTestBehavior(HitTestMode.None)
    .onAreaChange((_old: Area, area: Area) => {
      this.onAvailableArea(area);
    })
    .transition(TransitionEffect.translate({ y: -this.toolbarHeight })
      .animation({ duration: 180, curve: Curve.EaseOut }))
  }
}
