const PROBE_WIDTH: number = 180;
const PROBE_HEIGHT: number = 120;
const PROBE_DOT: number = 10;

// Debug overlay: draws the synthesized pinch contacts (normalized to their channel
// surface) on a small virtual touchpad so the simulation can be inspected visually.
@Component
export struct PinchProbeOverlay {
  @Prop active: boolean = false;
  @Prop p1x: number = 0.5;
  @Prop p1y: number = 0.5;
  @Prop p2x: number = 0.5;
  @Prop p2y: number = 0.5;

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor('#66000000')
        .border({ width: 1, color: '#99FFFFFF', radius: 6 })
      Column()
        .width(PROBE_DOT)
        .height(PROBE_DOT)
        .borderRadius(PROBE_DOT / 2)
        .backgroundColor('#FFF44336')
        .position({ x: this.p1x * PROBE_WIDTH - PROBE_DOT / 2,
          y: this.p1y * PROBE_HEIGHT - PROBE_DOT / 2 })
      Column()
        .width(PROBE_DOT)
        .height(PROBE_DOT)
        .borderRadius(PROBE_DOT / 2)
        .backgroundColor('#FF2196F3')
        .position({ x: this.p2x * PROBE_WIDTH - PROBE_DOT / 2,
          y: this.p2y * PROBE_HEIGHT - PROBE_DOT / 2 })
    }
    .width(PROBE_WIDTH)
    .height(PROBE_HEIGHT)
    .opacity(this.active ? 1 : 0.35)
  }
}
