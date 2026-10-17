const PROBE_WIDTH: number = 180;
const PROBE_HEIGHT: number = 120;
const PROBE_DOT: number = 10;

@Component
export struct PadProbeOverlay {
  @Prop p1x: number = 0.5;
  @Prop p1y: number = 0.5;
  @Prop p2x: number = 0.5;
  @Prop p2y: number = 0.5;
  onHoverChange: (hover: boolean) => void = (): void => {};

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      Column()
        .width('100%')
        .height('100%')
        
        
      Column()
        .width(PROBE_DOT)
        .height(PROBE_DOT)
        .borderRadius(PROBE_DOT / 2)
        
        .position({ x: this.p1x * PROBE_WIDTH - PROBE_DOT / 2,
          y: this.p1y * PROBE_HEIGHT - PROBE_DOT / 2 })
      Column()
        .width(PROBE_DOT)
        .height(PROBE_DOT)
        .borderRadius(PROBE_DOT / 2)
        
        .position({ x: this.p2x * PROBE_WIDTH - PROBE_DOT / 2,
          y: this.p2y * PROBE_HEIGHT - PROBE_DOT / 2 })
    }
    .width(PROBE_WIDTH)
    .height(PROBE_HEIGHT)
    .hitTestBehavior(HitTestMode.Transparent)
    .onHover((isHover: boolean) => {
      this.onHoverChange(isHover);
    })
  }
}


