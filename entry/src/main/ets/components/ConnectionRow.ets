/*
 * HmRdp - saved-connection list row.
 *
 * The card row of the connection list: protocol tile, name (+ status badge),
 * subtitle, an optional Moonlight "apps" button, the connect/disconnect round
 * button, the right-click menu and the CARD material. The caller owns the
 * connection state and the actions; the row only renders them.
 */
import { SavedConnection } from '../model/RdpModels';
import { ConnectionState } from '../services/SessionManager';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { ConnectAction, connectActionFor, connectActionLabel, primaryConnectAction }
  from '../utils/ConnectAction';
import { hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';

@Component
export struct ConnectionRow {
  conn: SavedConnection = new SavedConnection();
  @Prop state: ConnectionState = ConnectionState.Idle;
  // Host-side target the row's connect would act on: `hostTargetId` is what the
  // host currently has open (0 = none / unknown), `targetId` is what this row
  // would open (0 = the connection default, which cannot be resolved here). RDP
  // passes 0/0: it has no host-side session, so it never resumes.
  @Prop hostTargetId: number = 0;
  @Prop targetId: number = 0;
  // The "应用" button's own hover / press state, so it animates like the other
  // round controls (utils/UiMotion) instead of relying on the flat built-in
  // HoverEffect.
  @State appHovered: boolean = false;
  @State appPressed: boolean = false;
  onEdit: () => void = () => {};
  onToggleConnection: () => void = () => {};
  onOpenApps: () => void = () => {};
  onTerminate: () => void = () => {};
  onDelete: () => void = () => {};
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private menuOptions: ContextMenuOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private cardMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.CARD));

  private protocolIcon(): Resource {
    return this.conn.isMoonlight() ? $r('app.media.ic_proto_moonlight')
                                   : $r('app.media.ic_remote_desktop');
  }

  /** Moonlight shows the app it opens; RDP shows the account. */
  private subtitle(): string {
    const target: string = `${this.conn.host}:${this.conn.port}`;
    if (this.conn.isMoonlight()) {
      const appName: string = this.conn.mlDefaultAppName.length > 0 ? this.conn.mlDefaultAppName : '桌面';
      return this.conn.name.length > 0 ? `${target} · ${appName}` : appName;
    }
    if (this.conn.name.length === 0) {
      return this.conn.username;
    }
    return this.conn.username.length > 0 ? `${target} · ${this.conn.username}` : target;
  }

  private action(): ConnectAction {
    return connectActionFor(this.state, this.hostTargetId, this.targetId);
  }

  /**
   * The button's action. Moonlight collapses resume/switch/disconnect into 终止
   * (closing its session stops the host app); RDP keeps 断开连接.
   */
  private primary(): ConnectAction {
    return primaryConnectAction(this.action(), this.conn.isMoonlight());
  }

  private connectTip(): string {
    return connectActionLabel(this.primary());
  }

  /** The button click: 终止 asks the host to stop its app, else toggle the session. */
  private primaryTap(): void {
    if (this.primary() === ConnectAction.Terminate) {
      this.onTerminate();
    } else {
      this.onToggleConnection();
    }
  }

  @Builder
  menu() {
    Menu() {
      if (this.conn.isMoonlight()) {
        // The button never resumes, so 恢复 / 切换 / 连接 are offered here.
        if (this.action() === ConnectAction.Cancel) {
          MenuItem({ content: '取消连接' })
            .onClick(() => {
              this.onToggleConnection();
            })
        } else if (this.action() === ConnectAction.Connect) {
          MenuItem({ content: '连接' })
            .onClick(() => {
              this.onToggleConnection();
            })
        } else if (this.action() === ConnectAction.Resume) {
          MenuItem({ content: '恢复' })
            .onClick(() => {
              this.onToggleConnection();
            })
        } else if (this.action() === ConnectAction.Switch) {
          MenuItem({ content: '切换' })
            .onClick(() => {
              this.onToggleConnection();
            })
        }
        // 终止 needs something running on the host to stop.
        if (this.hostTargetId !== 0) {
          MenuItem({ content: '终止' })
            .contentFontColor($r('app.color.danger'))
            .onClick(() => {
              this.onTerminate();
            })
        }
      } else {
        // RDP keeps its dynamic 连接 / 断开连接 / 取消连接.
        MenuItem({ content: this.connectTip() })
          .onClick(() => {
            this.onToggleConnection();
          })
      }
      MenuItem({ content: '编辑' })
        .onClick(() => {
          this.onEdit();
        })
      MenuItem({ content: '删除' })
        .contentFontColor($r('app.color.danger'))
        .onClick(() => {
          this.onDelete();
        })
    }
  }

  build() {
    Row() {
      Row() {
        Image(this.protocolIcon())
          .width(40)
          .height(40)
          .borderRadius(10)
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
            } else if (this.action() === ConnectAction.Resume) {
              // The host is already running the app this row would open, so the
              // button resumes it (`/resume`). Same badge style as 已连接.
              Text('运行中')
                .fontSize(10)
                .fontColor($r('app.color.success'))
                .backgroundColor($r('app.color.success_bg'))
                .borderRadius(4)
                .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                .margin({ left: 8 })
            }
          }
          if (this.subtitle().length > 0) {
            Text(this.subtitle())
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

      if (this.conn.isMoonlight()) {
        Row() {
          Image($r('app.media.ic_apps'))
            .width(20)
            .height(20)
            .fillColor('#FFFFFF')
            .draggable(false)
        }
        .width(44)
        .height(44)
        .borderRadius(22)
        // Solid neutral (Moonlight's dark-chrome grey): the same filled round
        // style as the connect button, but a third colour so it does not compete
        // with the accent connect nor clash with the red danger state. Hover
        // lifts the surface and the press sinks the button, both via UiMotion.
        .backgroundColor(this.appHovered ? $r('app.color.neutral_hover') : $r('app.color.neutral'))
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
        } else if (this.primary() === ConnectAction.Terminate) {
          // 终止: stop the host's running app and close the session.
          Image($r('app.media.ic_stop'))
            .width(22)
            .height(22)
            .fillColor('#FFFFFF')
            .draggable(false)
        } else if (this.primary() === ConnectAction.Disconnect) {
          // RDP 断开连接 (local session only; no host-side app).
          Image($r('app.media.ic_power'))
            .width(22)
            .height(22)
            .fillColor('#FFFFFF')
            .draggable(false)
        } else {
          Image($r('app.media.ic_play'))
            .width(22)
            .height(22)
            .fillColor('#FFFFFF')
            .draggable(false)
        }
      }
      .width(44)
      .height(44)
      .borderRadius(22)
      .backgroundColor(this.primary() === ConnectAction.Terminate
        || this.primary() === ConnectAction.Disconnect
        ? $r('app.color.danger') : $r('app.color.accent'))
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .margin({ left: 8 })
      .hoverEffect(HoverEffect.Scale)
      .bindTips(this.connectTip(), this.tipsOptions)
      .onClick(() => {
        this.primaryTap();
      })
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .borderRadius(14)
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : $r('app.color.card_bg'))
    .shadow({ radius: 8, color: '#10000000', offsetX: 0, offsetY: 2 })
    .clip(false)
    .attributeModifier(this.cardMaterial)
    .bindContextMenu(this.menu(), ResponseType.RightClick, this.menuOptions)
  }
}
