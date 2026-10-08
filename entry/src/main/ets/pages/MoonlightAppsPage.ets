/*
 * HmRdp - Moonlight application management (main-window page).
 *
 * The app layer for a Moonlight host: it lists the host's applications, shows
 * which one is running, and lets the user connect (a card tap always ends in a
 * session: start the app if needed, else connect directly), set the connection's
 * default app (the star), or stop the running app. Reached from the Moonlight
 * row's app button; the pairing PIN stays a lightweight popup over the main
 * window. See doc_agent/moonlight-connection.md §4.
 */
import { common } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RdpConnectOrigin, SavedConnection } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { MoonlightAppsResult, MoonlightConnector } from '../services/MoonlightConnector';
import { MoonlightLauncher } from '../services/MoonlightLauncher';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState, HostTargetListener }
  from '../services/MoonlightHostState';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { MoonlightAppTile } from '../components/MoonlightAppTile';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { NvApp, PairResult } from '../moonlight/MoonlightModels';
import { ConnectAction, connectActionFor } from '../utils/ConnectAction';
import { showToast } from '../utils/UiUtil';
import { promptPairingPin } from '../utils/PairingPrompt';
import { FabButton } from '../components/FabButton';
import { HeaderBar } from '../components/HeaderBar';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMoonlightApps';

export interface MoonlightAppsParams {
  connectionId: string;
}

@Entry
@Component
struct MoonlightAppsPage {
  @State apps: NvApp[] = [];
  // The host's running app, mirrored from MoonlightHostState (the shared
  // authority) so this page and the list never disagree; never written directly.
  @State currentGame: number = 0;
  @State loading: boolean = true;
  // A refresh over an already-loaded list: only the FAB spins, the page keeps
  // showing the current apps until the new result arrives.
  @State refreshing: boolean = false;
  @State error: string = '';
  @State state: ConnectionState = ConnectionState.Idle;
  // The connection's default app id, mirrored into @State so the star repaints
  // when it is set (the saved record itself is not observable).
  @State defaultAppId: string = '';
  // App icons fetched from the host (/appasset), keyed by app id.
  @State iconMap: Map<number, image.PixelMap> = new Map<number, image.PixelMap>();
  private conn: SavedConnection | undefined = undefined;
  private httpsPort: number = 47984;
  // True once a list has been shown, so later refreshes keep the page instead of
  // blanking it with the full-page spinner.
  private hasLoaded: boolean = false;
  // Coalesces overlapping probes: the first appearance refreshes from both
  // aboutToAppear and onPageShow, and a page show can also arrive while the FAB's
  // refresh is still in flight.
  private probing: boolean = false;
  private readonly stateListener: SessionStateListener =
    (connectionId: string, state: ConnectionState, message: string, origin: RdpConnectOrigin): void => {
      if (this.conn !== undefined && connectionId === this.conn.id) {
        this.state = state;
      }
    };

  // Mirrors MoonlightHostState into the running badge: the authority pushes every
  // change (client action or host probe), so returning from a session shows the
  // host's actual state instead of the last tapped app.
  private readonly hostStateListener: HostTargetListener =
    (connectionId: string, currentGame: number): void => {
      if (this.conn !== undefined && connectionId === this.conn.id) {
        this.currentGame = currentGame;
      }
    };

  aboutToAppear(): void {
    const params: MoonlightAppsParams | undefined =
      this.getUIContext().getRouter().getParams() as MoonlightAppsParams;
    if (params !== undefined) {
      this.conn = ConnectionStore.get(params.connectionId);
    }
    SessionManager.subscribe(this.stateListener);
    MoonlightHostState.subscribe(this.hostStateListener);
    if (this.conn !== undefined) {
      this.state = SessionManager.stateOf(this.conn.id);
      this.currentGame = MoonlightHostState.targetOf(this.conn.id);
      this.defaultAppId = this.conn.mlDefaultAppId;
      this.refresh();
    } else {
      this.loading = false;
      this.error = '连接不存在';
    }
  }

  aboutToDisappear(): void {
    SessionManager.unsubscribe(this.stateListener);
    MoonlightHostState.unsubscribe(this.hostStateListener);
  }

  /**
   * Re-reads the host state whenever the page shows again (same contract as the
   * list page): a session opened from here runs in its own window, so returning
   * to this page must not keep a stale "running" app. This is the shared, cheap
   * target probe; the app list itself is only re-read on the first load / FAB.
   */
  onPageShow(): void {
    const conn: SavedConnection | undefined = this.conn;
    if (conn !== undefined && this.hasLoaded) {
      MoonlightHostState.refresh(conn, this.filesDir());
    }
  }

  private filesDir(): string {
    const context = this.getUIContext().getHostContext();
    return context !== undefined ? (context as common.UIAbilityContext).filesDir : '';
  }

