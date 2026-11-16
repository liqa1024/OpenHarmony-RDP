import { lengthToNumber } from '../utils/UiUtil';
import { fitPictureRect } from '../utils/PictureFit';
import { Scroll2D } from './Scroll2D';

@Component
export struct SessionPicture {
  xComponentId: string = '';
  pictureController: XComponentController = new XComponentController();
  @Prop @Watch('refit') contentWidth: number = 0;
  @Prop @Watch('refit') contentHeight: number = 0;
  // HDR 内容的显示亮度（ArkUI 组件级，0–1：0 = 按 SDR 亮度显示、1 = 按当前允许的最高 HDR 亮度，1 即平台
  // 默认值）。只有请求了 HDR 的会话才由会话页传非 1 的值；非 HDR 恒为默认值，不动 SDR 的显示。
  @Prop hdrBrightness: number = 1;
  @Prop revealBand: number = 0;
  @Prop revealBandWidth: number = 100;
  // 工具条收起时由会话页 +1：此时焦点已随 chip 卸载落回根容器，键盘通路随之失效。见 reclaimFocus。
  @Prop @Watch('reclaimFocus') reclaimFocusToken: number = 0;
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

  // defaultFocus 只在层级页面第一次进入时生效，而画面尺寸要等首次布局之后才知道，所以画面挂载时
  // 页面默认焦点已经落到根容器上，必须有显式申请：否则画面永远不获焦，按键（穿透关闭时的通路）
  // 没有获焦组件可投递。点击获焦另外给一条手动恢复的路。
  private claimSurfaceFocus(): void {
    setTimeout((): void => {
      try {
        this.getUIContext().getFocusController().requestFocus(this.xComponentId);
      } catch (error) {
      }
    }, 0);
  }

  // 工具条收起（自动隐藏 / 取消固定 / 连接完成）后把焦点还给画面，否则键盘通路没有获焦组件。
  private reclaimFocus(): void {
    this.claimSurfaceFocus();
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
            .hdrBrightness(this.hdrBrightness)

            .focusable(true)
            .defaultFocus(true)
            .focusOnTouch(true)
            .id(this.xComponentId)
            .onAppear(() => {
              this.claimSurfaceFocus();
            })
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
    .backgroundColor($r('app.color.chrome_bg_solid'))
    .onAreaChange((oldArea: Area, newArea: Area) => {
      this.containerWidth = lengthToNumber(newArea.width);
      this.containerHeight = lengthToNumber(newArea.height);
      this.refit();
    })
  }
}


