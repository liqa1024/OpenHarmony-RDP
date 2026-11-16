import { image } from '@kit.ImageKit';
import { ConnectAction, connectActionLabel } from '../model/Connections';
import { fadeTransition, hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';
import { ControlDisc } from './ControlDisc';
import { MaterialSupport } from '../utils/MaterialSupport';

const BOXART_ASPECT: number = 200 / 267;
export const TILE_WIDTH: number = 108;
const TILE_HEIGHT: number = TILE_WIDTH / BOXART_ASPECT;
const TILE_RADIUS: number = 14;
const STAR_CONTROL: number = 28;
const STAR_SIZE: number = 18;
const ACTION_CONTROL: number = 36;
const ACTION_SIZE: number = 22;
// 碟片摆放常量（星标右上、动作行底部居中），与原来「整层 Column + padding + 对齐」等价。
const STAR_INSET: number = 5;
const ACTION_INSET: number = 6;
const ACTION_GAP: number = 8;
const ACTION_ROW_WIDTH: number = ACTION_CONTROL * 2 + ACTION_GAP;
// 碟片的响应热区（`ControlDisc` 的 responseRegion 固定 44×44，比可视圆大一圈）。碟片所在的那一层把
// **响应热区**收到这一块：`HitTestMode.Default` 只阻塞兄弟节点、不影响祖先，所以碟片独占这一块的同时，
// 祖先 `Scroll` 照旧拿到轴事件（滚轮 / 触控板滚动不会被吞）。
const DISC_HIT: number = 44;
const STAR_ZONE_X: number = TILE_WIDTH - STAR_INSET - STAR_CONTROL - (DISC_HIT - STAR_CONTROL) / 2;
const STAR_ZONE_Y: number = STAR_INSET - (DISC_HIT - STAR_CONTROL) / 2;
const ACTION_ZONE_WIDTH: number = ACTION_ROW_WIDTH + (DISC_HIT - ACTION_CONTROL);
const ACTION_ZONE_X: number = (TILE_WIDTH - ACTION_ROW_WIDTH) / 2 - (DISC_HIT - ACTION_CONTROL) / 2;
const ACTION_ZONE_Y: number =
  TILE_HEIGHT - ACTION_INSET - ACTION_CONTROL - (DISC_HIT - ACTION_CONTROL) / 2;

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

  private clickable(): boolean {
    return !this.isRunning && !this.loading;
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      // 卡片自己的点击 / 悬停 / 提示面**单独成层、放在最下**（不是碟片的祖先）：这样碟片那两层与它是
      // **兄弟**关系，"默认模式阻塞兄弟、不影响祖先"就够用——碟片独占自己的热区、卡片其余区域照常响应，
      // 而祖先 `Scroll` 全程不被排除，滚动不会被吞。加了 `BLOCK_HIERARCHY` 就会连祖先一起断掉
      // （见 doc_agent/ui-feedback.md §4.7）。
      Column()
        .width('100%')
        .height('100%')
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
        .onTouch((event: TouchEvent) => {
          if (!this.clickable()) {
            return;
          }
          if (event.type === TouchType.Down) {
            this.pressed = true;
          } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
            this.pressed = false;
          }
        })
        .bindTips(this.loading ? '正在连接'
          : (this.isRunning ? '运行中' : this.tip), MaterialSupport.tipsOptions())
        .onClick(() => {
          if (this.clickable()) {
            this.onOpen();
          }
        })
        .focusable(this.clickable())
        .focusOnTouch(this.clickable())

      Stack({ alignContent: Alignment.Center }) {
        if (this.icon !== undefined) {
          Image(this.icon)
            .width('100%')
            .height('100%')
            .objectFit(ImageFit.Cover)
            .draggable(false)
            .hitTestBehavior(HitTestMode.None)
        } else {
          Image($r('app.media.ic_apps'))
            .width(36)
            .height(36)
            .fillColor($r('app.color.icon_tertiary'))
            .objectFit(ImageFit.Contain)
            .draggable(false)
            .hitTestBehavior(HitTestMode.None)
        }
      }
      .width('100%')
      .height('100%')
      .borderRadius(TILE_RADIUS)
      .clip(true)
      .hitTestBehavior(HitTestMode.None)

      if (this.loading) {
        Column() {
          LoadingProgress()
            .width(32)
            .height(32)
            .color($r('app.color.icon_on_brand'))
            .hitTestBehavior(HitTestMode.None)
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
            .hitTestBehavior(HitTestMode.None)
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
          frosted: true,
          iconColor: this.isDefault
            ? $r('app.color.brand_text') : $r('app.color.tile_glass_icon'),
          discHoverColor: $r('app.color.tile_glass_hover'),
          discPressColor: $r('app.color.tile_glass_pressed'),
          discBorder: this.isDefault
            ? $r('app.color.brand_tint_strong') : $r('app.color.tile_glass_border'),
          onTap: () => {
            this.onSetDefault();
          }
        })
      }
      .width('100%')
      .height('100%')
      .padding(STAR_INSET)
      .justifyContent(FlexAlign.Start)
      .alignItems(HorizontalAlign.End)
      // 星标那一层只在自己的响应热区里参与命中：热区内它（默认模式）阻塞下方的卡片点击面，热区外它不在
      // 命中结果里，卡片点得到。祖先不受影响，滚动照旧。
      .hitTestBehavior(HitTestMode.Default)
      .responseRegion({
        x: STAR_ZONE_X,
        y: STAR_ZONE_Y,
        width: DISC_HIT,
        height: DISC_HIT
      })

      Column() {
        if (this.isRunning) {
          Row() {
            if (this.hasSession) {
              ControlDisc({
                icon: $r('app.media.ic_disconnect'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                gap: ACTION_GAP,
                tip: '断开连接',
                animated: true,
                frosted: true,
                loading: this.connecting,
                iconColor: $r('app.color.tile_glass_icon'),
                discHoverColor: $r('app.color.tile_glass_hover'),
                discPressColor: $r('app.color.tile_glass_pressed'),
                discBorder: $r('app.color.tile_glass_border'),
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
                frosted: true,
                iconColor: $r('app.color.tile_glass_icon'),
                discHoverColor: $r('app.color.tile_glass_hover'),
                discPressColor: $r('app.color.tile_glass_pressed'),
                discBorder: $r('app.color.tile_glass_border'),
                onTap: () => {
                  this.onCloseAppAndDisconnect();
                }
              })
            } else {
              ControlDisc({
                icon: $r('app.media.ic_play'),
                diameter: ACTION_CONTROL,
                glyph: ACTION_SIZE,
                gap: ACTION_GAP,
                tip: connectActionLabel(ConnectAction.Resume),
                animated: true,
                frosted: true,
                loading: this.connecting,
                iconColor: $r('app.color.tile_glass_icon'),
                discHoverColor: $r('app.color.tile_glass_hover'),
                discPressColor: $r('app.color.tile_glass_pressed'),
                discBorder: $r('app.color.tile_glass_border'),
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
                frosted: true,
                iconColor: $r('app.color.tile_glass_icon'),
                discHoverColor: $r('app.color.tile_glass_hover'),
                discPressColor: $r('app.color.tile_glass_pressed'),
                discBorder: $r('app.color.tile_glass_border'),
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
      .padding({ bottom: ACTION_INSET })
      .justifyContent(FlexAlign.End)
      .alignItems(HorizontalAlign.Center)
      // 同星标一层：只在碟片热区内阻塞下方的卡片点击面。
      .hitTestBehavior(HitTestMode.Default)
      .responseRegion({
        x: ACTION_ZONE_X,
        y: ACTION_ZONE_Y,
        width: ACTION_ZONE_WIDTH,
        height: DISC_HIT
      })
    }
    .width(TILE_WIDTH)
    .height(TILE_HEIGHT)
    .margin({ bottom: 16 })
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
  }
}


