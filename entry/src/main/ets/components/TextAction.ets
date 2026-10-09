// ArkUI has no declarative text colour on hover / press, so the colour is
// @State-driven; HoverEffect.Highlight is not used (it draws a background box).
import { MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';

@Component
export struct TextAction {
  @Prop label: string = '';
  @Prop muted: boolean = false;
  onTap: () => void = () => {};

  @State hovered: boolean = false;
  @State pressed: boolean = false;

  private color(): ResourceColor {
    if (this.muted) {
      if (this.pressed) {
        return $r('app.color.text_primary');
      }
      return this.hovered ? $r('app.color.text_secondary') : $r('app.color.text_tertiary');
    }
    if (this.pressed) {
      return $r('app.color.accent_press');
    }
    return this.hovered ? $r('app.color.accent_hover') : $r('app.color.accent');
  }

  build() {
    Text(this.label)
      .fontSize(this.muted ? 12 : 14)
      .fontColor(this.color())
      .padding(this.muted
        ? { left: 8, right: 8, top: 6, bottom: 6 }
        : { top: 8, bottom: 8 })
      .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
      .hoverEffect(HoverEffect.None)
      .onHover((isHover: boolean) => {
        this.hovered = isHover;
        if (!isHover) {
          this.pressed = false;
        }
      })
      .onMouse((event: MouseEvent) => {
        if (event.action === MouseAction.Press) {
          this.pressed = true;
        } else if (event.action === MouseAction.Release) {
          this.pressed = false;
        }
      })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          this.pressed = true;
        } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          this.pressed = false;
        }
      })
      .onClick(() => {
        this.onTap();
      })
  }
}
