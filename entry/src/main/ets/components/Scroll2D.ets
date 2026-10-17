const ENGINE_FRAME_HEADROOM_VP: number = 2000;

const ENGINE_VERTICAL_ID: string = 'hmrdpEngineVertical';
const ENGINE_HORIZONTAL_ID: string = 'hmrdpEngineHorizontal';

function engineBrowsePeer(current: GestureRecognizer, others: Array<GestureRecognizer>,
  siblingId: string): GestureRecognizer {
  for (let i = 0; i < others.length; i++) {
    const candidate: GestureRecognizer = others[i];
    if (!candidate.isBuiltIn() || candidate.getType() !== GestureControl.GestureType.PAN_GESTURE) {
      continue;
    }
    const info: EventTargetInfo = candidate.getEventTargetInfo();
    if (info !== undefined && info.getId() === siblingId) {
      return candidate;
    }
  }
  return current;
}

@Component
export struct Scroll2D {
  @Prop viewWidth: number = 0;
  @Prop viewHeight: number = 0;
  onScroll: (dx: number, dy: number) => void = (dx: number, dy: number) => {};

  build() {
    Stack({ alignContent: Alignment.Center }) {
      Scroll() {
        Column()
          .width(1)
          .height(this.viewHeight + ENGINE_FRAME_HEADROOM_VP * 2)
      }
        .id(ENGINE_VERTICAL_ID)
        .scrollable(ScrollDirection.Vertical)
        .scrollBar(BarState.Off)
        .focusable(false)
        .initialOffset({ xOffset: 0, yOffset: ENGINE_FRAME_HEADROOM_VP })
        .width(this.viewWidth)
        .height(this.viewHeight)
        .hitTestBehavior(HitTestMode.Transparent)
        .shouldBuiltInRecognizerParallelWith((current: GestureRecognizer,
          others: Array<GestureRecognizer>): GestureRecognizer =>
          engineBrowsePeer(current, others, ENGINE_HORIZONTAL_ID))
        .onWillScroll((xOffset: number, yOffset: number, _scrollState: ScrollState,
          _scrollSource: ScrollSource) => {
          const pinned: OffsetResult = { xOffset: 0, yOffset: 0 };
          this.onScroll(0, yOffset);
          return pinned;
        })

      Scroll() {
        Column()
          .width(this.viewWidth + ENGINE_FRAME_HEADROOM_VP * 2)
          .height(1)
      }
        .id(ENGINE_HORIZONTAL_ID)
        .scrollable(ScrollDirection.Horizontal)
        .scrollBar(BarState.Off)
        .focusable(false)
        .initialOffset({ xOffset: ENGINE_FRAME_HEADROOM_VP, yOffset: 0 })
        .width(this.viewWidth)
        .height(this.viewHeight)
        .hitTestBehavior(HitTestMode.Transparent)
        .shouldBuiltInRecognizerParallelWith((current: GestureRecognizer,
          others: Array<GestureRecognizer>): GestureRecognizer =>
          engineBrowsePeer(current, others, ENGINE_VERTICAL_ID))
        .onWillScroll((xOffset: number, yOffset: number, _scrollState: ScrollState,
          _scrollSource: ScrollSource) => {
          const pinned: OffsetResult = { xOffset: 0, yOffset: 0 };
          this.onScroll(xOffset, 0);
          return pinned;
        })
    }
    .width(this.viewWidth)
    .height(this.viewHeight)
    .hitTestBehavior(HitTestMode.Transparent)
  }
}


