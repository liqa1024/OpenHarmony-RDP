import { hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';

@Component
export struct FabButton {
  @Prop icon: Resource = $r('app.media.ic_plus');
  @Prop tip: string = '';
  @Prop loading: boolean = false;
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;
  private tipsOptions: TipsOptions = {};

  build() {
    Row() {
      if (this.loading) {
        LoadingProgress()
          .width(24)
          .height(24)
          
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
    
    .justifyContent(FlexAlign.Center)
    .alignItems(VerticalAlign.Center)
    .scale(hoverPressScale(this.hovered, this.pressed))
    
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


