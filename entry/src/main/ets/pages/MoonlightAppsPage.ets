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
  @State currentGame: number = 0;
  @State loading: boolean = true;
  @State refreshing: boolean = false;
  @State error: string = '';
  @State state: ConnectionState = ConnectionState.Idle;
  @State defaultAppId: string = '';
  @State iconMap: Map<number, image.PixelMap> = new Map<number, image.PixelMap>();
  private conn: SavedConnection | undefined = undefined;
  private httpsPort: number = 47984;
  private hasLoaded: boolean = false;
  private probing: boolean = false;
  private pendingAppId: number = 0;
  private pendingResume: boolean = false;
  private readonly stateListener: SessionStateListener =
    (connectionId: string, state: ConnectionState, message: string, origin: RdpConnectOrigin): void => {
      if (this.conn !== undefined && connectionId === this.conn.id) {
        this.state = state;
        if (state !== ConnectionState.Connecting) {
          this.pendingAppId = 0;
          this.pendingResume = false;
        }
      }
    };

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
      MoonlightHostState.setProbed(conn.id, result.currentGame, result.httpsPort);
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

  private hasActiveSession(): boolean {
    return this.state === ConnectionState.Connecting || this.state === ConnectionState.Connected ||
      this.state === ConnectionState.Disconnecting;
  }

  private sessionEstablished(): boolean {
    return this.state === ConnectionState.Connected ||
      this.state === ConnectionState.Disconnecting;
  }

  private isConnecting(app: NvApp): boolean {
    return this.pendingAppId === app.id;
  }

  private displayRunning(app: NvApp): boolean {
    if (this.isConnecting(app) && !this.pendingResume) {
      return false;
    }
    return this.isRunning(app);
  }

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
    this.error = '';
    this.pendingAppId = app.id;
    this.pendingResume = this.isRunning(app);
    MoonlightHostState.markLaunched(conn.id, app.id);
    MoonlightLauncher.connect(this.getUIContext(), context as common.UIAbilityContext, conn,
      RdpConnectOrigin.List, app.id);
  }

  private disconnectSession(): void {
    const conn: SavedConnection | undefined = this.conn;
    if (conn !== undefined) {
      SessionManager.disconnect(conn.id);
    }
  }

  private async closeApp(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    const ok: boolean = await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    showToast(this.getUIContext(), ok ? '已请求关闭主机应用' : '关闭主机应用失败');
    if (ok) {
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

  private appTip(app: NvApp): string {
    if (this.isRunning(app)) {
      return app.name;
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

  // @Prop cannot carry the PixelMap (it does not survive the deep copy), so the tile key
  // carries the icon's per-app presence to recreate the tile only when its icon arrives.
  private tileKey(app: NvApp): string {
    return `${app.id}#${this.iconMap.has(app.id) ? 1 : 0}`;
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
                    isRunning: this.displayRunning(app),
                    hasSession: this.sessionEstablished(),
                    connecting: this.isConnecting(app),
                    loading: this.isConnecting(app) && !this.pendingResume,
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
            .padding({ left: 12, right: 88, top: 16, bottom: 96 })
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
