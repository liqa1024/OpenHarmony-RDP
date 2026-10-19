import { hoverPressScale, MOTION_CURVE, MOTION_HOVER_PRESS_MS } from '../utils/UiMotion';
import { MaterialSupport } from '../utils/MaterialSupport';

@Component
export struct FabButton {
  @Prop icon: Resource = $r('app.media.ic_plus');
  @Prop tip: string = '';
  @Prop loading: boolean = false;
  @Prop primary: boolean = true;
  @Prop inset: number = 0;
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;
  private tipsOptions: TipsOptions = MaterialSupport.tipsOptions();

  private fabBg(): Resource {
    if (this.primary) {
      return this.pressed ? $r('app.color.fab_primary_bg_pressed')
        : (this.hovered ? $r('app.color.fab_primary_bg_hover') : $r('app.color.fab_primary_bg'));
    }
    return this.pressed ? $r('app.color.control_bg_pressed')
      : (this.hovered ? $r('app.color.control_bg_hover') : $r('app.color.fab_secondary_bg'));
  }

  private fabBorder(): Resource {
    return this.primary ? $r('app.color.brand_tint_strong') : $r('app.color.control_border');
  }

  private fabIcon(): Resource {
    return this.primary ? $r('app.color.brand_text') : $r('app.color.icon_primary');
  }

  private fabShadowColor(): Resource {
    return this.primary ? $r('app.color.shadow_fab') : $r('app.color.shadow_2');
  }

  private fabShadowRadius(): number {
    return this.primary ? 24 : 16;
  }

  private fabShadowOffset(): number {
    return this.primary ? 8 : 4;
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      if (this.loading) {
        LoadingProgress()
          .width(24)
          .height(24)
          .color(this.fabIcon())
          .hitTestBehavior(HitTestMode.None)
      } else {
        Image(this.icon)
          .width(24)
          .height(24)
          .fillColor(this.fabIcon())
          .draggable(false)
          .hitTestBehavior(HitTestMode.None)
      }
    }
    .width(56)
    .height(56)
    .backgroundColor(this.fabBg())
    .border({ width: 1, color: this.fabBorder() })
    .borderRadius(28)
    .margin({ right: this.inset, bottom: this.inset })
    .shadow({
      radius: this.fabShadowRadius(),
      color: this.fabShadowColor(),
      offsetX: 0,
      offsetY: this.fabShadowOffset()
    })
    .opacity(this.loading ? 0.8 : 1)
    .scale(hoverPressScale(this.hovered, this.pressed))
    .animation({ duration: MOTION_HOVER_PRESS_MS, curve: MOTION_CURVE })
    .hitTestBehavior(HitTestMode.BLOCK_HIERARCHY)
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
    .bindTips(this.loading ? '处理中…' : this.tip, this.tipsOptions)
    .onClick(() => {
      if (!this.loading) {
        this.onTap();
      }
    })
    .focusable(!this.loading)
    .focusOnTouch(!this.loading)
  }
}
