import { ConnectAction, connectActionLabel, HostPower, RdpConnection, RowMenuAction, RowMenuEntry,
  SavedConnection } from '../model/Connections';
import { ConnectionState } from '../services/SessionManager';
import { FrostedModifier, FrostedSurface, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { WakeReachability } from '../services/WakeOnLan';
import { connectActionFor } from '../utils/ConnectAction';
import { fadeTransition, hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS, popTransition }
  from '../utils/UiMotion';

@Component
export struct ConnectionRow {
  conn: SavedConnection = new RdpConnection();
  @Prop state: ConnectionState = ConnectionState.Idle;
  // `hostTargetId` is the host's open app id (0 = none), `targetId` this
  // connection's default app id (0 = none); RDP passes 0/0, so it never resumes.
  @Prop hostTargetId: number = 0;
  @Prop targetId: number = 0;
  // Host reachability (Moonlight only; RDP stays Online).
  @Prop power: HostPower = HostPower.Online;
  @Prop wakeReachability: WakeReachability = WakeReachability.Unknown;
  @Prop waking: boolean = false;
  @Prop probing: boolean = false;
  // Page-controlled: >0 opens this row's menu, 0 closes it.
  @Prop @Watch('onMenuRequest') menuRequest: number = 0;
  // ArkUI has no shared declarative hover/press effect, so the button keeps its
  // own state and animates via utils/UiMotion.
  @State appHovered: boolean = false;
  @State appPressed: boolean = false;
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
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private cardFrost: AttributeModifier<RowAttribute> =
    new FrostedModifier<RowAttribute>(FrostedSurface.LIST_ROW);

  private action(): ConnectAction {
    return connectActionFor(this.state, this.hostTargetId, this.targetId);
  }

  private primary(): ConnectAction {
    return this.conn.primaryAction(this.action());
  }

  private connectTip(): string {
    return connectActionLabel(this.primary());
  }

  private hostDown(): boolean {
    return this.state === ConnectionState.Idle && this.power !== HostPower.Online;
  }

  private wakeDisabled(): boolean {
    if (!this.hostDown() || this.power !== HostPower.Offline) {
      return false;
    }
    return !this.conn.supportsWake() || this.wakeReachability === WakeReachability.OffLan;
  }

  private actionTip(): string {
    if (this.hostDown()) {
      if (this.waking) {
        return '取消状态检测';
      }
      if (this.probing && this.power === HostPower.Unknown) {
        return '正在检测主机状态';
      }
      if (this.power === HostPower.NoService) {
        return '主机已开机，请检查 Sunshine 是否运行';
      }
      if (this.power === HostPower.Unknown) {
        return '刷新主机状态';
      }
      if (this.wakeReachability === WakeReachability.OffLan) {
        return '当前网络无法到达主机所在局域网，无法唤醒';
      }
      return this.conn.supportsWake() ? '唤醒主机' : '未获取到主机 MAC，无法唤醒';
    }
    return this.connectTip();
  }

  private actionBackground(): Resource {
    if (!this.hostDown()) {
      return this.primary() === ConnectAction.Terminate
        || this.primary() === ConnectAction.Disconnect
        ? $r('app.color.danger') : $r('app.color.accent');
    }
    if (this.wakeDisabled()) {
      return $r('app.color.neutral');
    }
    return this.power === HostPower.Offline
      ? $r('app.color.wake') : $r('app.color.accent');
  }

  private primaryTap(): void {
    if (this.hostDown()) {
      if (this.waking) {
        this.onWake();
      } else if (this.power === HostPower.Unknown || this.power === HostPower.NoService) {
        this.onRefresh();
      } else if (!this.wakeDisabled()) {
        this.onWake();
      }
      return;
    }
    if (this.primary() === ConnectAction.Terminate) {
      this.onTerminate();
    } else {
      this.onToggleConnection();
    }
  }

  // false first re-arms the menu after a dismiss left the flag set.
  private onMenuRequest(): void {
    if (this.menuRequest > 0) {
      this.menuShown = false;
      this.menuShown = true;
    } else {
      this.menuShown = false;
    }
  }

  private menuOptions(): ContextMenuOptions {
    const options: ContextMenuOptions = {
      systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY),
      placement: Placement.BottomRight,
      offset: { x: this.menuWinX, y: this.menuWinY },
      modalMode: ModalMode.TARGET_WINDOW,
      // Must clear the latch, or a re-render reopens the dismissed menu.
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
      this.onPressRelease();
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
    return this.conn.menuEntries(this.action(), this.hostTargetId, this.power)
      .filter((entry: RowMenuEntry) => entry.sessionAction === session);
  }

  @Builder
  menuItem(entry: RowMenuEntry) {
    MenuItem({ content: entry.content })
      .contentFontColor(entry.action === RowMenuAction.Delete
        ? $r('app.color.danger') : $r('app.color.text_primary'))
      .onClick(() => {
        this.onMenuEntry(entry);
      })
  }

  @Builder
  menu() {
    Menu() {
      // Two groups draw the separator between the session actions and 编辑 / 删除.
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
          .borderRadius(10)
          .opacity(this.hostDown() ? 0.45 : 1.0)
          .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
          .draggable(false)
        Column() {
          Row() {
            Text(this.conn.label())
              .fontSize(15)
              .fontColor($r('app.color.text_primary'))
            if (this.state === ConnectionState.Connected) {
              Text('已连接')
                .fontSize(10)
                .fontColor($r('app.color.success'))
                .backgroundColor($r('app.color.success_bg'))
                .borderRadius(4)
                .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                .margin({ left: 8 })
                .transition(fadeTransition())
            } else if (this.action() === ConnectAction.Resume) {
              // The host is already running the app this row would open.
              Text('运行中')
                .fontSize(10)
                .fontColor($r('app.color.accent'))
                .backgroundColor($r('app.color.accent_bg'))
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
      }
      .layoutWeight(1)
      .bindTips('编辑连接', this.tipsOptions)
      .onClick(() => {
        this.onEdit();
      })

      if (this.conn.showsAppsButton() && !this.hostDown()) {
        Row() {
          Image($r('app.media.ic_apps'))
            .width(24)
            .height(24)
            .fillColor('#FFFFFF')
            .draggable(false)
        }
        .width(44)
        .height(44)
        .borderRadius(22)
        .backgroundColor(this.appHovered ? $r('app.color.neutral_hover') : $r('app.color.neutral'))
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .margin({ left: 8 })
        .scale(hoverPressScale(this.appHovered, this.appPressed))
        .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
        .transition(popTransition())
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
        .bindTips('应用', this.tipsOptions)
        .onClick(() => {
          this.onOpenApps();
        })
      }

      Row() {
        if (this.state === ConnectionState.Connecting) {
          LoadingProgress()
            .width(24)
            .height(24)
            .color('#FFFFFF')
            .transition(TransitionEffect.IDENTITY)
        } else if (this.hostDown() && this.waking) {
          LoadingProgress()
            .width(24)
            .height(24)
            .color('#FFFFFF')
            .transition(TransitionEffect.IDENTITY)
        } else if (this.hostDown() && this.probing
          && (this.power === HostPower.Unknown || this.power === HostPower.NoService)) {
          LoadingProgress()
            .width(24)
            .height(24)
            .color('#FFFFFF')
            .transition(TransitionEffect.IDENTITY)
        } else if (this.hostDown()
          && (this.power === HostPower.Unknown || this.power === HostPower.NoService)) {
          Image($r('app.media.ic_refresh'))
            .width(24)
            .height(24)
            .fillColor('#FFFFFF')
            .draggable(false)
            .transition(fadeTransition())
        } else if (this.hostDown()) {
          Image($r('app.media.ic_power'))
            .width(28)
            .height(28)
            .fillColor('#FFFFFF')
            .draggable(false)
            .transition(fadeTransition())
        } else if (this.primary() === ConnectAction.Terminate
          || this.primary() === ConnectAction.Disconnect) {
          Image($r('app.media.ic_stop'))
            .width(22)
            .height(22)
            .fillColor('#FFFFFF')
            .draggable(false)
            .transition(fadeTransition())
        } else {
          Image($r('app.media.ic_play'))
            .width(22)
            .height(22)
            .fillColor('#FFFFFF')
            .draggable(false)
            .transition(fadeTransition())
        }
      }
      .width(44)
      .height(44)
      .borderRadius(22)
      .backgroundColor(this.actionBackground())
      .opacity(this.wakeDisabled() ? 0.5 : 1.0)
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .margin({ left: 8 })
      .hoverEffect(this.wakeDisabled() ? HoverEffect.None : HoverEffect.Scale)
      .bindTips(this.actionTip(), this.tipsOptions)
      .onClick(() => {
        this.primaryTap();
      })

      // The isShown menu ignores this node's position; the point comes from `offset`.
      Row()
        .width(0)
        .height(0)
        .bindContextMenu(this.menuShown, this.menu(), this.menuOptions())
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .borderRadius(14)
    .shadow({ radius: 8, color: '#10000000', offsetX: 0, offsetY: 2 })
    .clip(false)
    .attributeModifier(this.cardFrost)
    .onMouse((event: MouseEvent) => {
      if (event.button === MouseButton.Right && event.action === MouseAction.Press) {
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
