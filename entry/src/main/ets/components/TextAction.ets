// ArkUI has no declarative text colour on hover / press, so the colour is
// @State-driven; HoverEffect.Highlight is not used (it draws a background box).
import { FrostedModifier, FrostedSurface } from '../services/SystemMaterial';
import { MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';

@Component
export struct TextAction {
  @Prop label: string = '';
  @Prop muted: boolean = false;
  onTap: () => void = () => {};

  @State hovered: boolean = false;
  @State pressed: boolean = false;
  private actionFrost: AttributeModifier<TextAttribute> =
    new FrostedModifier<TextAttribute>(FrostedSurface.CONTROL);

  private color(): ResourceColor {
    if (this.muted) {
      if (this.pressed || this.hovered) {
        return $r('app.color.text_primary');
      }
      return $r('app.color.text_secondary');
    }
    if (this.pressed) {
      return $r('app.color.control_accent_press');
    }
    return this.hovered ? $r('app.color.control_accent_hover') : $r('app.color.control_accent');
  }

  build() {
    Text(this.label)
      .fontSize(this.muted ? 12 : 14)
      .fontColor(this.color())
      .padding(this.muted
        ? { left: 10, right: 10, top: 6, bottom: 6 }
        : { left: 12, right: 12, top: 8, bottom: 8 })
      .borderRadius(8)
      .backgroundColor($r('app.color.control_bg'))
      .attributeModifier(this.actionFrost)
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