  private host(): string {
    return this.conn !== undefined ? this.conn.host : '';
  }

  private port(): number {
    return this.conn !== undefined ? this.conn.port : 47989;
  }

  /** The connection's name or "host:port", shown at the header's right edge. */
  private label(): string {
    return this.conn !== undefined ? this.conn.label() : '';
  }

  private async refresh(): Promise<void> {
    if (this.probing) {
      return;
    }
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    this.probing = true;
    // Keep the current page on a re-refresh; only the first load takes the
    // full-page spinner (there is nothing to preserve yet).
    if (this.hasLoaded) {
      this.refreshing = true;
    } else {
      this.loading = true;
    }
    this.error = '';
    try {
      const result: MoonlightAppsResult = await MoonlightConnector.probeApps(this.filesDir(),
        conn.host, conn.port,
        (pin: string, pairing: Promise<PairResult>): Promise<boolean> =>
          promptPairingPin(this.getUIContext(), pin, pairing),
        conn.mlHostId);
      if (!result.ok) {
        this.error = result.error;
        return;
      }
      this.hasLoaded = true;
      this.apps = result.apps;
      this.httpsPort = result.httpsPort;
      // Feed the host's own answer into the shared authority: it overwrites the
      // optimistic value a connect wrote, and the listener pushes it back here.
      MoonlightHostState.setProbed(conn.id, result.currentGame, result.httpsPort);
      // Record the host's own id for the connection-uniqueness check (only when
      // it changed, so a plain refresh does not write the store every time).
      if (result.hostId.length > 0 && conn.mlHostId !== result.hostId) {
        conn.mlHostId = result.hostId;
        conn.updatedAt = Date.now();
        ConnectionStore.save(conn);
      }
      this.defaultAppId = this.resolveDefaultAppId(result.apps);
      this.loadIcons();
    } finally {
      this.loading = false;
      this.refreshing = false;
      this.probing = false;
    }
  }

  /**
   * The star marks the app a connect would open: the connection's explicit
   * default when it still exists, else the host default (@see pickApp). Without
   * this fallback an unset default shows no star even though connecting opens an
   * app.
   */
  private resolveDefaultAppId(apps: NvApp[]): string {
    const stored: string = this.conn !== undefined ? this.conn.mlDefaultAppId : '';
    if (apps.length === 0) {
      return stored;
    }
    for (let i = 0; i < apps.length; i++) {
      if (apps[i].id.toString() === stored) {
        return stored;
      }
    }
    return MoonlightConnector.pickApp(apps).toString();
  }

