import { HostPower, RdpConnection, RowMenuAction, RowMenuEntry,
  SavedConnection } from '../model/Connections';
import { ConnectionRowBadge, ConnectionRowIcon, ConnectionRowIntent,
  ConnectionRowModel } from '../model/ConnectionRowModel';
import { ConnectionState } from '../services/SessionManager';
import { WakeReachability } from '../services/WakeOnLan';
import { fadeTransition, hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS }
  from '../utils/UiMotion';

@Component
export struct ConnectionRow {
  conn: SavedConnection = new RdpConnection();
  @Prop state: ConnectionState = ConnectionState.Idle;
  @Prop hostTargetId: number = 0;
  @Prop targetId: number = 0;
  @Prop power: HostPower = HostPower.Online;
  @Prop wakeReachability: WakeReachability = WakeReachability.Unknown;
  @Prop waking: boolean = false;
  @Prop probing: boolean = false;
  @Prop @Watch('onMenuRequest') menuRequest: number = 0;
  @State appHovered: boolean = false;
  @State appPressed: boolean = false;
  @State rowHovered: boolean = false;
  @State menuShown: boolean = false;
  @State menuWinX: number = 0;
  @State menuWinY: number = 0;
  onEdit: () => void = () => {};
  onToggleConnection: () => void = () => {};
  onOpenApps: () => void = () => {};
  onTerminate: () => void = () => {};
  onDelete: () => void = () => {};
  onRefresh: () => void = () => {};
  onWake: () => void = () => {};
  onRequestMenu: () => void = () => {};
  onPressRelease: () => void = () => {};
  private tipsOptions: TipsOptions = {};

  private model(): ConnectionRowModel {
    return ConnectionRowModel.of(this.conn, this.state, this.hostTargetId, this.targetId,
      this.power, this.wakeReachability, this.waking, this.probing);
  }

  private busy(): boolean {
    return this.model().busy;
  }

  private menuAllowed(): boolean {
    return this.model().menuAllowed;
  }

  private hostDown(): boolean {
    return this.model().hostDown;
  }

  private wakeDisabled(): boolean {
    return this.model().wakeDisabled;
  }

  private wakeLabel(): string {
    if (!this.conn.supportsWake()) {
      return '缺少主机 MAC';
    }
    if (this.wakeReachability === WakeReachability.OffLan) {
      return '不在主机局域网';
    }
    return '无法唤醒';
  }

  private actionBg(): Resource {
    if (this.model().icon === ConnectionRowIcon.Stop) {
      return $r('app.color.danger_tint');
    }
    if (this.model().icon === ConnectionRowIcon.Refresh
      || this.model().icon === ConnectionRowIcon.Power) {
      return $r('app.color.control_bg');
    }
    return $r('app.color.brand_tint');
  }

  private actionIconColor(): Resource {
    if (this.model().icon === ConnectionRowIcon.Stop) {
      return $r('app.color.danger');
    }
    if (this.model().icon === ConnectionRowIcon.Refresh
      || this.model().icon === ConnectionRowIcon.Power) {
      return $r('app.color.icon_secondary');
    }
    return $r('app.color.brand_text');
  }

  private actionBorder(): Resource {
    if (this.model().icon === ConnectionRowIcon.Stop) {
      return $r('app.color.danger_border');
    }
    if (this.model().icon === ConnectionRowIcon.Refresh
      || this.model().icon === ConnectionRowIcon.Power) {
      return $r('app.color.control_border');
    }
    return $r('app.color.brand_tint_strong');
  }

  private primaryTap(): void {
    const intent: number = this.model().intent;
    if (intent === ConnectionRowIntent.Wake) {
      this.onWake();
    } else if (intent === ConnectionRowIntent.Refresh) {
      this.onRefresh();
    } else if (intent === ConnectionRowIntent.Terminate) {
      this.onTerminate();
    } else if (intent === ConnectionRowIntent.Toggle) {
      this.onToggleConnection();
    }
  }

  private onMenuRequest(): void {
    if (!this.menuAllowed()) {
      this.menuShown = false;
      return;
    }
    if (this.menuRequest > 0) {
      this.menuShown = false;
      this.menuShown = true;
    } else {
      this.menuShown = false;
    }
  }

  private menuOptions(): ContextMenuOptions {
    const options: ContextMenuOptions = {
      placement: Placement.BottomRight,
      offset: { x: this.menuWinX, y: this.menuWinY },
      modalMode: ModalMode.TARGET_WINDOW,
      aboutToDisappear: () => {
        this.menuShown = false;
      }
    };
    return options;
  }

