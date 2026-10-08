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
import { NvApp, PairResult } from '../moonlight/MoonlightModels';
import { showToast } from '../utils/UiUtil';
import { promptPairingPin } from '../utils/PairingPrompt';
import { FabButton } from '../components/FabButton';
import { HeaderBar } from '../components/HeaderBar';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMoonlightApps';
// Moonlight's box art is 200x267 (see moonlight-qt AppView.qml): the icon tile
// keeps that width:height so a fetched poster is not cropped by a wrong frame.
const BOXART_ASPECT: number = 200 / 267;
// The tile is the poster itself (no card / frame wrapper). Its box-art width.
const TILE_WIDTH: number = 108;
const TILE_HEIGHT: number = TILE_WIDTH / BOXART_ASPECT;
const TILE_RADIUS: number = 14;
// On-poster controls sit on a THIN immersive-material disc (fallback: a flat
// input_bg disc). The star's disc stays small; the running app's action discs
// are bigger so they are easier to hit.
const STAR_CONTROL: number = 28;
const STAR_SIZE: number = 18;
const ACTION_CONTROL: number = 36;
const ACTION_SIZE: number = 22;

export interface MoonlightAppsParams {
  connectionId: string;
}

@Entry
@Component
struct MoonlightAppsPage {
  @State apps: NvApp[] = [];
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
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private controlMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.CONTROL),
      BlurStyle.COMPONENT_THIN);
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
    // Keep the current page on a re-refresh; only the first load takes the
    // full-page spinner (there is nothing to preserve yet).
    if (this.hasLoaded) {
      this.refreshing = true;
    } else {
      this.loading = true;
    }
    this.error = '';
    const result: MoonlightAppsResult = await MoonlightConnector.probeApps(this.filesDir(),
      conn.host, conn.port,
      (pin: string, pairing: Promise<PairResult>): Promise<boolean> =>
        promptPairingPin(this.getUIContext(), pin, pairing),
      conn.mlHostId);
    this.loading = false;
    this.refreshing = false;
    if (!result.ok) {
      this.error = result.error;
      return;
    }
    this.hasLoaded = true;
    this.apps = result.apps;
    this.currentGame = result.currentGame;
    this.httpsPort = result.httpsPort;
    // Record the host's own id for the connection-uniqueness check (only when it
    // changed, so a plain refresh does not write the store every time).
    if (result.hostId.length > 0 && conn.mlHostId !== result.hostId) {
      conn.mlHostId = result.hostId;
      conn.updatedAt = Date.now();
      ConnectionStore.save(conn);
    }
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

  /**
   * The poster is the tile and carries no name, so the tooltip both names the
   * app and says what a click does: "切换到" when another app is running, else
   * "连接到".
   */
  private appTip(app: NvApp): string {
    if (this.isRunning(app)) {
      return `连接到「${app.name}」`;
    }
    return this.currentGame !== 0 ? `切换到「${app.name}」` : `连接到「${app.name}」`;
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
   * A round on-poster control: a THIN immersive-material disc, flat input_bg
   * where the system has no material. BLOCK_HIERARCHY keeps a tap off the poster
   * underneath, so the tile's own click does not also fire.
   */
  @Builder
  controlDisc(icon: Resource, color: ResourceColor, disc: number, glyph: number,
    gap: number, tip: string, action: () => void) {
    Row() {
      Image(icon)
        .width(glyph)
        .height(glyph)
        .fillColor(color)
        .draggable(false)
    }
    .width(disc)
    .height(disc)
    .borderRadius(disc / 2)
    // Explicit transparent background, like the connection rows: the material is
    // the surface (an opaque background would cover it).
    .backgroundColor(Color.Transparent)
    // Material when the system has it, otherwise a component-level frosted style
    // — both live in the modifier so the fallback blur is never set alongside
    // the material (it would sit above and hide it).
    .attributeModifier(this.controlMaterial)
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .margin({ right: gap })
    .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
    .bindTips(tip, this.tipsOptions)
    .onClick(() => {
      action();
    })
  }

  /** One icon action overlaid on the running app's poster. */
  @Builder
  tileAction(icon: Resource, color: ResourceColor, tip: string, gap: number, action: () => void) {
    this.controlDisc(icon, color, ACTION_CONTROL, ACTION_SIZE, gap, tip, action)
  }

  /** The set-as-default star, top-right on the poster. */
  @Builder
  starControl(app: NvApp) {
    this.controlDisc($r('app.media.ic_star'),
      this.isDefault(app) ? $r('app.color.accent') : $r('app.color.text_primary'),
      STAR_CONTROL, STAR_SIZE, 0, '设为默认',
      (): void => {
        this.setDefault(app);
      })
  }

  /**
   * The tile is the box art itself: no card / frame wrapper around it and no
   * name row. The name lives in the hover tip; the default star sits at the
   * poster's top-right and the running app's actions at its bottom-center, both
   * overlaid on the art.
   */
  @Builder
  appTile(app: NvApp) {
    Stack({ alignContent: Alignment.Center }) {
      // The poster frame: radius / clip / placeholder only. The "连接 / 切换"
      // tip lives here (on the whole tile the star / action tips would also
      // fire, since nested bindTips are not suppressed by the inner one).
      Stack({ alignContent: Alignment.Center }) {
        if (this.iconMap.get(app.id) !== undefined) {
          Image(this.iconMap.get(app.id) as image.PixelMap)
            .width('100%')
            .height('100%')
            .objectFit(ImageFit.Cover)
            .draggable(false)
        } else {
          Image($r('app.media.ic_apps'))
            .width(36)
            .height(36)
            .objectFit(ImageFit.Contain)
            .draggable(false)
        }
      }
      .width('100%')
      .height('100%')
      .borderRadius(TILE_RADIUS)
      .clip(true)
      .backgroundColor($r('app.color.input_bg'))
      .bindTips(this.appTip(app), this.tipsOptions)

      // "运行中" badge at the poster's top-left.
      if (this.isRunning(app)) {
        Column() {
          Text('运行中')
            .fontSize(9)
            .fontColor('#FFFFFF')
            .backgroundColor($r('app.color.success'))
            .borderRadius(4)
            .padding({ left: 5, right: 5, top: 1, bottom: 1 })
        }
        .width('100%')
        .height('100%')
        .padding(6)
        .justifyContent(FlexAlign.Start)
        .alignItems(HorizontalAlign.Start)
        .hitTestBehavior(HitTestMode.None)
      }

      // Default star at the poster's top-right.
      Column() {
        this.starControl(app)
      }
      .width('100%')
      .height('100%')
      .padding(5)
      .justifyContent(FlexAlign.Start)
      .alignItems(HorizontalAlign.End)
      .hitTestBehavior(HitTestMode.None)

      // Bottom-center (running app only): two actions selected by whether a
      // session is already up — [连接 / 断开] then [关闭].
      if (this.isRunning(app)) {
        Column() {
          Row() {
            if (this.hasActiveSession()) {
              this.tileAction($r('app.media.ic_power'), $r('app.color.text_primary'), '断开连接', 8,
                (): void => {
                  this.disconnectSession();
                })

              this.tileAction($r('app.media.ic_stop'), $r('app.color.danger'), '关闭应用并断开', 0,
                (): void => {
                  this.closeAppAndDisconnect();
                })
            } else {
              this.tileAction($r('app.media.ic_play'), $r('app.color.accent'), '连接', 8,
                (): void => {
                  this.openApp(app);
                })

              this.tileAction($r('app.media.ic_stop'), $r('app.color.danger'), '关闭应用', 0,
                (): void => {
                  this.closeApp();
                })
            }
          }
        }
        .width('100%')
        .height('100%')
        .padding({ bottom: 6 })
        .justifyContent(FlexAlign.End)
        .alignItems(HorizontalAlign.Center)
        .hitTestBehavior(HitTestMode.None)
      }
    }
    // Explicit size: the full-size overlay columns below use height('100%'), and
    // without a definite Stack height they would expand the Stack (a Stack sizes
    // to its largest child).
    .width(TILE_WIDTH)
    .height(TILE_HEIGHT)
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
