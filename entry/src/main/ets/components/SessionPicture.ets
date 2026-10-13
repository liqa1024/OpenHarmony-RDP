import { lengthToNumber } from '../utils/UiUtil';
import { fitPictureRect } from '../utils/PictureFit';

// The overlay pins its offset (the fake content stays put); the scrollable range spans both sides so
// the framework can report each frame's delta without clamping it.
const ENGINE_FRAME_HEADROOM_VP: number = 2000;

// The session picture surface: a SURFACE XComponent centred at the content aspect
// ratio; input stays on the XComponent, so its coordinates are picture-local.
@Component
export struct SessionPicture {
  xComponentId: string = '';
  pictureController: XComponentController = new XComponentController();
  @Prop @Watch('refit') contentWidth: number = 0;
  @Prop @Watch('refit') contentHeight: number = 0;
  @Prop revealBand: number = 0;
  // Percent of the picture width the reveal band spans, centred; the ends stay
  // inert so a pointer brushing an edge or corner does not arm the toolbar.
  @Prop revealBandWidth: number = 100;
  // System scroll engine: the transparent overlay below owns touchpad/wheel scroll inertia.
  engineScroll: boolean = false;
  onPointerMouse: (event: MouseEvent) => void = (event: MouseEvent) => {};
  onPointerTouch: (event: TouchEvent) => void = (event: TouchEvent) => {};
  onPointerHover: (isHover: boolean) => void = (isHover: boolean) => {};
  onPointerAxis: (event: AxisEvent) => void = (event: AxisEvent) => {};
  onSurfaceKey: (event: KeyEvent) => void = (event: KeyEvent) => {};
  onSurfaceBlur: () => void = () => {};
  onPictureArea: (width: number, height: number) => void = (width: number, height: number) => {};
  onRevealZone: (inBand: boolean) => void = (inBand: boolean) => {};
  onEngineScroll: (vertical: number, fromTouchpad: boolean) => void =
    (vertical: number, fromTouchpad: boolean) => {};
  private containerWidth: number = 0;
  private containerHeight: number = 0;
  @State private pictureWidth: number = 0;
  @State private pictureHeight: number = 0;

  private refit(): void {
    if (this.containerWidth <= 0 || this.containerHeight <= 0) {
      return;
    }
    const rect: number[] = fitPictureRect(this.containerWidth, this.containerHeight,
      this.contentWidth, this.contentHeight);
    if (rect[2] === this.pictureWidth && rect[3] === this.pictureHeight) {
      return;
    }
    this.pictureWidth = rect[2];
    this.pictureHeight = rect[3];
    this.onPictureArea(rect[2], rect[3]);
  }

  build() {
    Stack({ alignContent: Alignment.Top }) {
      Stack({ alignContent: Alignment.Center }) {
        if (this.pictureWidth > 0 && this.pictureHeight > 0) {
          XComponent({
            id: this.xComponentId,
            type: XComponentType.SURFACE,
            controller: this.pictureController
          })
            .width(this.pictureWidth)
            .height(this.pictureHeight)
            .backgroundColor('#000000')
            .focusable(true)
            .defaultFocus(true)
            .onMouse((event: MouseEvent) => {
              this.onPointerMouse(event);
            })
            .onTouch((event: TouchEvent) => {
              this.onPointerTouch(event);
            })
            .onHover((isHover: boolean) => {
              this.onPointerHover(isHover);
            })
            .onAxisEvent((event: AxisEvent) => {
              this.onPointerAxis(event);
            })
            .onKeyEvent((event: KeyEvent) => {
              this.onSurfaceKey(event);
            })
            .onBlur(() => {
              this.onSurfaceBlur();
            })

          if (this.engineScroll) {
            // Vertical only (Scroll is single-axis); Transparent keeps mouse/touch/keyboard on the
            // XComponent, and scrollBar must be Off (Scroll defaults to Auto).
            Scroll() {
              Column()
                .width(1)
                .height(this.pictureHeight + ENGINE_FRAME_HEADROOM_VP * 2)
            }
              .scrollable(ScrollDirection.Vertical)
              .scrollBar(BarState.Off)
              .focusable(false)
              .initialOffset({ xOffset: 0, yOffset: ENGINE_FRAME_HEADROOM_VP })
              .width(this.pictureWidth)
              .height(this.pictureHeight)
              .hitTestBehavior(HitTestMode.Transparent)
              .onWillScroll((xOffset: number, yOffset: number, scrollState: ScrollState,
                scrollSource: ScrollSource) => {
                // yOffset is in vp and same-signed as a touchpad axisVertical (xOffset stays 0).
                const pinned: OffsetResult = { xOffset: 0, yOffset: 0 };
                // Drag/fling is the touchpad (scaled by 「滚动速度」); the wheel is OTHER_USER_INPUT.
                this.onEngineScroll(yOffset, scrollSource === ScrollSource.DRAG
                  || scrollSource === ScrollSource.FLING);
                return pinned;
              })
          }
        }
      }
      .width('100%')
      .height('100%')

      if (this.revealBand > 0) {
        Row()
          .width(`${this.revealBandWidth}%`)
          .height(this.revealBand)
          .onHover((isHover: boolean) => {
            this.onRevealZone(isHover);
          })
      }
    }
    .width('100%')
    .height('100%')
    .onAreaChange((oldArea: Area, newArea: Area) => {
      this.containerWidth = lengthToNumber(newArea.width);
      this.containerHeight = lengthToNumber(newArea.height);
      this.refit();
    })
  }
}