  private onRowTouch(event: TouchEvent): void {
    if (event.type === TouchType.Down && event.touches.length > 0) {
      this.menuWinX = event.touches[0].windowX;
      this.menuWinY = event.touches[0].windowY;
    } else if (event.type === TouchType.Up) {
      if (this.menuAllowed()) {
        this.onPressRelease();
      }
    }
  }

  private onMenuEntry(entry: RowMenuEntry): void {
    if (entry.action === RowMenuAction.Toggle) {
      this.onToggleConnection();
    } else if (entry.action === RowMenuAction.Terminate) {
      this.onTerminate();
    } else if (entry.action === RowMenuAction.Refresh) {
      this.onRefresh();
    } else if (entry.action === RowMenuAction.Wake) {
      this.onWake();
    } else if (entry.action === RowMenuAction.Edit) {
      this.onEdit();
    } else if (entry.action === RowMenuAction.Delete) {
      this.onDelete();
    }
  }

  private menuGroup(session: boolean): RowMenuEntry[] {
    return this.conn.menuEntries(this.model().action, this.hostTargetId, this.power)
      .filter((entry: RowMenuEntry) => entry.sessionAction === session)
      .filter((entry: RowMenuEntry) =>
        !(this.wakeDisabled() && entry.action === RowMenuAction.Wake));
  }

  @Builder
  menuItem(entry: RowMenuEntry) {
    MenuItem({ content: entry.content })
      .onClick(() => {
        this.onMenuEntry(entry);
      })
  }

  @Builder
  menu() {
    Menu() {
      MenuItemGroup() {
        ForEach(this.menuGroup(true), (entry: RowMenuEntry) => {
          this.menuItem(entry)
        }, (entry: RowMenuEntry) => `${entry.action}#${entry.content}`)
      }
      MenuItemGroup() {
        ForEach(this.menuGroup(false), (entry: RowMenuEntry) => {
          this.menuItem(entry)
        }, (entry: RowMenuEntry) => `${entry.action}#${entry.content}`)
      }
    }
  }

