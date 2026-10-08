/*
 * HmRdp - floating circular action button.
 *
 * The bottom-right round action (new connection, refresh, ...): accent circle,
 * shared hover / press feedback and an OVERLAY-material tooltip. The caller
 * places it (e.g. via .margin) and owns the action, so the same control is used
 * wherever a page needs a floating primary action. While `loading` is set the
 * icon itself is replaced by a spinner and taps are ignored, leaving the page
 * behind it untouched.
 */
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';

@Component
export struct FabButton {
  @Prop icon: Resource = $r('app.media.ic_plus');
  @Prop tip: string = '';
  @Prop loading: boolean = false;
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };

  build() {
    Row() {
      if (this.loading) {
        LoadingProgress()
          .width(24)
          .height(24)
          .color('#FFFFFF')
      } else {
        Image(this.icon)
          .width(24)
          .height(24)
          .draggable(false)
      }
    }
    .width(56)
    .height(56)
    .borderRadius(28)
    .backgroundColor($r('app.color.accent'))
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .scale(hoverPressScale(this.hovered, this.pressed))
    .shadow({ radius: 18, color: '#33000000', offsetX: 0, offsetY: 6 })
    .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
    .onHover((isHover: boolean) => {
      this.hovered = isHover;
    })
    .onMouse((event: MouseEvent) => {
      if (event.action === MouseAction.Press) {
        this.pressed = true;
      } else if (event.action === MouseAction.Release) {
        this.pressed = false;
      }
    })
    .bindTips(this.loading ? '' : this.tip, this.tipsOptions)
    .onClick(() => {
      if (!this.loading) {
        this.onTap();
      }
    })
  }
}