  /** Fetches and decodes each app's icon; failures just keep the placeholder. */
  private async loadIcons(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    const filesDir: string = this.filesDir();
    for (const app of this.apps) {
      if (this.iconMap.get(app.id) !== undefined) {
        continue;
      }
      const bytes: ArrayBuffer | undefined =
        await MoonlightClient.fetchAppAsset(filesDir, conn.host, this.httpsPort, app.id);
      if (bytes === undefined) {
        continue;
      }
      try {
        const source: image.ImageSource = image.createImageSource(bytes);
        const pixelMap: image.PixelMap = await source.createPixelMap();
        source.release();
        const next: Map<number, image.PixelMap> = new Map<number, image.PixelMap>(this.iconMap);
        next.set(app.id, pixelMap);
        this.iconMap = next;
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'icon decode failed: %{public}s', JSON.stringify(error));
      }
    }
  }

  /** True while this connection already has a session (multi-open is not allowed). */
  private hasActiveSession(): boolean {
    return this.state === ConnectionState.Connecting || this.state === ConnectionState.Connected ||
      this.state === ConnectionState.Disconnecting;
  }

  /**
   * Open one app: a card tap (or the running app's 连接). One connection supports
   * one session, so if one is already up it is dropped first and the new app
   * opened — this is the "switch app" action, with no dialog (the tooltip says
   * what the tap does).
   */
  private openApp(app: NvApp): void {
    if (this.hasActiveSession() && this.conn !== undefined) {
      SessionManager.disconnect(this.conn.id);
    }
    this.launch(app);
  }

  private launch(app: NvApp): void {
    const conn: SavedConnection | undefined = this.conn;
    const context = this.getUIContext().getHostContext();
    if (conn === undefined || context === undefined) {
      return;
    }
    // A specific app id; the pre-flight runs inside the manager (Connecting).
    this.error = '';
    // Mark the target as the running app right away (through the shared
    // authority): connecting starts it (and a switch stops the previous one), so
    // its tile must show the 运行中 badge without waiting for the next probe.
    MoonlightHostState.markLaunched(conn.id, app.id);
    MoonlightLauncher.connect(this.getUIContext(), context as common.UIAbilityContext, conn,
      RdpConnectOrigin.List, app.id);
  }

  /** Ends the session but leaves the host app running. */
  private disconnectSession(): void {
    const conn: SavedConnection | undefined = this.conn;
    if (conn !== undefined) {
      SessionManager.disconnect(conn.id);
    }
  }

  /** 中断: ask the host to stop its running app; the shared target clears on success. */
  private async closeApp(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    const ok: boolean = await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    showToast(this.getUIContext(), ok ? '已请求关闭主机应用' : '关闭主机应用失败');
    if (ok) {
      // The client knows the app is gone; clear it now (both surfaces agree) and
      // reconcile with the host once the cancel has settled.
      MoonlightHostState.markStopped(conn.id);
      MoonlightHostState.refreshAfter(conn, this.filesDir(), HOST_REPROBE_DELAY_MS);
    }
  }

  private async closeAppAndDisconnect(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    const ok: boolean = await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    if (ok) {
      MoonlightHostState.markStopped(conn.id);
      MoonlightHostState.refreshAfter(conn, this.filesDir(), HOST_REPROBE_DELAY_MS);
    }
    SessionManager.disconnect(conn.id);
    showToast(this.getUIContext(), ok ? '已关闭主机应用并断开连接' : '已断开连接');
  }

  /**
   * The poster is the tile and carries no name, so the tooltip both names the
   * app and says what a click does: "切换到" when another app is running, else
   * "连接到". A running app has its own action discs and its card no longer
   * reacts, so its tip is empty (no tooltip).
   */
  private appTip(app: NvApp): string {
    if (this.isRunning(app)) {
      return '';
    }
    const action: ConnectAction =
      connectActionFor(ConnectionState.Idle, this.currentGame, app.id);
    return action === ConnectAction.Switch ? `切换到「${app.name}」`
                                           : `连接到「${app.name}」`;
  }

  private setDefault(app: NvApp): void {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    conn.mlDefaultAppId = app.id.toString();
    conn.mlDefaultAppName = app.name;
    conn.updatedAt = Date.now();
    ConnectionStore.save(conn);
    this.defaultAppId = conn.mlDefaultAppId;
    showToast(this.getUIContext(), `默认应用已设为「${app.name}」`);
  }

  private isRunning(app: NvApp): boolean {
    return app.id === this.currentGame;
  }

  private isDefault(app: NvApp): boolean {
    return this.defaultAppId === app.id.toString();
  }

  /**
   * The tile only needs a new ForEach key when its identity data changes: the
   * loaded icon, which @Prop cannot carry (its PixelMap does not survive the
   * deep copy), so the tile is recreated with the new bitmap. Everything else
   * reactive (default / running / session) updates in place through @Prop.
   */
  private tileKey(app: NvApp): string {
    return `${app.id}#${this.iconMap.size}`;
  }

  build() {
    Stack({ alignContent: Alignment.BottomEnd }) {
      Column() {
        HeaderBar({
          title: '应用',
          leftLabel: '返回',
          onLeft: () => {
            this.getUIContext().getRouter().back();
          }
        }) {
          Text(this.label())
            .fontSize(13)
            .fontColor($r('app.color.text_tertiary'))
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .constraintSize({ maxWidth: 320 })
        }

        if (this.loading) {
          Column() {
            LoadingProgress().width(40).height(40).color($r('app.color.accent'))
          }
          .width('100%')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
        } else if (this.error.length > 0) {
          Column() {
            Text(this.error)
              .fontSize(14)
              .fontColor($r('app.color.text_secondary'))
              .textAlign(TextAlign.Center)
              .margin({ bottom: 12 })
            Button('重试')
              .onClick(() => {
                this.refresh();
              })
          }
          .width('100%')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
        } else {
          Scroll() {
            Column() {
              Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
                ForEach(this.apps, (app: NvApp) => {
                  MoonlightAppTile({
                    app: app,
                    icon: this.iconMap.get(app.id),
                    isDefault: this.isDefault(app),
                    isRunning: this.isRunning(app),
                    hasSession: this.hasActiveSession(),
                    tip: this.appTip(app),
                    onOpen: () => {
                      this.openApp(app);
                    },
                    onSetDefault: () => {
                      this.setDefault(app);
                    },
                    onDisconnect: () => {
                      this.disconnectSession();
                    },
                    onCloseApp: () => {
                      this.closeApp();
                    },
                    onCloseAppAndDisconnect: () => {
                      this.closeAppAndDisconnect();
                    }
                  })
                }, (app: NvApp) => this.tileKey(app))
              }
              .width('100%')
            }
            .width('100%')
            .padding({ left: 12, right: 12, top: 16, bottom: 96 })
          }
          .layoutWeight(1)
          .align(Alignment.Top)
        }
      }
      .width('100%')
      .height('100%')

      FabButton({
        icon: $r('app.media.ic_refresh'),
        tip: '刷新',
        loading: this.refreshing,
        onTap: () => {
          this.refresh();
        }
      })
        .margin({ right: 24, bottom: 24 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
