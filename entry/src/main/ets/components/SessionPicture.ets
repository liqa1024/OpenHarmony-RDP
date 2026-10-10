import { lengthToNumber } from '../utils/UiUtil';
import { fitPictureRect } from '../utils/PictureFit';

// The session picture surface: a SURFACE XComponent laid out centred at the content
// aspect ratio, so the compositor scales it proportionally on a window resize. The
// black bars it leaves are inert; input stays on the XComponent, whose coordinates
// are therefore picture-local. In fullscreen a top strip reports the toolbar reveal zone.
@Component
export struct SessionPicture {
  xComponentId: string = '';
  pictureController: XComponentController = new XComponentController();
  @Prop @Watch('refit') contentWidth: number = 0;
  @Prop @Watch('refit') contentHeight: number = 0;
  @Prop revealBand: number = 0;
  onPointerMouse: (event: MouseEvent) => void = (event: MouseEvent) => {};
  onPointerTouch: (event: TouchEvent) => void = (event: TouchEvent) => {};
  onPointerHover: (isHover: boolean) => void = (isHover: boolean) => {};
  onPointerAxis: (event: AxisEvent) => void = (event: AxisEvent) => {};
  onSurfaceKey: (event: KeyEvent) => void = (event: KeyEvent) => {};
  onSurfaceBlur: () => void = () => {};
  onPictureArea: (width: number, height: number) => void = (width: number, height: number) => {};
  onRevealZone: (inBand: boolean) => void = (inBand: boolean) => {};
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
        }
      }
      .width('100%')
      .height('100%')

      if (this.revealBand > 0) {
        Row()
          .width('100%')
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
