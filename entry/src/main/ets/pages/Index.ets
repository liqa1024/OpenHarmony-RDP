import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ConnectOrigin, EditConnectionParams, SavedConnection } from '../model/Connections';
import { ConnectionLauncher } from '../services/ConnectionLauncher';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState, HostTargetListener }
  from '../services/MoonlightHostState';
import { MoonlightAppsParams } from './MoonlightAppsPage';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { SettingsStore } from '../services/SettingsStore';
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { ConnectionRow } from '../components/ConnectionRow';
import { FabButton } from '../components/FabButton';
import { showConfirm, showNotice, showToast } from '../utils/UiUtil';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpIndex';

@Entry
@Component
struct Index {
  @State connections: SavedConnection[] = [];
  @State statuses: Map<string, ConnectionState> = new Map<string, ConnectionState>();
  @State hostTargets: Map<string, number> = new Map<string, number>();
  @State draggingIndex: number = -1;
  // Which row's menu is open; menuSeq changes so a repeat request still re-opens it.
  @State menuConnectionId: string = '';
  @State menuSeq: number = 0;
  private armedMenuId: string = '';
  @State settingsHovered: boolean = false;
  @State settingsPressed: boolean = false;
  @State developerMode: boolean = false;

  // Older systems simply ignore the material in the tips options.
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };

  private sessionListener: SessionStateListener = (connectionId: string,
    state: ConnectionState, message: string, origin: ConnectOrigin): void => {
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
    if (state === ConnectionState.Failed && origin === ConnectOrigin.List) {
      this.showConnectError(message);
    }
  };

  private hostStateListener: HostTargetListener =
    (connectionId: string, currentGame: number): void => {
      const next: Map<string, number> = new Map<string, number>(this.hostTargets);
      if (currentGame === 0) {
        next.delete(connectionId);
      } else {
        next.set(connectionId, currentGame);
      }
      this.hostTargets = next;
    };

  aboutToAppear(): void {
    SessionManager.subscribe(this.sessionListener);
    MoonlightHostState.subscribe(this.hostStateListener);
    this.refresh();
  }

  aboutToDisappear(): void {
    SessionManager.unsubscribe(this.sessionListener);
    MoonlightHostState.unsubscribe(this.hostStateListener);
  }

  onPageShow(): void {
    this.refresh();
  }

  private refresh(): void {
    this.connections = ConnectionStore.list();
    this.developerMode = SettingsStore.load().developerMode;
    this.syncStatuses();
    this.syncHostTargets();
    this.probeHosts();
  }

  private syncStatuses(): void {
    const next: Map<string, ConnectionState> = new Map<string, ConnectionState>();
    for (const conn of this.connections) {
      const state: ConnectionState = SessionManager.stateOf(conn.id);
      if (state !== ConnectionState.Idle) {
        next.set(conn.id, state);
      }
    }
    this.statuses = next;
  }

  private syncHostTargets(): void {
    const next: Map<string, number> = new Map<string, number>();
    for (const conn of this.connections) {
      const target: number = MoonlightHostState.targetOf(conn.id);
      if (target !== 0) {
        next.set(conn.id, target);
      }
    }
    this.hostTargets = next;
  }

  private filesDir(): string {
    const context = this.getUIContext().getHostContext();
    return context === undefined ? '' : (context as common.UIAbilityContext).filesDir;
  }

  private probeHosts(): void {
    MoonlightHostState.refreshAll(this.connections, this.filesDir());
  }

  private refreshHostState(conn: SavedConnection): void {
    MoonlightHostState.refresh(conn, this.filesDir());
  }

  private terminateHostApp(conn: SavedConnection): void {
    const filesDir: string = this.filesDir();
    if (filesDir.length === 0) {
      return;
    }
    if (!MoonlightHostState.reachableOf(conn.id)) {
      showToast(this.getUIContext(), '无法终止主机应用：主机离线');
      return;
    }
    const httpsPort: number = MoonlightHostState.httpsPortOf(conn.id);
    MoonlightClient.cancel(filesDir, conn.host, httpsPort)
      .then((ok: boolean): void => {
        showToast(this.getUIContext(), ok ? '已请求终止主机应用' : '无法终止主机应用：主机离线');
        if (ok) {
          MoonlightHostState.markStopped(conn.id);
          MoonlightHostState.refreshAfter(conn, filesDir, HOST_REPROBE_DELAY_MS);
        } else {
          MoonlightHostState.refresh(conn, filesDir);
        }
        if (this.connState(conn.id) !== ConnectionState.Idle) {
          SessionManager.disconnect(conn.id);
        }
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'terminate host app failed: %{public}s', JSON.stringify(error));
        MoonlightHostState.refresh(conn, filesDir);
      });
  }

  private hostTargetId(conn: SavedConnection): number {
    return conn.hostTargetId(this.hostTargets);
  }

  private targetId(conn: SavedConnection): number {
    return conn.defaultTargetId();
  }

  private openRowMenu(connectionId: string): void {
    this.menuConnectionId = connectionId;
    this.menuSeq = this.menuSeq + 1;
  }

  private closeRowMenu(): void {
    this.menuConnectionId = '';
  }

  private armRowMenu(index: number): void {
    const row: SavedConnection | undefined = this.connections[index];
    this.armedMenuId = row === undefined ? '' : row.id;
  }

  private releaseRowMenu(): void {
    if (this.armedMenuId.length > 0) {
      this.openRowMenu(this.armedMenuId);
      this.armedMenuId = '';
    }
  }

  private menuRequestFor(conn: SavedConnection): number {
    return this.menuConnectionId === conn.id ? this.menuSeq : 0;
  }

  private persistOrder(): void {
    const ids: string[] = [];
    for (const conn of this.connections) {
      ids.push(conn.id);
    }
    ConnectionStore.reorder(ids);
  }

  // Mutated in place so the settling animation stays continuous.
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

  private connect(conn: SavedConnection): void {
    this.startConnection(conn);
  }

  private toggleConnection(conn: SavedConnection): void {
    const state: ConnectionState = this.connState(conn.id);
    if (state === ConnectionState.Connecting || state === ConnectionState.Connected) {
      SessionManager.disconnect(conn.id);
      return;
    }
    if (state === ConnectionState.Disconnecting) {
      return;
    }
    this.connect(conn);
  }

  private showConnectError(message: string): void {
    showNotice(this.getUIContext(), '连接失败', message);
  }

  private startConnection(conn: SavedConnection): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    ConnectionLauncher.start(this.getUIContext(), context as common.UIAbilityContext, conn,
      ConnectOrigin.List);
  }

  private openMoonlightApps(conn: SavedConnection): void {
    const params: MoonlightAppsParams = { connectionId: conn.id };
    this.getUIContext().getRouter()
      .pushUrl({ url: 'pages/MoonlightAppsPage', params: params })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open moonlight apps failed: %{public}s', JSON.stringify(error));
      });
  }

  private openEditor(id: string, protocol: number): void {
    const params: EditConnectionParams = { id: id, protocol: protocol };
    this.getUIContext().getRouter()
      .pushUrl({ url: 'pages/EditConnectionPage', params: params })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open editor failed: %{public}s', JSON.stringify(error));
      });
  }

  private openNewConnection(): void {
    this.openEditor('', SettingsStore.lastNewProtocol());
  }

  private openSettings(): void {
    this.getUIContext().getRouter().pushUrl({ url: 'pages/SettingsPage' })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'open settings failed: %{public}s', JSON.stringify(error));
      });
  }

  private remove(conn: SavedConnection): void {
    ConnectionStore.remove(conn.id);
    const hostContext = this.getUIContext().getHostContext();
    if (hostContext !== undefined) {
      // Pairing is per host: dropping it here keeps a later re-add from skipping the PIN round.
      ConnectionLauncher.afterDelete(conn, (hostContext as common.UIAbilityContext).filesDir);
    }
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
    showConfirm(this.getUIContext(), {
      title: '删除连接',
      message: `将删除「${conn.label()}」，该操作无法撤销。是否继续？`,
      confirmLabel: '删除',
      danger: true,
      onConfirm: () => {
        this.remove(conn);
      }
    });
  }

  build() {
    Stack({ alignContent: Alignment.BottomEnd }) {
      Column() {
      Row() {
        Image($r('app.media.app_icon'))
          .width(44)
          .height(44)
          .borderRadius(11)
          .draggable(false)
        Column() {
          Text($r('app.string.app_name'))
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

        if (this.developerMode) {
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
        }

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
            .draggable(false)
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
        .bindTips('设置', this.tipsOptions)
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
            .draggable(false)
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
              this.openNewConnection();
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
              ConnectionRow({
                conn: conn,
                state: this.connState(conn.id),
                hostTargetId: this.hostTargetId(conn),
                targetId: this.targetId(conn),
                menuRequest: this.menuRequestFor(conn),
                onRequestMenu: () => {
                  this.openRowMenu(conn.id);
                },
                onPressRelease: () => {
                  this.releaseRowMenu();
                },
                onEdit: () => {
                  this.openEditor(conn.id, conn.protocol);
                },
                onToggleConnection: () => {
                  this.toggleConnection(conn);
                },
                onOpenApps: () => {
                  this.openMoonlightApps(conn);
                },
                onTerminate: () => {
                  this.terminateHostApp(conn);
                },
                onRefresh: () => {
                  this.refreshHostState(conn);
                },
                onDelete: () => {
                  this.confirmDelete(conn);
                }
              })
            }
            .margin({ left: 64, right: 88, top: 2, bottom: 2 })
            // The scaled drag preview would be covered by its siblings without this.
            .zIndex(index === this.draggingIndex ? 1 : 0)
          }, (conn: SavedConnection) => `${conn.id}#${conn.updatedAt}`)
            .onMove((from: number, to: number) => {
              this.moveConnection(from, to);
            }, {
              onLongPress: (index: number) => {
                this.armRowMenu(index);
              },
              onDragStart: (index: number) => {
                this.armedMenuId = '';
                this.draggingIndex = index;
                this.closeRowMenu();
              },
              onDrop: (index: number) => {
                this.draggingIndex = -1;
              }
            })
        }
        .layoutWeight(1)
        .width('100%')
        // Full-width so the clip area stays wide; the side insets live on the ListItem margins.
        .padding({ left: 0, right: 0, top: 12, bottom: 20 })
        .scrollBar(BarState.Auto)
        // Fallback release point, and a fresh press drops any stale arm.
        .onTouch((event: TouchEvent) => {
          if (event.type === TouchType.Down) {
            this.armedMenuId = '';
          } else if (event.type === TouchType.Up) {
            this.releaseRowMenu();
          }
        })
      }
      }
      .width('100%')
      .height('100%')

      FabButton({
        icon: $r('app.media.ic_plus'),
        tip: '新建连接',
        onTap: () => {
          this.openNewConnection();
        }
      })
        .margin({ right: 24, bottom: 24 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
