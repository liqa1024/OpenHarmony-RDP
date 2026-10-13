// The 2D scroll engine: a pair of transparent single-axis Scroll overlays whose built-in pan
// recognizers are bridged so ONE touchpad/wheel gesture drives both axes at once. Single-axis
// components are mandatory, not a shortcut: their built-in drag start is the only place a touchpad
// gesture's "press" is turned into the internal signal that stops a running fling (2D free scrolling
// only stops on a real touch Down, which a touchpad never sends). Each overlay pins its offset (the
// fake content stays put) so only the per-frame delta leaves.
//
// Whether both axes actually take effect at the far end is the receiving application's call, not
// ours — see doc_agent/session-layer.md §8.
const ENGINE_FRAME_HEADROOM_VP: number = 2000;

// The two axis overlays bridge their pan recognizers in parallel (shouldBuiltInRecognizerParallelWith),
// matched by these ids in the response chain, so a diagonal gesture drives both.
const ENGINE_VERTICAL_ID: string = 'hmrdpEngineVertical';
const ENGINE_HORIZONTAL_ID: string = 'hmrdpEngineHorizontal';

// Return the sibling's built-in PAN recognizer so both run in parallel (a diagonal gesture drives
// both axes). Returning `current` (self) means "no bridge" to ArkUI, and returning `undefined` is not
// allowed by the callback type, so the fallback is the current recognizer.
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
