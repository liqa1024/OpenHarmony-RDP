
import { hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS, popTransition } from '../utils/UiMotion';

@Component
export struct ControlDisc {
  @Prop icon: Resource = $r('app.media.ic_play');
  @Prop diameter: number = 36;
  @Prop glyph: number = 22;
  @Prop gap: number = 0;
  @Prop tip: string = '';
  @Prop loading: boolean = false;
  @Prop animated: boolean = false;
  @Prop iconColor: Resource = $r('app.color.icon_secondary');
  @Prop discColor: Resource = $r('app.color.transparent');
  @Prop discHoverColor: Resource = $r('app.color.hover');
  @Prop discPressColor: Resource = $r('app.color.pressed');
  @Prop discBorder: Resource = $r('app.color.transparent');
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;
  private tipsOptions: TipsOptions = {};

  build() {
    Row() {
      if (this.loading) {
        LoadingProgress()
          .width(this.glyph)
          .height(this.glyph)
          .color(this.iconColor)
      } else {
        Image(this.icon)
          .width(this.glyph)
          .height(this.glyph)
          .fillColor(this.iconColor)
          .draggable(false)
      }
    }
    .width(this.diameter)
    .height(this.diameter)
    .borderRadius(this.diameter / 2)
    .backgroundColor(this.pressed ? this.discPressColor
      : (this.hovered ? this.discHoverColor : this.discColor))
    .border({ width: 1, color: this.discBorder })
    .responseRegion({
      x: (this.diameter - 44) / 2,
      y: (this.diameter - 44) / 2,
      width: 44,
      height: 44
    })
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .margin({ right: this.gap })
    .scale(hoverPressScale(this.hovered && !this.loading, this.pressed && !this.loading))
    .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
    .transition(this.animated ? popTransition() : TransitionEffect.IDENTITY)
    .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
    .onHover((isHover: boolean) => {
      if (this.loading) {
        return;
      }
      this.hovered = isHover;
      if (!isHover) {
        this.pressed = false;
      }
    })
    .onMouse((event: MouseEvent) => {
      if (this.loading) {
        return;
      }
      if (event.action === MouseAction.Press) {
        this.pressed = true;
      } else if (event.action === MouseAction.Release) {
        this.pressed = false;
      }
    })
    .onTouch((event: TouchEvent) => {
      if (this.loading) {
        return;
      }
      if (event.type === TouchType.Down) {
        this.pressed = true;
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        this.pressed = false;
      }
    })
    .bindTips(this.loading ? '' : this.tip, this.tipsOptions)
    .onClick(() => {
      if (!this.loading) {
        this.onTap();
      }
    })
    .focusable(!this.loading)
    .focusOnTouch(!this.loading)
  }
}


