import { common } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ConnectAction, ConnectOrigin, HostPower, MoonlightConnection,
  SavedConnection } from '../model/Connections';
import { ConnectionStore } from '../services/ConnectionStore';
import { MoonlightAppsResult, MoonlightConnector } from '../services/MoonlightConnector';
import { MoonlightLauncher } from '../services/MoonlightLauncher';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState, HostTargetListener }
  from '../services/MoonlightHostState';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { MoonlightAppTile } from '../components/MoonlightAppTile';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { NvApp, PairResult } from '../moonlight/MoonlightModels';
import { BackgroundImage } from '../services/BackgroundImage';
import { SettingsStore } from '../services/SettingsStore';
import { connectActionFor } from '../utils/ConnectAction';
import { showToast } from '../utils/UiUtil';
import { promptPairingPin } from '../utils/PairingPrompt';
import { FabButton } from '../components/FabButton';
import { PageHeader } from '../components/PageHeader';
import { PageBackground } from '../components/PageBackground';
import { StateView, StateViewKind } from '../components/StateView';

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
  @State backgroundUri: string = '';
  private conn: MoonlightConnection | undefined = undefined;
  private connectionId: string = '';
  private httpsPort: number = 47984;
  private hasLoaded: boolean = false;
  private probing: boolean = false;
  private pendingAppId: number = 0;
  private pendingResume: boolean = false;
  private readonly stateListener: SessionStateListener =
    (connectionId: string, state: ConnectionState, message: string, origin: ConnectOrigin): void => {
      if (this.conn !== undefined && connectionId === this.conn.id) {
        this.state = state;
        if (state !== ConnectionState.Connecting) {
          this.pendingAppId = 0;
          this.pendingResume = false;
        }
      }
    };

  private readonly hostStateListener: HostTargetListener =
    (connectionId: string, currentGame: number, _power: HostPower, _probing: boolean): void => {
      if (this.conn !== undefined && connectionId === this.conn.id) {
        this.currentGame = currentGame;
      }
    };

  aboutToAppear(): void {
    this.applyBackground();
    const params: MoonlightAppsParams | undefined =
      this.getUIContext().getRouter().getParams() as MoonlightAppsParams;
    if (params !== undefined) {
      this.connectionId = params.connectionId;
      this.resolveConnection();
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
    this.applyBackground();
    const conn: MoonlightConnection | undefined = this.conn;
    if (conn !== undefined && this.hasLoaded) {
      MoonlightHostState.refresh(conn, this.filesDir());
    }
  }

  private applyBackground(): void {
    const settings = SettingsStore.load();
    const path: string = settings.appsBackgroundImage.length > 0
      ? settings.appsBackgroundImage : settings.backgroundImage;
    this.backgroundUri = BackgroundImage.uriOf(path);
  }

  private resolveConnection(): void {
    if (this.connectionId.length === 0) {
      this.conn = undefined;
      return;
    }
    const loaded: SavedConnection | undefined = ConnectionStore.get(this.connectionId);
    this.conn = loaded instanceof MoonlightConnection ? loaded : undefined;
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
    if (this.conn === undefined) {
      this.resolveConnection();
    }
    const conn: MoonlightConnection | undefined = this.conn;
    if (conn === undefined) {
      this.loading = false;
      this.error = '连接不存在';
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
    const conn: MoonlightConnection | undefined = this.conn;
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
    const conn: MoonlightConnection | undefined = this.conn;
    const context = this.getUIContext().getHostContext();
    if (conn === undefined || context === undefined) {
      return;
    }
    this.error = '';
    this.pendingAppId = app.id;
    this.pendingResume = this.isRunning(app);
    MoonlightLauncher.connect(this.getUIContext(), context as common.UIAbilityContext, conn,
      ConnectOrigin.List, app.id);
  }

  private disconnectSession(): void {
    const conn: MoonlightConnection | undefined = this.conn;
    if (conn !== undefined) {
      SessionManager.disconnect(conn.id);
    }
  }

  private async closeApp(): Promise<void> {
    const conn: MoonlightConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    const ok: boolean = await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    showToast(this.getUIContext(), ok ? '已请求关闭主机应用' : '无法关闭主机应用：主机离线');
    if (ok) {
      MoonlightHostState.markStopped(conn.id);
      MoonlightHostState.refreshAfter(conn, this.filesDir(), HOST_REPROBE_DELAY_MS);
    } else {
      MoonlightHostState.refresh(conn, this.filesDir());
    }
  }

  private async closeAppAndDisconnect(): Promise<void> {
    const conn: MoonlightConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    const ok: boolean = await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    if (ok) {
      MoonlightHostState.markStopped(conn.id);
      MoonlightHostState.refreshAfter(conn, this.filesDir(), HOST_REPROBE_DELAY_MS);
    }
    SessionManager.disconnect(conn.id);
    showToast(this.getUIContext(), ok ? '已关闭主机应用并断开连接' : '已断开连接：主机离线');
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
    const conn: MoonlightConnection | undefined = this.conn;
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

  private tileKey(app: NvApp): string {
    return `${app.id}#${this.iconMap.has(app.id) ? 1 : 0}`;
  }

  build() {
    Stack({ alignContent: Alignment.BottomEnd }) {
      PageBackground({ uri: this.backgroundUri })
      Column() {
        PageHeader({
          title: '应用',
          leftLabel: '返回',
          onLeft: () => {
            this.getUIContext().getRouter().back();
          }
        }) {
          Text(this.label())
            .fontSize(13)
            
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .constraintSize({ maxWidth: 320 })
        }

        if (this.loading) {
          StateView({
            kind: StateViewKind.Loading
          })
            .width('100%')
            .layoutWeight(1)
        } else if (this.error.length > 0) {
          StateView({
            kind: StateViewKind.Error,
            message: this.error,
            primaryLabel: '重试',
            onPrimary: () => {
              this.refresh();
            }
          })
            .width('100%')
            .layoutWeight(1)
        } else if (this.apps.length === 0) {
          StateView({
            kind: StateViewKind.Empty,
            title: '没有可用应用',
            message: '主机未返回应用列表',
            primaryLabel: '刷新',
            onPrimary: () => {
              this.refresh();
            }
          })
            .width('100%')
            .layoutWeight(1)
        } else {
          Scroll() {
            Column() {
              Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
                ForEach(this.apps, (app: NvApp) => {
                  MoonlightAppTile({
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
            .padding({ left: 24, right: 88, top: 16, bottom: 16 })
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
    
  }
}


