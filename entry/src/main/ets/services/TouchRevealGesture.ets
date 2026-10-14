// Top-edge pull-down reveal for touchscreens: without a pointer the hover-driven toolbar zone is
// unreachable, so a deliberate pull-down from the top edge stands in for it. The same path closes
// the loop the other way — with the toolbar showing, a touch on the picture dismisses it.
//
// The gesture owns its contact from the Down: what it holds is either replayed through the touch
// path (a pull that never became a swipe, or any drag) or dropped whole (a reveal swipe, a dismiss
// tap), so a reveal never leaks a stray drag into the far end. Nothing is latched across gestures:
// the mode is read per event, so the toolbar appearing mid-gesture cannot strand a held contact.

export const TOUCH_REVEAL_NONE: number = 0;
export const TOUCH_REVEAL_HIDDEN: number = 1;
export const TOUCH_REVEAL_VISIBLE: number = 2;

// A downward drag is remote scrolling unless it starts on the top edge strip (vp, window
// coordinates) — the touch equivalent of the pointer's reveal band.
const REVEAL_BAND_VP: number = 32;
const REVEAL_SWIPE_VP: number = 24;
const REVEAL_SLOP_VP: number = 16;
const DISMISS_TAP_SLOP_VP: number = 10;

export interface TouchRevealHost {
  mode: () => number;
  forward: (phase: TouchType, id: number, x: number, y: number, pressure: number) => void;
  noteContact: () => void;
  onSwipe: () => void;
  onDismiss: () => void;
}

class HeldSample {
  phase: TouchType = TouchType.Down;
  id: number = 0;
  x: number = 0;
  y: number = 0;
  pressure: number = 0;
}

export class TouchRevealGesture {
  private host: TouchRevealHost;
  private owning: boolean = false;
  private swallowing: boolean = false;
  private id: number = -1;
  private startX: number = 0;
  private startY: number = 0;
  private held: HeldSample[] = [];

  constructor(host: TouchRevealHost) {
    this.host = host;
  }

  // true = the caller must not hand this event to the touch path.
  handle(event: TouchEvent): boolean {
    if (event.source !== SourceType.TouchScreen) {
      return false;
    }
    const changed: TouchObject[] = event.changedTouches;
    if (this.owning) {
      return this.track(changed);
    }
    return this.begin(changed);
  }

  // Teardown: whatever was held is not worth replaying into a dying session.
  dispose(): void {
    this.held.length = 0;
    this.owning = false;
    this.swallowing = false;
    this.id = -1;
  }

  private begin(changed: TouchObject[]): boolean {
    if (changed.length !== 1 || changed[0].type !== TouchType.Down) {
      return false;
    }
    const touch: TouchObject = changed[0];
    const mode: number = this.host.mode();
    if (mode === TOUCH_REVEAL_NONE) {
      return false;
    }
    if (mode === TOUCH_REVEAL_HIDDEN && touch.windowY > REVEAL_BAND_VP) {
      return false;
    }
    this.owning = true;
    this.swallowing = false;
    this.id = touch.id;
    this.startX = touch.x;
    this.startY = touch.windowY;
    this.held.length = 0;
    this.hold(touch);
    // The engine yields the gesture even though the touch path may never see this contact.
    this.host.noteContact();
    return true;
  }

  private track(changed: TouchObject[]): boolean {
    if (this.swallowing) {
      for (let i = 0; i < changed.length; i++) {
        const touch: TouchObject = changed[i];
        if (touch.id === this.id && (touch.type === TouchType.Up
          || touch.type === TouchType.Cancel)) {
          this.release();
          break;
        }
      }
      return true;
    }
    const mode: number = this.host.mode();
    if (mode === TOUCH_REVEAL_NONE) {
      this.flush();
      this.release();
      return false;
    }
    let mine: TouchObject | undefined = undefined;
    for (let i = 0; i < changed.length; i++) {
      if (changed[i].id !== this.id) {
        // A second finger joins: give the whole gesture back to the touch path.
        this.flush();
        this.release();
        return false;
      }
      mine = changed[i];
    }
    if (mine === undefined) {
      return true;
    }
    const dx: number = mine.x - this.startX;
    const dy: number = mine.windowY - this.startY;
    const ended: boolean = mine.type === TouchType.Up || mine.type === TouchType.Cancel;
    if (mode === TOUCH_REVEAL_HIDDEN) {
      if (dy >= REVEAL_SWIPE_VP && dy >= Math.abs(dx)) {
        this.held.length = 0;
        this.swallowing = true;
        this.host.onSwipe();
        if (ended) {
          this.release();
        }
        return true;
      }
      if (ended || dy <= -REVEAL_SLOP_VP || Math.abs(dx) >= REVEAL_SLOP_VP) {
        this.flush();
        this.release();
        return false;
      }
      this.hold(mine);
      return true;
    }
    if (ended) {
      this.held.length = 0;
      this.release();
      this.host.onDismiss();
      return true;
    }
    if (Math.abs(dx) > DISMISS_TAP_SLOP_VP || Math.abs(dy) > DISMISS_TAP_SLOP_VP) {
      this.flush();
      this.release();
      this.host.onDismiss();
      return false;
    }
    this.hold(mine);
    return true;
  }

  private hold(touch: TouchObject): void {
    const sample: HeldSample = new HeldSample();
    sample.phase = touch.type;
    sample.id = touch.id;
    sample.x = touch.x;
    sample.y = touch.y;
    sample.pressure = touch.pressure ?? 0;
    this.held.push(sample);
  }

  private flush(): void {
    for (let i = 0; i < this.held.length; i++) {
      const sample: HeldSample = this.held[i];
      this.host.forward(sample.phase, sample.id, sample.x, sample.y, sample.pressure);
    }
    this.held.length = 0;
  }

  private release(): void {
    this.held.length = 0;
    this.owning = false;
    this.swallowing = false;
    this.id = -1;
  }
}