  build() {
    Row() {
      Row() {
        Image(this.conn.protocolIcon())
          .width(40)
          .height(40)
          .border({ width: 1, color: $r('app.color.control_border') })
          .borderRadius(10)
          .clip(true)
          .opacity(this.hostDown() ? 0.45 : 1.0)
          .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
          .draggable(false)
          .hitTestBehavior(HitTestMode.None)
        Column() {
          Row() {
            Text(this.conn.label())
              .fontSize(16)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
            if (this.model().badge === ConnectionRowBadge.Connected) {
              Text('已连接')
                .fontSize(10)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.success'))
                .backgroundColor($r('app.color.success_tint'))
                .borderRadius(4)
                .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                .margin({ left: 8 })
                .transition(fadeTransition())
            } else if (this.model().badge === ConnectionRowBadge.Running) {
              Text('运行中')
                .fontSize(10)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.protocol_moonlight_deep'))
                .backgroundColor($r('app.color.protocol_moonlight_tint'))
                .borderRadius(4)
                .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                .margin({ left: 8 })
                .transition(fadeTransition())
            }
          }
          if (this.conn.subtitle().length > 0) {
            Text(this.conn.subtitle())
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .margin({ top: 3 })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        .margin({ left: 12 })
        .hitTestBehavior(HitTestMode.None)
      }
      .layoutWeight(1)
      // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
      .onTouch(() => {})
      .bindTips('编辑连接', this.tipsOptions)
      .onClick(() => {
        this.onEdit();
      })
      .focusable(true)
      .focusOnTouch(true)

      if (this.wakeDisabled()) {
        Text(this.wakeLabel())
          .fontSize(10)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_tertiary'))
          .backgroundColor($r('app.color.surface_sunken'))
          .borderRadius(4)
          .padding({ left: 6, right: 6, top: 2, bottom: 2 })
          .margin({ left: 8 })
          .maxLines(1)
      }

      if (this.conn.showsAppsButton() && !this.hostDown()) {
        Row() {
          Image($r('app.media.ic_apps'))
            .width(24)
            .height(24)
            .fillColor($r('app.color.icon_primary'))
            .draggable(false)
            .hitTestBehavior(HitTestMode.None)
        }
        .width(44)
        .height(44)
        .backgroundColor(this.appPressed ? $r('app.color.control_bg_pressed')
          : (this.appHovered ? $r('app.color.control_bg_hover') : $r('app.color.control_bg')))
        .border({ width: 1, color: $r('app.color.control_border') })
        .borderRadius(22)
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .margin({ left: 8 })
        .scale(hoverPressScale(this.appHovered, this.appPressed))
        .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
        .onHover((isHover: boolean) => {
          this.appHovered = isHover;
          if (!isHover) {
            this.appPressed = false;
          }
        })
        .onMouse((event: MouseEvent) => {
          if (event.action === MouseAction.Press) {
            this.appPressed = true;
          } else if (event.action === MouseAction.Release) {
            this.appPressed = false;
          }
        })
        .onTouch((event: TouchEvent) => {
          if (event.type === TouchType.Down) {
            this.appPressed = true;
          } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
            this.appPressed = false;
          }
        })
        .bindTips('应用', this.tipsOptions)
        .onClick(() => {
          this.onOpenApps();
        })
        .focusable(true)
        .focusOnTouch(true)
      }

      Row() {
        if (this.model().icon === ConnectionRowIcon.Loading) {
          LoadingProgress()
            .width(24)
            .height(24)
            .color(this.actionIconColor())
            .transition(TransitionEffect.IDENTITY)
            .hitTestBehavior(HitTestMode.None)
        } else if (this.model().icon === ConnectionRowIcon.Refresh) {
          Image($r('app.media.ic_refresh'))
            .width(24)
            .height(24)
            .fillColor(this.actionIconColor())
            .draggable(false)
            .transition(fadeTransition())
            .hitTestBehavior(HitTestMode.None)
        } else if (this.model().icon === ConnectionRowIcon.Power) {
          Image($r('app.media.ic_power'))
            .width(24)
            .height(24)
            .fillColor(this.actionIconColor())
            .draggable(false)
            .transition(fadeTransition())
            .hitTestBehavior(HitTestMode.None)
        } else if (this.model().icon === ConnectionRowIcon.Stop) {
          Image($r('app.media.ic_stop'))
            .width(24)
            .height(24)
            .fillColor(this.actionIconColor())
            .draggable(false)
            .transition(fadeTransition())
            .hitTestBehavior(HitTestMode.None)
        } else {
          Image($r('app.media.ic_play'))
            .width(24)
            .height(24)
            .fillColor(this.actionIconColor())
            .draggable(false)
            .transition(fadeTransition())
            .hitTestBehavior(HitTestMode.None)
        }
      }
      .width(44)
      .height(44)
      .backgroundColor(this.actionBg())
      .border({ width: 1, color: this.actionBorder() })
      .borderRadius(22)
      .opacity(this.busy() || this.wakeDisabled() ? 0.5 : 1.0)
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .margin({ left: 8 })
      .hoverEffect(this.busy() || this.wakeDisabled() ? HoverEffect.None : HoverEffect.Highlight)
      .enabled(!this.busy() && !this.wakeDisabled())
      // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
      .onTouch(() => {})
      .bindTips(this.busy() ? '正在断开…'
        : (this.wakeDisabled() ? this.wakeLabel() : this.model().actionTip), this.tipsOptions)
      .onClick(() => {
        this.primaryTap();
      })
      .focusable(!this.busy() && !this.wakeDisabled())
      .focusOnTouch(!this.busy() && !this.wakeDisabled())

      Row()
        .width(0)
        .height(0)
        .bindContextMenu(this.menuShown, this.menu(), this.menuOptions())
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .borderRadius(14)
    .backgroundColor(this.rowHovered
      ? $r('app.color.surface_translucent_hover') : $r('app.color.surface_translucent'))
    .border({ width: 1, color: $r('app.color.border') })
    .shadow({
      radius: this.rowHovered ? 16 : 8,
      color: this.rowHovered ? $r('app.color.shadow_2') : $r('app.color.shadow_1'),
      offsetX: 0,
      offsetY: this.rowHovered ? 4 : 2
    })
    .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
    .onHover((isHover: boolean) => {
      this.rowHovered = isHover;
    })
    .onMouse((event: MouseEvent) => {
      if (event.button === MouseButton.Right && event.action === MouseAction.Press
        && this.menuAllowed()) {
        this.menuWinX = event.windowX;
        this.menuWinY = event.windowY;
        this.onRequestMenu();
      }
    })
    .onTouch((event: TouchEvent) => {
      this.onRowTouch(event);
    })
  }
}


