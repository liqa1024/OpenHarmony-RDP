/*
 * HmRdp - Moonlight application management (main-window page).
 *
 * The app layer for a Moonlight host: it lists the host's applications, shows
 * which one is running, and lets the user connect (the click always ends in a
 * session: start the app if needed, else connect directly), stop the running
 * app, or set the connection's default app. Reached from the Moonlight row's
 * app button; the pairing PIN stays a lightweight popup over the main window.
 * See doc_agent/moonlight-connection.md §4.
 */
import { common } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RdpConnectOrigin, SavedConnection } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { MoonlightAppsResult, MoonlightConnector } from '../services/MoonlightConnector';
import { MoonlightLauncher } from '../services/MoonlightLauncher';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { NvApp } from '../moonlight/MoonlightModels';
import { showToast } from '../utils/UiUtil';
import { promptPairingPin } from '../utils/PairingPrompt';

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
  @State error: string = '';
  @State state: ConnectionState = ConnectionState.Idle;
  // App icons fetched from the host (/appasset), keyed by app id.
  @State iconMap: Map<number, image.PixelMap> = new Map<number, image.PixelMap>();
  private conn: SavedConnection | undefined = undefined;
  private httpsPort: number = 47984;
  private readonly stateListener: SessionStateListener =
    (connectionId: string, state: ConnectionState, message: string, origin: RdpConnectOrigin): void => {
      if (this.conn !== undefined && connectionId === this.conn.id) {
        this.state = state;
      }
    };

  aboutToAppear(): void {
    const params: MoonlightAppsParams | undefined =
      this.getUIContext().getRouter().getParams() as MoonlightAppsParams;
    if (params !== undefined) {
      this.conn = ConnectionStore.get(params.connectionId);
    }
    SessionManager.subscribe(this.stateListener);
    if (this.conn !== undefined) {
      this.state = SessionManager.stateOf(this.conn.id);
      this.refresh();
    } else {
      this.loading = false;
      this.error = '连接不存在';
    }
  }

  aboutToDisappear(): void {
    SessionManager.unsubscribe(this.stateListener);
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

  private async refresh(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    this.loading = true;
    this.error = '';
    const result: MoonlightAppsResult = await MoonlightConnector.probeApps(this.filesDir(),
      conn.host, conn.port, (pin: string): Promise<boolean> => promptPairingPin(this.getUIContext(), pin));
    this.loading = false;
    if (!result.ok) {
      this.error = result.error;
      return;
    }
    this.apps = result.apps;
    this.currentGame = result.currentGame;
    this.httpsPort = result.httpsPort;
    this.loadIcons();
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

  /** Connect to one app: same operation whether it is running or not. */
  private connectApp(app: NvApp): void {
    const conn: SavedConnection | undefined = this.conn;
    const context = this.getUIContext().getHostContext();
    if (conn === undefined || context === undefined) {
      return;
    }
    // A specific app id; the pre-flight runs inside the manager (Connecting).
    this.error = '';
    MoonlightLauncher.connect(this.getUIContext(), context as common.UIAbilityContext, conn,
      RdpConnectOrigin.List, app.id);
  }

  private async closeApp(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    showToast(this.getUIContext(), '已请求关闭主机应用');
    await this.refresh();
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
    showToast(this.getUIContext(), `默认应用已设为「${app.name}」`);
  }

  private isRunning(app: NvApp): boolean {
    return app.id === this.currentGame;
  }

  private isDefault(app: NvApp): boolean {
    return this.conn !== undefined && this.conn.mlDefaultAppId === app.id.toString();
  }

  @Builder
  appMenu(app: NvApp) {
    Menu() {
      MenuItem({ content: '连接' })
        .enabled(this.state === ConnectionState.Idle || this.state === ConnectionState.Failed)
        .onClick(() => {
          this.connectApp(app);
        })
      MenuItem({ content: '设为默认' })
        .onClick(() => {
          this.setDefault(app);
        })
      if (this.isRunning(app)) {
        MenuItem({ content: '关闭应用' })
          .onClick(() => {
            this.closeApp();
          })
      }
    }
  }

  /** Square tile: the Moonlight host ships square app icons. Tap = connect. */
  @Builder
  appTile(app: NvApp) {
    Column() {
      Stack({ alignContent: Alignment.TopEnd }) {
        if (this.iconMap.get(app.id) !== undefined) {
          Image(this.iconMap.get(app.id) as image.PixelMap)
            .width('100%')
            .height('100%')
            .objectFit(ImageFit.Cover)
            .borderRadius(14)
        } else {
          Image($r('app.media.ic_apps'))
            .width(40)
            .height(40)
            .objectFit(ImageFit.Contain)
        }
        if (this.isRunning(app)) {
          Text('运行中')
            .fontSize(9)
            .fontColor('#FFFFFF')
            .backgroundColor($r('app.color.success'))
            .borderRadius(4)
            .padding({ left: 5, right: 5, top: 1, bottom: 1 })
            .margin(6)
        }
      }
      .width('100%')
      .aspectRatio(1)
      .borderRadius(14)
      .backgroundColor($r('app.color.card_bg'))
      .clip(true)

      Text(app.name)
        .fontSize(13)
        .fontColor($r('app.color.text_primary'))
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .margin({ top: 8 })

      if (this.isDefault(app)) {
        Text('默认')
          .fontSize(10)
          .fontColor($r('app.color.accent'))
          .margin({ top: 2 })
      }
    }
    .width(116)
    .margin({ right: 12, bottom: 16 })
    .onClick(() => {
      this.connectApp(app);
    })
    .bindContextMenu(this.appMenu(app), ResponseType.RightClick)
  }

  @Builder
  header() {
    Row() {
      Text('应用')
        .fontSize(18)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .layoutWeight(1)
      Text('刷新')
        .fontSize(14)
        .fontColor($r('app.color.accent'))
        .onClick(() => {
          this.refresh();
        })
    }
    .width('100%')
    .padding({ left: 4, right: 4, top: 8, bottom: 8 })
  }

  build() {
    Column() {
      Row() {
        Text('返回')
          .fontSize(14)
          .fontColor($r('app.color.accent'))
          .onClick(() => {
            this.getUIContext().getRouter().back();
          })
        Column() {
          Text(this.host())
            .fontSize(17)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Center)
        Text('')
          .fontSize(14)
          .width(32)
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 10, bottom: 10 })

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
            this.header()
            Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
              ForEach(this.apps, (app: NvApp) => {
                this.appTile(app)
              }, (app: NvApp) => app.id.toString())
            }
            .width('100%')
          }
          .width('100%')
          .padding({ left: 12, right: 12, bottom: 24 })
        }
        .layoutWeight(1)
        .align(Alignment.Top)
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
