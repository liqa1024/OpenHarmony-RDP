/*
 * HmRdp - one application tile on the Moonlight apps page.
 *
 * The poster is the tile (no card / frame wrapper): the name lives in the hover
 * tip, the set-as-default star sits at the top-right, the "运行中" badge at the
 * top-left and the running app's actions at the bottom-centre, all over the art
 * on a round immersive-material control.
 *
 * It is a component, not a page @Builder, so the reactive flags (@Prop) really
 * refresh inside the page's ForEach: a @Builder with by-value params never
 * re-renders, and ForEach reuses an item whose key is unchanged
 * (doc_agent/arkts-conventions.md §2). The icon cannot be a @Prop — its PixelMap
 * does not survive @Prop's deep copy — so it is a plain field and the caller
 * bumps the ForEach key once the bitmap has loaded.
 */
import { image } from '@kit.ImageKit';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { NvApp } from '../moonlight/MoonlightModels';
import { ConnectAction, connectActionLabel } from '../utils/ConnectAction';

// Moonlight's box art is 200x267 (see moonlight-qt AppView.qml): the tile keeps
// that width:height so a fetched poster is not cropped by a wrong frame.
const BOXART_ASPECT: number = 200 / 267;
const TILE_WIDTH: number = 108;
const TILE_HEIGHT: number = TILE_WIDTH / BOXART_ASPECT;
const TILE_RADIUS: number = 14;
// On-poster controls sit on a THIN immersive-material disc (fallback: a flat
// frosted component style). The star's disc stays small; the running app's
// action discs are bigger so they are easier to hit.
const STAR_CONTROL: number = 28;
const STAR_SIZE: number = 18;
const ACTION_CONTROL: number = 36;
const ACTION_SIZE: number = 22;

/**
 * A control disc's parameters. Passed as a single object literal so the builder
 * gets it *by reference*: a @Builder with by-value params never re-renders, even
 * when the component around it does (official @Builder FAQ) — which is exactly
 * what froze the default star's colour. See doc_agent/arkts-conventions.md §2.
 */
interface ControlDiscOptions {
  icon: Resource;
  color: ResourceColor;
  diameter: number;
  glyph: number;
  gap: number;
  tip: string;
  action: () => void;
}

@Component
export struct MoonlightAppTile {
  app: NvApp = new NvApp();
  icon: image.PixelMap | undefined = undefined;
  @Prop isDefault: boolean = false;
  @Prop isRunning: boolean = false;
  @Prop hasSession: boolean = false;
  @Prop tip: string = '';
  onOpen: () => void = () => {};
  onSetDefault: () => void = () => {};
  onDisconnect: () => void = () => {};
  onCloseApp: () => void = () => {};
  onCloseAppAndDisconnect: () => void = () => {};

  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private controlMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.CONTROL),
      BlurStyle.COMPONENT_THIN);

  /**
   * A round on-poster control: a THIN immersive-material disc, flat frosted where
   * the system has no material. BLOCK_HIERARCHY keeps a tap off the poster
   * underneath, so the tile's own click does not also fire.
   */
  @Builder
  controlDisc(options: ControlDiscOptions) {
    Row() {
      Image(options.icon)
        .width(options.glyph)
        .height(options.glyph)
        .fillColor(options.color)
        .draggable(false)
    }
    .width(options.diameter)
    .height(options.diameter)
    .borderRadius(options.diameter / 2)
    // Explicit transparent background, like the connection rows: the material is
    // the surface (an opaque background would cover it).
    .backgroundColor(Color.Transparent)
    // Material when the system has it, otherwise a component-level frosted style
    // — both live in the modifier so the fallback blur is never set alongside
    // the material (it would sit above and hide it).
    .attributeModifier(this.controlMaterial)
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .margin({ right: options.gap })
    .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
    .bindTips(options.tip, this.tipsOptions)
    .onClick(() => {
      options.action();
    })
  }

  /** One icon action overlaid on the running app's poster. */
  @Builder
  tileAction(icon: Resource, color: ResourceColor, tip: string, gap: number, action: () => void) {
    this.controlDisc({
      icon: icon,
      color: color,
      diameter: ACTION_CONTROL,
      glyph: ACTION_SIZE,
      gap: gap,
      tip: tip,
      action: action
    })
  }

  /** The set-as-default star, top-right on the poster. */
  @Builder
  starControl() {
    this.controlDisc({
      icon: $r('app.media.ic_star'),
      color: this.isDefault ? $r('app.color.accent') : $r('app.color.text_primary'),
      diameter: STAR_CONTROL,
      glyph: STAR_SIZE,
      gap: 0,
      tip: '设为默认',
      action: (): void => {
        this.onSetDefault();
      }
    })
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      // The poster frame: radius / clip / placeholder only. The "连接 / 切换"
      // tip lives here (on the whole tile the star / action tips would also fire,
      // since nested bindTips are not suppressed by the inner one).
      Stack({ alignContent: Alignment.Center }) {
        if (this.icon !== undefined) {
          Image(this.icon)
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
      .bindTips(this.tip, this.tipsOptions)

      // "运行中" badge at the poster's top-left.
      if (this.isRunning) {
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
        this.starControl()
      }
      .width('100%')
      .height('100%')
      .padding(5)
      .justifyContent(FlexAlign.Start)
      .alignItems(HorizontalAlign.End)
      .hitTestBehavior(HitTestMode.None)

      // Bottom-center (running app only): two actions selected by whether a
      // session is already up — [恢复 / 断开] then [关闭]. The app is already
      // running on the host, so 恢复 resumes it (`/resume`) instead of starting
      // it again.
      if (this.isRunning) {
        Column() {
          Row() {
            if (this.hasSession) {
              this.tileAction($r('app.media.ic_power'), $r('app.color.text_primary'), '断开连接', 8,
                (): void => {
                  this.onDisconnect();
                })

              this.tileAction($r('app.media.ic_stop'), $r('app.color.danger'), '关闭应用并断开', 0,
                (): void => {
                  this.onCloseAppAndDisconnect();
                })
            } else {
              this.tileAction($r('app.media.ic_play'), $r('app.color.accent'),
                connectActionLabel(ConnectAction.Resume), 8,
                (): void => {
                  this.onOpen();
                })

              this.tileAction($r('app.media.ic_stop'), $r('app.color.danger'), '关闭应用', 0,
                (): void => {
                  this.onCloseApp();
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
    // Explicit size: the full-size overlay columns use height('100%'), and without
    // a definite Stack height they would expand the Stack (a Stack sizes to its
    // largest child).
    .width(TILE_WIDTH)
    .height(TILE_HEIGHT)
    .margin({ right: 12, bottom: 16 })
    // A running app acts through its own discs; its card must not also react to a
    // tap (and its tip is empty, so it has no tooltip either).
    .onClick(() => {
      if (!this.isRunning) {
        this.onOpen();
      }
    })
  }
}
