/*
 * HmRdp - saved connections list (home page).
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AdvancedProfile, DisplayProfile, EditConnectionParams, PendingConnection,
  RdpConnectOptions, RdpConnectOrigin, SavedConnection } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { DeviceCapabilities } from '../services/DeviceCapabilities';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { SettingsStore } from '../services/SettingsStore';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpIndex';

@Entry
@Component
struct Index {
  @State connections: SavedConnection[] = [];
  @State statuses: Map<string, ConnectionState> = new Map<string, ConnectionState>();
  // Index of the item being drag-reordered; -1 when idle. Used only to lift the
  // dragged item above its siblings.
  @State draggingIndex: number = -1;
  @State settingsHovered: boolean = false;
  @State settingsPressed: boolean = false;
  @State fabHovered: boolean = false;
  @State fabPressed: boolean = false;

  private sessionListener: SessionStateListener = (connectionId: string,
    state: ConnectionState, message: string, origin: RdpConnectOrigin): void => {
    const next: Map<string, ConnectionState> = new Map<string, ConnectionState>();
    this.statuses.forEach((value: ConnectionState, key: string) => {
      next.set(key, value);
    });
    if (state === ConnectionState.Idle) {
      next.delete(connectionId);
    } else {
      next.set(connectionId, state);
    }
    this.statuses = next;
    // Editor-originated failures are reported on the edit page instead.
    if (state === ConnectionState.Failed && origin === RdpConnectOrigin.List) {
      this.showConnectError(message);
    }
  };

  aboutToAppear(): void {
    SessionManager.subscribe(this.sessionListener);
    this.refresh();
  }

  aboutToDisappear(): void {
    SessionManager.unsubscribe(this.sessionListener);
  }

  onPageShow(): void {
    this.refresh();
  }

  private refresh(): void {
    this.connections = ConnectionStore.list();
  }

  private persistOrder(): void {
    const ids: string[] = [];
    for (const conn of this.connections) {
      ids.push(conn.id);
    }
    ConnectionStore.reorder(ids);
  }

  /**
   * Applies a drag reorder from the list's onMove callback. The array is mutated
   * in place so the settling animation stays continuous, then persisted.
   */
  private moveConnection(from: number, to: number): void {
    if (from === to || from < 0 || to < 0
      || from >= this.connections.length || to >= this.connections.length) {
      return;
    }
    const moved: SavedConnection[] = this.connections.splice(from, 1);
    if (moved.length === 0) {
      return;
    }
    this.connections.splice(to, 0, moved[0]);
    this.persistOrder();
  }

  private connState(id: string): ConnectionState {
    const state: ConnectionState | undefined = this.statuses.get(id);
    return state === undefined ? ConnectionState.Idle : state;
  }

  /** Hover hint for the row's round action button. */
  private connectTip(conn: SavedConnection): string {
    const state: ConnectionState = this.connState(conn.id);
    if (state === ConnectionState.Connecting) {
      return '取消连接';
    }
    if (state === ConnectionState.Connected) {
      return '断开连接';
    }
    return '连接';
  }

  private connect(conn: SavedConnection): void {
    this.startConnection(conn);
  }

  private toggleConnection(conn: SavedConnection): void {
    const state: ConnectionState = this.connState(conn.id);
    if (state === ConnectionState.Connecting || state === ConnectionState.Connected) {
      SessionManager.disconnect(conn.id);
      return;
    }
    this.connect(conn);
  }

  private showConnectError(message: string): void {
    this.getUIContext().showAlertDialog({
      title: '连接失败',
      message: message,
      confirm: {
        value: '知道了',
        action: () => {
        }
      }
    });
  }

  private async startConnection(conn: SavedConnection): Promise<void> {
    const options: RdpConnectOptions = conn.toOptions();
    const profile: DisplayProfile = SettingsStore.resolveDisplay(conn);
    options.width = profile.width;
    options.height = profile.height;
    options.scalePercent = profile.scalePercent;
    const advanced: AdvancedProfile = SettingsStore.resolveAdvanced(conn);
    options.enableClipboard = advanced.enableClipboard;
    options.enableAudio = advanced.enableAudio && DeviceCapabilities.audio().supported;
    options.enableGfx = advanced.enableGfx;
    options.enableH264 = advanced.enableH264;
    options.ignoreCertificate = advanced.ignoreCertificate;
    const password: string = await CredentialStore.loadPassword(conn.id);
    options.password = password;
    const request: PendingConnection = new PendingConnection();
    request.options = options;
    request.connectionId = conn.id;
    request.origin = RdpConnectOrigin.List;
    request.onConnected = undefined;
    const context = this.getUIContext().getHostContext();
    if (context !== undefined) {
      SessionManager.connect(request, context as common.UIAbilityContext);
    }
  }

  private openEditor(id: string): void {
    const params: EditConnectionParams = { id: id };
    this.getUIContext().getRouter()
      .pushUrl({ url: 'pages/EditConnectionPage', params: params })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open editor failed: %{public}s', JSON.stringify(error));
      });
  }

  private openSettings(): void {
    this.getUIContext().getRouter().pushUrl({ url: 'pages/SettingsPage' })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open settings failed: %{public}s', JSON.stringify(error));
      });
  }

  private remove(conn: SavedConnection): void {
    ConnectionStore.remove(conn.id);
    CredentialStore.removePassword(conn.id)
      .then(() => {
        this.refresh();
      })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'remove failed: %{public}s', JSON.stringify(error));
        this.refresh();
      });
  }

  private confirmDelete(conn: SavedConnection): void {
    this.getUIContext().showAlertDialog({
      title: '删除连接',
      message: `确定要删除「${conn.label()}」？该操作无法撤销。`,
      primaryButton: {
        value: '取消',
        action: () => {
        }
      },
      secondaryButton: {
        value: '删除',
        fontColor: $r('app.color.danger'),
        action: () => {
          this.remove(conn);
        }
      }
    });
  }

  @Builder
  connectionMenu(conn: SavedConnection) {
    Menu() {
      MenuItem({ content: '连接' })
        .onClick(() => {
          this.connect(conn);
        })
      MenuItem({ content: '编辑' })
        .onClick(() => {
          this.openEditor(conn.id);
        })
      MenuItem({ content: '删除' })
        .contentFontColor($r('app.color.danger'))
        .onClick(() => {
          this.confirmDelete(conn);
        })
    }
  }

  @Builder
  connectionRow(conn: SavedConnection) {
    Row() {
      Row() {
        Image($r('app.media.app_icon'))
          .width(40)
          .height(40)
          .borderRadius(10)
        Column() {
          Row() {
            Text(conn.label())
              .fontSize(15)
              .fontColor($r('app.color.text_primary'))
            if (this.connState(conn.id) === ConnectionState.Connected) {
              Text('已连接')
                .fontSize(10)
                .fontColor($r('app.color.success'))
                .backgroundColor($r('app.color.success_bg'))
                .borderRadius(4)
                .padding({ left: 6, right: 6, top: 2, bottom: 2 })
                .margin({ left: 8 })
            }
          }
          Text(conn.username.length > 0 ? conn.username : '未设置用户名')
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 3 })
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)
        .margin({ left: 12 })
      }
      .layoutWeight(1)
      .bindTips('编辑连接')
      .onClick(() => {
        this.openEditor(conn.id);
      })

      Row() {
        if (this.connState(conn.id) === ConnectionState.Connecting) {
          LoadingProgress()
            .width(24)
            .height(24)
            .color('#FFFFFF')
        } else if (this.connState(conn.id) === ConnectionState.Connected) {
          Image($r('app.media.ic_disconnect'))
            .width(22)
            .height(22)
        } else {
          Image($r('app.media.ic_arrow_right'))
            .width(20)
            .height(20)
        }
      }
      .width(44)
      .height(44)
      .borderRadius(22)
      .backgroundColor(this.connState(conn.id) === ConnectionState.Connected
        ? $r('app.color.danger') : $r('app.color.accent'))
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .margin({ left: 8 })
      .hoverEffect(HoverEffect.Scale)
      .bindTips(this.connectTip(conn))
      .onClick(() => {
        this.toggleConnection(conn);
      })
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 10, bottom: 10 })
    .borderRadius(14)
    .backgroundColor($r('app.color.card_bg'))
    .shadow({ radius: 8, color: '#10000000', offsetX: 0, offsetY: 2 })
    .clip(false)
    .bindContextMenu(this.connectionMenu(conn), ResponseType.RightClick)
  }

  build() {
    Stack({ alignContent: Alignment.BottomEnd }) {
      Column() {
      Row() {
        Image($r('app.media.app_icon'))
          .width(44)
          .height(44)
          .borderRadius(11)
        Column() {
          Text('RDP 远程桌面')
            .fontSize(20)
            .fontWeight(FontWeight.Bold)
            .fontColor($r('app.color.text_primary'))
          Text('远程控制 Windows 桌面')
            .fontSize(12)
            .fontColor($r('app.color.text_secondary'))
            .margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        .margin({ left: 12 })

        Blank()

        // Dev-only entry (doc_agent/gfx-engine.md §6): recorded RDP replay page.
        Text('回放测试')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
          .padding({ left: 10, right: 10, top: 6, bottom: 6 })
          .borderRadius(9)
          .backgroundColor($r('app.color.hover_bg'))
          .margin({ right: 8 })
          .onClick(() => {
            this.getUIContext().getRouter().pushUrl({ url: 'pages/GfxReplayPage' });
          })

        Stack() {
          Row()
            .width(44)
            .height(44)
            .borderRadius(22)
            .backgroundColor($r('app.color.hover_bg'))
            .opacity(this.settingsHovered ? 1.0 : 0.0)
            .animation({ duration: 150, curve: Curve.FastOutSlowIn })

          Image($r('app.media.ic_settings'))
            .width(24)
            .height(24)
            .opacity(this.settingsPressed ? 0.55 : 1.0)
            .scale({
              x: this.settingsPressed ? 0.84 : 1.0,
              y: this.settingsPressed ? 0.84 : 1.0
            })
            .animation({ duration: 150, curve: Curve.FastOutSlowIn })
        }
        .width(44)
        .height(44)
        .onHover((isHover: boolean) => {
          this.settingsHovered = isHover;
        })
        .onMouse((event: MouseEvent) => {
          if (event.action === MouseAction.Press) {
            this.settingsPressed = true;
          } else if (event.action === MouseAction.Release) {
            this.settingsPressed = false;
          }
        })
        .bindTips('设置')
        .onClick(() => {
          this.openSettings();
        })
      }
      .width('100%')
      .padding({ left: 24, right: 24, top: 20, bottom: 16 })

      Divider().color($r('app.color.divider'))

      if (this.connections.length === 0) {
        Column() {
          Image($r('app.media.app_icon'))
            .width(72)
            .height(72)
            .borderRadius(18)
            .opacity(0.35)
          Text('还没有保存的连接')
            .fontSize(15)
            .fontColor($r('app.color.text_secondary'))
            .margin({ top: 18 })
          Text('点击右上角 + 添加一个远程桌面')
            .fontSize(13)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 6 })
          Button('添加连接')
            .height(44)
            .fontSize(15)
            .backgroundColor($r('app.color.accent'))
            .borderRadius(12)
            .margin({ top: 22 })
            .onClick(() => {
              this.openEditor('');
            })
        }
        .layoutWeight(1)
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
      } else {
        List({ space: 6 }) {
          ForEach(this.connections, (conn: SavedConnection, index: number) => {
            ListItem() {
              this.connectionRow(conn)
            }
            .margin({ left: 52, right: 52, top: 2, bottom: 2 })
            // Near the drag preview's scale, a higher z-index keeps the lifted
            // card above its neighbours instead of being covered at the sides.
            .zIndex(index === this.draggingIndex ? 1 : 0)
          }, (conn: SavedConnection) => `${conn.id}#${conn.updatedAt}`)
            .onMove((from: number, to: number) => {
              this.moveConnection(from, to);
            }, {
              onDragStart: (index: number) => {
                this.draggingIndex = index;
              },
              onDrop: (index: number) => {
                this.draggingIndex = -1;
              }
            })
        }
        .layoutWeight(1)
        .width('100%')
        // Keep the list full-width so its clip area is as wide as possible; the
        // card's side insets come from the ListItem margins instead. The list
        // still clips vertically, which hints the drag position at the edges.
        .padding({ left: 0, right: 0, top: 12, bottom: 20 })
        .scrollBar(BarState.Auto)
      }
      }
      .width('100%')
      .height('100%')

      Row() {
        Image($r('app.media.ic_plus'))
          .width(24)
          .height(24)
      }
      .width(56)
      .height(56)
      .borderRadius(28)
      .backgroundColor($r('app.color.accent'))
      .justifyContent(FlexAlign.Center)
      .alignItems(VerticalAlign.Center)
      .scale({
        x: this.fabPressed ? 0.9 : (this.fabHovered ? 1.06 : 1.0),
        y: this.fabPressed ? 0.9 : (this.fabHovered ? 1.06 : 1.0)
      })
      .shadow({ radius: 18, color: '#33000000', offsetX: 0, offsetY: 6 })
      .animation({ duration: 140, curve: Curve.FastOutSlowIn })
      .margin({ right: 24, bottom: 24 })
      .onHover((isHover: boolean) => {
        this.fabHovered = isHover;
      })
      .onMouse((event: MouseEvent) => {
        if (event.action === MouseAction.Press) {
          this.fabPressed = true;
        } else if (event.action === MouseAction.Release) {
          this.fabPressed = false;
        }
      })
      .bindTips('新建连接')
      .onClick(() => {
        this.openEditor('');
      })
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
