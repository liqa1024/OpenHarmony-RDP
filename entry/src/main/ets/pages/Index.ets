import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ConnectOrigin, EditConnectionParams, HostPower, MoonlightConnection,
  SavedConnection } from '../model/Connections';
import { ConnectionLauncher } from '../services/ConnectionLauncher';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState, HostTargetListener }
  from '../services/MoonlightHostState';
import { MoonlightAppsParams } from './MoonlightAppsPage';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { BackgroundImage } from '../services/BackgroundImage';
import { SettingsStore } from '../services/SettingsStore';
import { WakeOnLan, WakeReachability, WakeSendResult } from '../services/WakeOnLan';
import { ConnectionRow } from '../components/ConnectionRow';
import { ControlDisc } from '../components/ControlDisc';
import { FabButton } from '../components/FabButton';
import { TextAction } from '../components/TextAction';
import { PageBackground } from '../components/PageBackground';
import { PageHeader, PageHeaderVariant } from '../components/PageHeader';
import { StateView, StateViewKind } from '../components/StateView';
import { showConfirm, showNotice, showToast } from '../utils/UiUtil';
import { promptWolHelp } from '../utils/WolHelpPrompt';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpIndex';
const WAKE_POLL_INTERVAL_MS: number = 2000;
const WAKE_WATCH_TIMEOUT_MS: number = 120000;

