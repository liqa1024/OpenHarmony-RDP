import { lengthToNumber } from '../utils/UiUtil';
import { fitPictureRect } from '../utils/PictureFit';
import { Scroll2D } from './Scroll2D';

@Component
export struct SessionPicture {
  xComponentId: string = '';
  pictureController: XComponentController = new XComponentController();
  @Prop @Watch('refit') contentWidth: number = 0;
  @Prop @Watch('refit') contentHeight: number = 0;
  @Prop revealBand: number = 0;
  @Prop revealBandWidth: number = 100;
  engineScroll: boolean = false;
  onPointerMouse: (event: MouseEvent) => void = (event: MouseEvent) => {};
  onPointerTouch: (event: TouchEvent) => void = (event: TouchEvent) => {};
  onPointerHover: (isHover: boolean) => void = (isHover: boolean) => {};
  onPointerAxis: (event: AxisEvent) => void = (event: AxisEvent) => {};
  onSurfaceKey: (event: KeyEvent) => void = (event: KeyEvent) => {};
  onSurfaceFocusAxis: (event: FocusAxisEvent) => void = (event: FocusAxisEvent) => {};
  onSurfaceBlur: () => void = () => {};
  onPictureArea: (width: number, height: number) => void = (width: number, height: number) => {};
  onRevealZone: (inBand: boolean) => void = (inBand: boolean) => {};
  onEngineScroll: (dx: number, dy: number) => void = (dx: number, dy: number) => {};
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
            .onFocusAxisEvent((event: FocusAxisEvent) => {
              this.onSurfaceFocusAxis(event);
            })
            .onBlur(() => {
              this.onSurfaceBlur();
            })

          if (this.engineScroll) {
            Scroll2D({
              viewWidth: this.pictureWidth,
              viewHeight: this.pictureHeight,
              onScroll: (dx: number, dy: number) => {
                this.onEngineScroll(dx, dy);
              }
            })
          }
        }
      }
      .width('100%')
      .height('100%')
      .focusScopeId('sessionPicture', true, false)

      if (this.revealBand > 0) {
        Row()
          .width(`${this.revealBandWidth}%`)
          .height(this.revealBand)
          .hitTestBehavior(HitTestMode.Transparent)
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


