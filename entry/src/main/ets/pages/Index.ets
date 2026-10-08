/*
 * HmRdp - saved connections list (home page).
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AdvancedProfile, DisplayProfile, EditConnectionParams,
  PendingConnection, RdpConnectOptions,
  RdpConnectOrigin, SavedConnection } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { DeviceCapabilities } from '../services/DeviceCapabilities';
import { MoonlightLauncher } from '../services/MoonlightLauncher';
import { MoonlightConnector } from '../services/MoonlightConnector';
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
  // Host-side target per connection, mirrored from MoonlightHostState so the row
  // (恢复 / 运行中 / 终止) repaints: what the host currently has open (0 = none /
  // unknown). Only Moonlight has one; RDP stays 0. The app page reads the same
  // authority, so both surfaces agree.
  @State hostTargets: Map<string, number> = new Map<string, number>();
  // Index of the item being drag-reordered; -1 when idle. Used only to lift the
  // dragged item above its siblings.
  @State draggingIndex: number = -1;
  @State settingsHovered: boolean = false;
  @State settingsPressed: boolean = false;
  // Developer options (SettingsStore.developerMode): the replay entry is a dev
  // tool, so it only exists while this is on.
  @State developerMode: boolean = false;

  // Tooltips carry the immersive material in their options object, which an
  // older system simply ignores. The connection card owns its own material
  // inside the shared ConnectionRow component.
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };

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

  // Mirrors MoonlightHostState into the row map: the authority pushes every
  // change (client action or host probe), so the row is correct without probing.
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

  /**
   * Reconciles the badge map with the authority (SessionManager) on every show,
   * instead of trusting only the listener pushes: a state that changed while this
   * page was hidden must render immediately when it comes back.
   */
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

  /** Seeds the row map from the authority, so a show renders the current value. */
  private syncHostTargets(): void {
    const next: Map<string, number> = new Map<string, number>();
    for (const conn of this.connections) {
      if (!conn.isMoonlight()) {
        continue;
      }
      const target: number = MoonlightHostState.targetOf(conn.id);
      if (target !== 0) {
        next.set(conn.id, target);
      }
    }
    this.hostTargets = next;
  }

  /**
   * Best-effort host-state refresh for the homepage: for each Moonlight
   * connection ask the shared authority to probe the host's running app (plain
   * `serverinfo`, no pairing, no polling), so the row can show 恢复 / 运行中. A
   * host that is offline keeps its last known value.
   */
  private probeHosts(): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    const filesDir: string = (context as common.UIAbilityContext).filesDir;
    MoonlightHostState.refreshAll(this.connections, filesDir);
  }

  /**
   * 终止: ask the host to stop the app it is running (`/cancel`, HTTPS + mTLS),
   * then drop a local session if one exists. A successful cancel clears the shared
   * host target immediately (both surfaces agree without waiting for a probe).
   */
  private terminateHostApp(conn: SavedConnection): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    const filesDir: string = (context as common.UIAbilityContext).filesDir;
    const httpsPort: number = MoonlightHostState.httpsPortOf(conn.id);
    MoonlightClient.cancel(filesDir, conn.host, httpsPort)
      .then((ok: boolean): void => {
        showToast(this.getUIContext(), ok ? '已请求终止主机应用' : '终止主机应用失败');
        if (ok) {
          MoonlightHostState.markStopped(conn.id);
          MoonlightHostState.refreshAfter(conn, filesDir, HOST_REPROBE_DELAY_MS);
        }
        if (this.connState(conn.id) !== ConnectionState.Idle) {
          SessionManager.disconnect(conn.id);
        }
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'terminate host app failed: %{public}s', JSON.stringify(error));
      });
  }

  /** What the host currently has open for this connection (0 = RDP / unknown / none). */
  private hostTargetId(conn: SavedConnection): number {
    if (!conn.isMoonlight()) {
      return 0;
    }
    const value: number | undefined = this.hostTargets.get(conn.id);
    return value === undefined ? 0 : value;
  }

  /** What the row's connect would open: the stored default (0 = host default). */
  private targetId(conn: SavedConnection): number {
    if (!conn.isMoonlight() || conn.mlDefaultAppId.length === 0) {
      return 0;
    }
    const parsed: number = Number.parseInt(conn.mlDefaultAppId, 10);
    return Number.isNaN(parsed) ? 0 : parsed;
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
      // The teardown is in flight: a new attempt is refused until it finishes.
      return;
    }
    this.connect(conn);
  }

  private showConnectError(message: string): void {
    showNotice(this.getUIContext(), '连接失败', message);
  }

  private async startConnection(conn: SavedConnection): Promise<void> {
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    if (conn.isMoonlight()) {
      // The manager drives the control-layer pre-flight (serverinfo -> pair ->
      // launch) while Connecting; see MoonlightLauncher.
      MoonlightLauncher.connect(this.getUIContext(), context as common.UIAbilityContext, conn,
        RdpConnectOrigin.List);
      return;
    }
    const options: RdpConnectOptions = conn.toOptions();
    const request: PendingConnection = new PendingConnection();
    request.connectionId = conn.id;
    request.origin = RdpConnectOrigin.List;
    request.onConnected = undefined;
    request.protocol = conn.protocol;
    request.savedConnection = conn;
    const profile: DisplayProfile = SettingsStore.resolveDisplay(conn);
    options.width = profile.width;
    options.height = profile.height;
    options.scalePercent = profile.scalePercent;
    options.srEnabled = profile.srEnabled;
    options.srRatioPercent = profile.srRatioPercent;
    options.srBackend = profile.srBackend;
    const advanced: AdvancedProfile = SettingsStore.resolveAdvanced(conn);
    options.enableAudio = advanced.enableAudio && DeviceCapabilities.audio().supported;
    options.ignoreCertificate = advanced.ignoreCertificate;
    options.password = await CredentialStore.loadPassword(conn.id);
    request.options = options;
    SessionManager.connect(request, context as common.UIAbilityContext);
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

  /**
   * Opens the editor for a new connection. The protocol is picked inside the
   * editor itself, so this just defaults to the last one the user added.
   */
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
    if (conn.isMoonlight()) {
      // Drop the host pairing with the connection, or a later re-add would skip
      // the PIN round (the pairing is per host, not per connection).
      const hostContext = this.getUIContext().getHostContext();
      if (hostContext !== undefined) {
        MoonlightConnector.forgetHost((hostContext as common.UIAbilityContext).filesDir,
          conn.host, conn.port);
      }
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
      message: `确定要删除「${conn.label()}」？该操作无法撤销。`,
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

        // Dev-only entry (doc_agent/gfx-engine.md §6): recorded RDP replay page.
        // Only present while developer options are on.
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
                onDelete: () => {
                  this.confirmDelete(conn);
                }
              })
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
