/*
 * HmRdp - round icon control laid over a surface (e.g. a Moonlight app poster).
 *
 * A single reusable control: an icon on a round THIN immersive-material disc
 * (flat frosted where the system has no material), with cursor-hover and press
 * feedback and an appear / disappear transition. It is a component, not a page
 * @Builder, so each disc keeps its own hover / press state (several discs share
 * one tile) and its inputs refresh in place through @Prop.
 *
 * Motion comes from utils/UiMotion so it matches the other controls; the blur
 * fallback lives in the material modifier so it is never set alongside the
 * material (it would sit above and hide it) — see doc_agent/arkts-conventions.md
 * §6.
 */
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS, popTransition } from '../utils/UiMotion';

@Component
export struct ControlDisc {
  @Prop icon: Resource = $r('app.media.ic_play');
  @Prop iconColor: ResourceColor = $r('app.color.text_primary');
  @Prop diameter: number = 36;
  @Prop glyph: number = 22;
  @Prop gap: number = 0;
  @Prop tip: string = '';
  // In-flight: the icon is replaced by a spinner and taps are ignored, so the
  // control reports its own busy state (the caller decides when it is loading).
  @Prop loading: boolean = false;
  // Whether the disc plays an appear / disappear transition when it is inserted
  // or removed by the caller. Opt-in: an always-present disc (e.g. the poster's
  // star) must not pop again when its tile is recreated for another reason.
  @Prop animated: boolean = false;
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private controlMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.CONTROL),
      BlurStyle.COMPONENT_THIN);

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
    // Explicit transparent background: the material is the surface (an opaque
    // background would cover it).
    .backgroundColor(Color.Transparent)
    .attributeModifier(this.controlMaterial)
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .margin({ right: this.gap })
    .scale(hoverPressScale(this.hovered && !this.loading, this.pressed && !this.loading))
    .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
    .transition(this.animated ? popTransition() : TransitionEffect.IDENTITY)
    // Keep a tap on the disc off the poster underneath, so the tile's own click
    // does not also fire.
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
  }
}
