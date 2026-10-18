import { image } from '@kit.ImageKit';
import { ConnectAction, connectActionLabel } from '../model/Connections';
import { fadeTransition, hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';
import { ControlDisc } from './ControlDisc';

const BOXART_ASPECT: number = 200 / 267;
const TILE_WIDTH: number = 108;
const TILE_HEIGHT: number = TILE_WIDTH / BOXART_ASPECT;
const TILE_RADIUS: number = 14;
const STAR_CONTROL: number = 28;
const STAR_SIZE: number = 18;
const ACTION_CONTROL: number = 36;
const ACTION_SIZE: number = 22;

@Component
export struct MoonlightAppTile {
  icon: image.PixelMap | undefined = undefined;
  @Prop isDefault: boolean = false;
  @Prop isRunning: boolean = false;
  @Prop hasSession: boolean = false;
  @Prop loading: boolean = false;
  @Prop connecting: boolean = false;
  @Prop tip: string = '';
  @State hovered: boolean = false;
  @State pressed: boolean = false;
  onOpen: () => void = () => {};
  onSetDefault: () => void = () => {};
  onDisconnect: () => void = () => {};
  onCloseApp: () => void = () => {};
  onCloseAppAndDisconnect: () => void = () => {};
  private tipsOptions: TipsOptions = {};

  private clickable(): boolean {
    return !this.isRunning && !this.loading;
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
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
            .fillColor($r('app.color.icon_tertiary'))
            .objectFit(ImageFit.Contain)
            .draggable(false)
        }
      }
      .width('100%')
      .height('100%')
      .borderRadius(TILE_RADIUS)
      .clip(true)
      
      .bindTips(this.loading ? '正在连接'
        : (this.isRunning ? '运行中' : this.tip), this.tipsOptions)

      if (this.loading) {
        Column() {
          LoadingProgress()
            .width(32)
            .height(32)
            .color($r('app.color.icon_on_brand'))
        }
        .width('100%')
        .height('100%')
        .borderRadius(TILE_RADIUS)
        .backgroundColor($r('app.color.chrome_scrim'))
        .justifyContent(FlexAlign.Center)
        .alignItems(HorizontalAlign.Center)
        .hitTestBehavior(HitTestMode.None)
      }

      Column() {
        if (this.isRunning) {
          Text('运行中')
            .fontSize(10)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.protocol_moonlight_deep'))
            .backgroundColor($r('app.color.protocol_moonlight_tint'))
            .borderRadius(4)
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
            .transition(fadeTransition())
        }
      }
      .width('100%')
      .height('100%')
      .padding(6)
      .justifyContent(FlexAlign.Start)
      .alignItems(HorizontalAlign.Start)
      .hitTestBehavior(HitTestMode.None)

      Column() {
        ControlDisc({
          icon: $r('app.media.ic_star'),
          diameter: STAR_CONTROL,
          glyph: STAR_SIZE,
          tip: '设为默认',
          iconColor: this.isDefault
            ? $r('app.color.brand_text') : $r('app.color.icon_on_brand'),
          discColor: this.isDefault
            ? $r('app.color.brand_tint') : $r('app.color.chrome_bg'),
          discHoverColor: this.isDefault
            ? $r('app.color.brand_tint_strong') : $r('app.color.chrome_pressed'),
          discPressColor: $r('app.color.chrome_pressed'),
          discBorder: this.isDefault
            ? $r('app.color.brand_tint_strong') : $r('app.color.chrome_border'),
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

      Column() {
        if (this.isRunning) {
          Row() {
            if (this.hasSession) {
              ControlDisc({
                icon: $r('app.media.ic_disconnect'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                gap: 8,
                tip: '断开连接',
                animated: true,
                loading: this.connecting,
                iconColor: $r('app.color.icon_on_brand'),
                discColor: $r('app.color.chrome_bg'),
                discHoverColor: $r('app.color.chrome_pressed'),
                discPressColor: $r('app.color.chrome_pressed'),
                discBorder: $r('app.color.chrome_border'),
                onTap: () => {
                  this.onDisconnect();
                }
              })

              ControlDisc({
                icon: $r('app.media.ic_stop'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                tip: '关闭应用并断开',
                animated: true,
                iconColor: $r('app.color.icon_on_brand'),
                discColor: $r('app.color.chrome_bg'),
                discHoverColor: $r('app.color.chrome_pressed'),
                discPressColor: $r('app.color.chrome_pressed'),
                discBorder: $r('app.color.chrome_border'),
                onTap: () => {
                  this.onCloseAppAndDisconnect();
                }
              })
            } else {
              ControlDisc({
                icon: $r('app.media.ic_play'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                gap: 8,
                tip: connectActionLabel(ConnectAction.Resume),
                animated: true,
                loading: this.connecting,
                iconColor: $r('app.color.icon_on_brand'),
                discColor: $r('app.color.chrome_bg'),
                discHoverColor: $r('app.color.chrome_pressed'),
                discPressColor: $r('app.color.chrome_pressed'),
                discBorder: $r('app.color.chrome_border'),
                onTap: () => {
                  this.onOpen();
                }
              })

              ControlDisc({
                icon: $r('app.media.ic_stop'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                tip: '关闭应用',
                animated: true,
                iconColor: $r('app.color.icon_on_brand'),
                discColor: $r('app.color.chrome_bg'),
                discHoverColor: $r('app.color.chrome_pressed'),
                discPressColor: $r('app.color.chrome_pressed'),
                discBorder: $r('app.color.chrome_border'),
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
    .width(TILE_WIDTH)
    .height(TILE_HEIGHT)
    .margin({ right: 12, bottom: 16 })
    .backgroundColor($r('app.color.surface_subtle'))
    .borderRadius(TILE_RADIUS)
    .border({
      width: this.isRunning ? 2 : 1,
      color: this.isRunning ? $r('app.color.protocol_moonlight')
        : (this.hovered && this.clickable()
          ? $r('app.color.brand_tint_strong') : $r('app.color.border'))
    })
    .shadow({
      radius: this.hovered && this.clickable() ? 16 : 0,
      color: $r('app.color.shadow_2'),
      offsetX: 0,
      offsetY: this.hovered && this.clickable() ? 4 : 0
    })
    .scale(hoverPressScale(this.hovered && this.clickable(), this.pressed && this.clickable()))
    .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
    .onHover((isHover: boolean) => {
      if (!this.clickable()) {
        return;
      }
      this.hovered = isHover;
      if (!isHover) {
        this.pressed = false;
      }
    })
    .onMouse((event: MouseEvent) => {
      if (!this.clickable()) {
        return;
      }
      if (event.action === MouseAction.Press) {
        this.pressed = true;
      } else if (event.action === MouseAction.Release) {
        this.pressed = false;
      }
    })
    .onClick(() => {
      if (this.clickable()) {
        this.onOpen();
      }
    })
    .focusable(this.clickable())
    .focusOnTouch(this.clickable())
  }
}