@Entry
@Component
struct Index {
  @State connections: SavedConnection[] = [];
  @State statuses: Map<string, ConnectionState> = new Map<string, ConnectionState>();
  @State hostTargets: Map<string, number> = new Map<string, number>();
  @State hostPowers: Map<string, HostPower> = new Map<string, HostPower>();
  @State hostWake: Map<string, WakeReachability> = new Map<string, WakeReachability>();
  @State wakingHosts: Map<string, boolean> = new Map<string, boolean>();
  @State probingHosts: Map<string, boolean> = new Map<string, boolean>();
  private wakeTimers: Map<string, number> = new Map<string, number>();
  @State draggingIndex: number = -1;
  @State menuConnectionId: string = '';
  @State menuSeq: number = 0;
  private armedMenuId: string = '';
  @State developerMode: boolean = false;
  @State backgroundUri: string = '';

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
    if (state === ConnectionState.Failed && origin === ConnectOrigin.List) {
      this.showConnectError(message);
    }
  };

  private hostStateListener: HostTargetListener =
    (connectionId: string, currentGame: number, power: HostPower, probing: boolean): void => {
      const next: Map<string, number> = new Map<string, number>(this.hostTargets);
      if (currentGame === 0) {
        next.delete(connectionId);
      } else {
        next.set(connectionId, currentGame);
      }
      this.hostTargets = next;
      const nextPowers: Map<string, HostPower> = new Map<string, HostPower>(this.hostPowers);
      nextPowers.set(connectionId, power);
      this.hostPowers = nextPowers;
      const reach: WakeReachability = MoonlightHostState.wakeReachabilityOf(connectionId);
      const nextWake: Map<string, WakeReachability> = new Map<string, WakeReachability>(this.hostWake);
      nextWake.set(connectionId, reach);
      this.hostWake = nextWake;
      this.setProbing(connectionId, probing);
      if (this.wakingHosts.get(connectionId) === true) {
        if (power === HostPower.Online) {
          this.stopWakeWatch(connectionId);
          showToast(this.getUIContext(), '主机已上线');
        } else if (power === HostPower.NoService) {
          this.stopWakeWatch(connectionId);
          showToast(this.getUIContext(), '主机已开机，Sunshine 未运行');
        } else if (reach === WakeReachability.OffLan) {
          this.stopWakeWatch(connectionId);
          showToast(this.getUIContext(), '网络无法到达主机所在局域网');
        }
      }
    };

  aboutToAppear(): void {
    SessionManager.subscribe(this.sessionListener);
    MoonlightHostState.subscribe(this.hostStateListener);
    this.refresh();
  }

  aboutToDisappear(): void {
    SessionManager.unsubscribe(this.sessionListener);
    MoonlightHostState.unsubscribe(this.hostStateListener);
    this.clearWakeWatches();
  }

  onPageShow(): void {
    this.refresh();
  }

  private refresh(): void {
    this.connections = ConnectionStore.list();
    const settings = SettingsStore.load();
    this.developerMode = settings.developerMode;
    this.backgroundUri = BackgroundImage.uriOf(settings.backgroundImage);
    this.syncStatuses();
    this.syncHostTargets();
    this.syncHostPowers();
    this.syncHostWake();
    this.syncHostProbing();
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

  private syncHostPowers(): void {
    const next: Map<string, HostPower> = new Map<string, HostPower>();
    for (const conn of this.connections) {
      if (conn instanceof MoonlightConnection) {
        next.set(conn.id, MoonlightHostState.powerOf(conn.id));
      }
    }
    this.hostPowers = next;
  }

  private powerFor(conn: SavedConnection): HostPower {
    if (!(conn instanceof MoonlightConnection)) {
      return HostPower.Online;
    }
    const power: HostPower | undefined = this.hostPowers.get(conn.id);
    return power === undefined ? HostPower.Unknown : power;
  }

  private syncHostWake(): void {
    const next: Map<string, WakeReachability> = new Map<string, WakeReachability>();
    for (const conn of this.connections) {
      if (conn instanceof MoonlightConnection) {
        next.set(conn.id, MoonlightHostState.wakeReachabilityOf(conn.id));
      }
    }
    this.hostWake = next;
  }

  private wakeReachabilityFor(conn: SavedConnection): WakeReachability {
    if (!(conn instanceof MoonlightConnection)) {
      return WakeReachability.Unknown;
    }
    const value: WakeReachability | undefined = this.hostWake.get(conn.id);
    return value === undefined ? WakeReachability.Unknown : value;
  }

  private syncHostProbing(): void {
    const next: Map<string, boolean> = new Map<string, boolean>();
    for (const conn of this.connections) {
      if (conn instanceof MoonlightConnection && MoonlightHostState.probingOf(conn.id)) {
        next.set(conn.id, true);
      }
    }
    this.probingHosts = next;
  }

  private probingFor(conn: SavedConnection): boolean {
    return this.probingHosts.get(conn.id) === true;
  }

  private setProbing(connectionId: string, probing: boolean): void {
    if ((this.probingHosts.get(connectionId) === true) === probing) {
      return;
    }
    const next: Map<string, boolean> = new Map<string, boolean>(this.probingHosts);
    if (probing) {
      next.set(connectionId, true);
    } else {
      next.delete(connectionId);
    }
    this.probingHosts = next;
  }

  private wakingFor(conn: SavedConnection): boolean {
    return this.wakingHosts.get(conn.id) === true;
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

  private toggleWake(conn: SavedConnection): void {
    if (this.wakingHosts.get(conn.id) === true) {
      this.stopWakeWatch(conn.id);
      showToast(this.getUIContext(), '已取消状态检测');
      return;
    }
    if (!(conn instanceof MoonlightConnection) || !conn.supportsWake()) {
      showToast(this.getUIContext(), '未获取到主机 MAC，无法唤醒');
      return;
    }
    if (this.wakeReachabilityFor(conn) === WakeReachability.OffLan) {
      showToast(this.getUIContext(), '当前网络无法到达主机所在局域网');
      return;
    }
    const target: MoonlightConnection = conn;
    if (SettingsStore.load().wolHelpShown) {
      this.sendWake(target);
      return;
    }
    promptWolHelp(this.getUIContext()).then((): void => {
      SettingsStore.rememberWolHelpShown();
      this.sendWake(target);
    });
  }

  private sendWake(conn: MoonlightConnection): void {
    const filesDir: string = this.filesDir();
    WakeOnLan.wake(conn.mlMac, conn.host)
      .then((result: WakeSendResult): void => {
        if (!result.sent) {
          showToast(this.getUIContext(), '唤醒包发送失败');
          return;
        }
        showToast(this.getUIContext(),
          result.bound ? '尝试唤醒主机' : '未找到主机所在网段，唤醒包可能无法到达');
        this.startWakeWatch(conn, filesDir);
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'wake failed: %{public}s', JSON.stringify(error));
        showToast(this.getUIContext(), '唤醒包发送失败');
      });
  }

  private startWakeWatch(conn: SavedConnection, filesDir: string): void {
    this.stopWakeWatch(conn.id);
    let elapsed: number = 0;
    const timer: number = setInterval((): void => {
      elapsed += WAKE_POLL_INTERVAL_MS;
      if (elapsed >= WAKE_WATCH_TIMEOUT_MS) {
        this.stopWakeWatch(conn.id);
        showToast(this.getUIContext(), '未检测到主机上线');
        return;
      }
      MoonlightHostState.refresh(conn, filesDir);
    }, WAKE_POLL_INTERVAL_MS);
    this.wakeTimers.set(conn.id, timer);
    this.setWaking(conn.id, true);
  }

  private stopWakeWatch(connectionId: string): void {
    const timer: number | undefined = this.wakeTimers.get(connectionId);
    if (timer !== undefined) {
      clearInterval(timer);
      this.wakeTimers.delete(connectionId);
    }
    this.setWaking(connectionId, false);
  }

  private clearWakeWatches(): void {
    this.wakeTimers.forEach((timer: number): void => {
      clearInterval(timer);
    });
    this.wakeTimers.clear();
    this.wakingHosts = new Map<string, boolean>();
  }

  private setWaking(connectionId: string, waking: boolean): void {
    if ((this.wakingHosts.get(connectionId) === true) === waking) {
      return;
    }
    const next: Map<string, boolean> = new Map<string, boolean>(this.wakingHosts);
    if (waking) {
      next.set(connectionId, true);
    } else {
      next.delete(connectionId);
    }
    this.wakingHosts = next;
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
    if (this.connState(connectionId) === ConnectionState.Disconnecting) {
      return;
    }
    this.menuConnectionId = connectionId;
    this.menuSeq = this.menuSeq + 1;
  }

  private closeRowMenu(): void {
    this.menuConnectionId = '';
  }

  private armRowMenu(index: number): void {
    const row: SavedConnection | undefined = this.connections[index];
    if (row === undefined || this.connState(row.id) === ConnectionState.Disconnecting) {
      this.armedMenuId = '';
      return;
    }
    this.armedMenuId = row.id;
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
    this.stopWakeWatch(conn.id);
    const hostContext = this.getUIContext().getHostContext();
    if (hostContext !== undefined) {
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
      PageBackground({ uri: this.backgroundUri })
      Column() {
      PageHeader({
        variant: PageHeaderVariant.Brand,
        title: $r('app.string.app_name'),
        subtitle: '远程控制 Windows 桌面'
      }) {
        if (this.developerMode) {
          TextAction({
            label: '回放测试',
            muted: true,
            onTap: () => {
              this.getUIContext().getRouter().pushUrl({ url: 'pages/GfxReplayPage' });
            }
          })
            .margin({ right: 8 })
        }

        ControlDisc({
          icon: $r('app.media.ic_settings'),
          diameter: 44,
          glyph: 24,
          tip: '设置',
          onTap: () => {
            this.openSettings();
          }
        })
      }

      if (this.connections.length === 0) {
        StateView({
          kind: StateViewKind.Empty,
          icon: $r('app.media.app_icon'),
          title: '还没有保存的连接',
          message: '点击右上角 + 添加一个远程桌面',
          primaryLabel: '添加连接',
          onPrimary: () => {
            this.openNewConnection();
          }
        })
          .layoutWeight(1)
          .width('100%')
      } else {
        List({ space: 6 }) {
          ForEach(this.connections, (conn: SavedConnection, index: number) => {
            ListItem() {
              ConnectionRow({
                conn: conn,
                state: this.connState(conn.id),
                hostTargetId: this.hostTargetId(conn),
                targetId: this.targetId(conn),
                power: this.powerFor(conn),
                wakeReachability: this.wakeReachabilityFor(conn),
                waking: this.wakingFor(conn),
                probing: this.probingFor(conn),
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
                onWake: () => {
                  this.toggleWake(conn);
                },
                onDelete: () => {
                  this.confirmDelete(conn);
                }
              })
            }
            .margin({ left: 64, right: 88, top: 2, bottom: 2 })
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
        .padding({ left: 0, right: 0, top: 12, bottom: 20 })
        .scrollBar(BarState.Auto)
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
    
  }
}


