/*
 * HmRdp - one application tile on the Moonlight apps page.
 *
 * The poster is the tile (no card / frame wrapper): the name lives in the hover
 * tip, the set-as-default star sits at the top-right, the "运行中" badge at the
 * top-left and the running app's actions at the bottom-centre, all over the art
 * on a round immersive-material control (components/ControlDisc, which owns the
 * hover / press / appear / disappear motion).
 *
 * It is a component, not a page @Builder, so the reactive flags (@Prop) really
 * refresh inside the page's ForEach: a @Builder with by-value params never
 * re-renders, and ForEach reuses an item whose key is unchanged
 * (doc_agent/arkts-conventions.md §2). The icon cannot be a @Prop — its PixelMap
 * does not survive @Prop's deep copy — so it is a plain field and the caller
 * bumps the ForEach key once the bitmap has loaded.
 */
import { image } from '@kit.ImageKit';
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { NvApp } from '../moonlight/MoonlightModels';
import { ConnectAction, connectActionLabel } from '../utils/ConnectAction';
import { fadeTransition, hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';
import { ControlDisc } from './ControlDisc';

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

@Component
export struct MoonlightAppTile {
  app: NvApp = new NvApp();
  icon: image.PixelMap | undefined = undefined;
  @Prop isDefault: boolean = false;
  @Prop isRunning: boolean = false;
  @Prop hasSession: boolean = false;
  @Prop tip: string = '';
  // The card's own hover / press state. A running app's card is not clickable
  // (it acts through its discs), so it never reacts — the handlers below are
  // gated on !isRunning and the scale falls back to 1 while running.
  @State hovered: boolean = false;
  @State pressed: boolean = false;
  onOpen: () => void = () => {};
  onSetDefault: () => void = () => {};
  onDisconnect: () => void = () => {};
  onCloseApp: () => void = () => {};
  onCloseAppAndDisconnect: () => void = () => {};
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };

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

      // "运行中" badge at the poster's top-left. The @Builder-free Text carries
      // the appear / disappear transition, so a running app's badge pops in when
      // it starts and fades out when it stops.
      Column() {
        if (this.isRunning) {
          Text('运行中')
            .fontSize(9)
            .fontColor('#FFFFFF')
            .backgroundColor($r('app.color.success'))
            .borderRadius(4)
            .padding({ left: 5, right: 5, top: 1, bottom: 1 })
            .transition(fadeTransition())
        }
      }
      .width('100%')
      .height('100%')
      .padding(6)
      .justifyContent(FlexAlign.Start)
      .alignItems(HorizontalAlign.Start)
      .hitTestBehavior(HitTestMode.None)

      // Default star at the poster's top-right. Always present, so its @Prop
      // colour refreshes in place (the by-value @Builder that used to freeze it
      // is gone).
      Column() {
        ControlDisc({
          icon: $r('app.media.ic_star'),
          iconColor: this.isDefault ? $r('app.color.accent') : $r('app.color.text_primary'),
          diameter: STAR_CONTROL,
          glyph: STAR_SIZE,
          tip: '设为默认',
          onTap: () => {
            this.onSetDefault();
          }
        })
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
      // it again. The row fades as a whole, and each disc pops in its own
      // transition; switching session state swaps the first disc in place.
      Column() {
        if (this.isRunning) {
          Row() {
            if (this.hasSession) {
              ControlDisc({
                icon: $r('app.media.ic_disconnect'),
                iconColor: $r('app.color.text_primary'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                gap: 8,
                tip: '断开连接',
                animated: true,
                onTap: () => {
                  this.onDisconnect();
                }
              })

              ControlDisc({
                icon: $r('app.media.ic_stop'),
                iconColor: $r('app.color.danger'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                tip: '关闭应用并断开',
                animated: true,
                onTap: () => {
                  this.onCloseAppAndDisconnect();
                }
              })
            } else {
              ControlDisc({
                icon: $r('app.media.ic_play'),
                iconColor: $r('app.color.accent'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                gap: 8,
                tip: connectActionLabel(ConnectAction.Resume),
                animated: true,
                onTap: () => {
                  this.onOpen();
                }
              })

              ControlDisc({
                icon: $r('app.media.ic_stop'),
                iconColor: $r('app.color.danger'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                tip: '关闭应用',
                animated: true,
                onTap: () => {
                  this.onCloseApp();
                }
              })
            }
          }
          .transition(fadeTransition())
        }
      }
      .width('100%')
      .height('100%')
      .padding({ bottom: 6 })
      .justifyContent(FlexAlign.End)
      .alignItems(HorizontalAlign.Center)
      .hitTestBehavior(HitTestMode.None)
    }
    // Explicit size: the full-size overlay columns use height('100%'), and without
    // a definite Stack height they would expand the Stack (a Stack sizes to its
    // largest child).
    .width(TILE_WIDTH)
    .height(TILE_HEIGHT)
    .margin({ right: 12, bottom: 16 })
    // The card itself gets the same hover / press feedback as its controls, but
    // only while it is clickable: a running app acts through its discs, so its
    // card must not react to hover or press (see doc_agent/moonlight-connection.md §4).
    .scale(hoverPressScale(this.hovered && !this.isRunning, this.pressed && !this.isRunning))
    .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
    .onHover((isHover: boolean) => {
      if (this.isRunning) {
        return;
      }
      this.hovered = isHover;
      if (!isHover) {
        this.pressed = false;
      }
    })
    .onMouse((event: MouseEvent) => {
      if (this.isRunning) {
        return;
      }
      if (event.action === MouseAction.Press) {
        this.pressed = true;
      } else if (event.action === MouseAction.Release) {
        this.pressed = false;
      }
    })
    // A running app acts through its own discs; its card must not also react to a
    // tap (and its tip is empty, so it has no tooltip either).
    .onClick(() => {
      if (!this.isRunning) {
        this.onOpen();
      }
    })
  }
}
