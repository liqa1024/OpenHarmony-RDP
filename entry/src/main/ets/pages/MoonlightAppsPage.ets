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
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { NvApp } from '../moonlight/MoonlightModels';
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
  @State error: string = '';
  @State state: ConnectionState = ConnectionState.Idle;
  // The connection's default app id, mirrored into @State so the star repaints
  // when it is set (the saved record itself is not observable).
  @State defaultAppId: string = '';
  // App icons fetched from the host (/appasset), keyed by app id.
  @State iconMap: Map<number, image.PixelMap> = new Map<number, image.PixelMap>();
  private conn: SavedConnection | undefined = undefined;
  private httpsPort: number = 47984;
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private cardMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.CARD));
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
      this.defaultAppId = this.conn.mlDefaultAppId;
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

  /** The connection's name or "host:port", shown at the header's right edge. */
  private label(): string {
    return this.conn !== undefined ? this.conn.label() : '';
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
    this.defaultAppId = this.resolveDefaultAppId(result.apps);
    this.loadIcons();
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

  /** 中断: ask the host to stop its running app, then refresh the list. */
  private async closeApp(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    showToast(this.getUIContext(), '已请求关闭主机应用');
    await this.refresh();
  }

  private async closeAppAndDisconnect(): Promise<void> {
    const conn: SavedConnection | undefined = this.conn;
    if (conn === undefined) {
      return;
    }
    await MoonlightClient.cancel(this.filesDir(), conn.host, this.httpsPort);
    SessionManager.disconnect(conn.id);
    showToast(this.getUIContext(), '已关闭主机应用并断开连接');
    await this.refresh();
  }

  /** Card tooltip: a non-running app means "连接", or "切换" when one is running. */
  private appTip(app: NvApp): string {
    if (this.isRunning(app)) {
      return '连接';
    }
    return this.currentGame !== 0 ? '切换到此应用' : '连接';
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

  /** One round icon action shown for the running app (connect / 断开 / 关闭). */
  @Builder
  tileAction(icon: Resource, color: ResourceColor, tip: string, gap: number, action: () => void) {
    Row() {
      Image(icon)
        .width(18)
        .height(18)
        .fillColor(color)
        .draggable(false)
    }
    .width(32)
    .height(32)
    .borderRadius(16)
    .backgroundColor($r('app.color.input_bg'))
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .margin({ right: gap })
    .hoverEffect(HoverEffect.Highlight)
    .bindTips(tip, this.tipsOptions)
    .onClick(() => {
      action();
    })
  }

  /** Square tile: name on top, the app icon, then (when running) the actions. */
  @Builder
  appTile(app: NvApp) {
    Column() {
      // Name on top, with the set-as-default star at the card's top-right. It
      // sits on the card surface (not over the icon), so it needs no scrim.
      Stack({ alignContent: Alignment.End }) {
        Text(app.name)
          .fontSize(13)
          .fontColor($r('app.color.text_primary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
          .textAlign(TextAlign.Center)
          .padding({ left: 26, right: 26 })

        Image($r('app.media.ic_star'))
          .width(18)
          .height(18)
          .fillColor(this.isDefault(app)
            ? $r('app.color.accent') : $r('app.color.text_tertiary'))
          .draggable(false)
          .bindTips('设为默认', this.tipsOptions)
          .onClick(() => {
            this.setDefault(app);
          })
      }
      .width('100%')
      .height(24)

      Stack({ alignContent: Alignment.TopStart }) {
        Stack({ alignContent: Alignment.Center }) {
          if (this.iconMap.get(app.id) !== undefined) {
            Image(this.iconMap.get(app.id) as image.PixelMap)
              .width('100%')
              .height('100%')
              .objectFit(ImageFit.Cover)
              .draggable(false)
          } else {
            // The host ships square art; the placeholder is centered with a
            // little breathing room so a non-square icon still reads well.
            Image($r('app.media.ic_apps'))
              .width(36)
              .height(36)
              .objectFit(ImageFit.Contain)
              .draggable(false)
          }
        }
        .width('100%')
        .aspectRatio(1)
        .borderRadius(14)
        .clip(true)
        .backgroundColor($r('app.color.input_bg'))
        // The card's "连接 / 切换" hint lives on the icon only: on the whole card
        // it would also fire while hovering the star / action buttons (nested
        // bindTips are not suppressed by the inner one).
        .bindTips(this.appTip(app), this.tipsOptions)

        if (this.isRunning(app)) {
          Text('运行中')
            .fontSize(9)
            .fontColor('#FFFFFF')
            .backgroundColor($r('app.color.success'))
            .borderRadius(4)
            .padding({ left: 5, right: 5, top: 1, bottom: 1 })
            .margin({ left: 6, top: 6 })
        }
      }
      .width('100%')
      .margin({ top: 4 })

      // The running app gets two explicit actions on one row, selected by whether
      // a session is already up: [连接 / 断开] then [关闭].
      if (this.isRunning(app)) {
        Row() {
          if (this.hasActiveSession()) {
            this.tileAction($r('app.media.ic_power'), $r('app.color.text_secondary'),
              '断开连接', 10,
              (): void => {
                this.disconnectSession();
              })

            this.tileAction($r('app.media.ic_stop'), $r('app.color.danger'), '关闭应用并断开', 0,
              (): void => {
                this.closeAppAndDisconnect();
              })
          } else {
            this.tileAction($r('app.media.ic_play'), $r('app.color.accent'), '连接', 10,
              (): void => {
                this.openApp(app);
              })

            this.tileAction($r('app.media.ic_stop'), $r('app.color.danger'), '关闭应用', 0,
              (): void => {
                this.closeApp();
              })
          }
        }
        .margin({ top: 8 })
      }
    }
    .width(124)
    .padding(8)
    .borderRadius(16)
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : $r('app.color.card_bg'))
    .attributeModifier(this.cardMaterial)
    .shadow({ radius: 8, color: '#10000000', offsetX: 0, offsetY: 2 })
    .margin({ right: 12, bottom: 16 })
    .onClick(() => {
      this.openApp(app);
    })
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
                  this.appTile(app)
                }, (app: NvApp) => app.id.toString())
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
