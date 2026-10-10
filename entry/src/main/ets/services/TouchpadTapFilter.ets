// 触控板「双指轻触＝右键」与「按压/按住右键」在 ArkUI 事件里不可区分（见 doc_agent/session-layer.md §8），
// 中键靠延迟右键一个双击窗口合成；按下→抬起时长把轻触与实体右键（拖动）分开。
export interface TouchpadTapTarget {
  rightDown(x: number, y: number): void;
  rightUp(x: number, y: number): void;
  clickRight(x: number, y: number): void;
  clickMiddle(x: number, y: number): void;
}

export class TouchpadTapFilter {
  private target: TouchpadTapTarget;
  private doubleTapMs: number;
  private holdMs: number;
  private moveSlop: number;
  private down: boolean = false;
  private held: boolean = false;
  private pending: boolean = false;
  private swallow: boolean = false;
  private clickTimer: number = -1;
  private holdTimer: number = -1;
  private pressX: number = 0;
  private pressY: number = 0;

  constructor(target: TouchpadTapTarget, doubleTapMs: number, holdMs: number, moveSlop: number) {
    this.target = target;
    this.doubleTapMs = doubleTapMs;
    this.holdMs = holdMs;
    this.moveSlop = moveSlop;
  }

  press(x: number, y: number): void {
    if (this.held) {
      this.target.rightDown(x, y);
      return;
    }
    this.down = true;
    this.pressX = x;
    this.pressY = y;
    if (this.pending) {
      this.clearTimers();
      this.pending = false;
      this.swallow = true;
      this.target.clickMiddle(x, y);
      return;
    }
    this.pending = true;
    this.clickTimer = setTimeout(() => {
      this.clickTimer = -1;
      if (!this.pending) {
        return;
      }
      this.pending = false;
      this.target.clickRight(this.pressX, this.pressY);
    }, this.doubleTapMs);
    this.holdTimer = setTimeout(() => {
      this.holdTimer = -1;
      if (!this.pending) {
        return;
      }
      this.commit(x, y);
    }, this.holdMs);
  }

  release(x: number, y: number): void {
    this.down = false;
    if (this.swallow) {
      this.swallow = false;
      return;
    }
    if (this.held) {
      this.held = false;
      this.target.rightUp(x, y);
      return;
    }
    this.cancelHoldTimer();
  }

  observeMove(x: number, y: number): void {
    if (!this.down || !this.pending) {
      return;
    }
    const dx: number = x - this.pressX;
    const dy: number = y - this.pressY;
    if (dx * dx + dy * dy <= this.moveSlop * this.moveSlop) {
      return;
    }
    this.commit(x, y);
  }

  cancel(x: number, y: number): void {
    const wasHeld: boolean = this.held;
    this.reset();
    if (wasHeld) {
      this.target.rightUp(x, y);
    }
  }

  reset(): void {
    this.clearTimers();
    this.down = false;
    this.held = false;
    this.pending = false;
    this.swallow = false;
  }

  private commit(x: number, y: number): void {
    this.clearTimers();
    this.pending = false;
    this.held = true;
    this.target.rightDown(x, y);
  }

  private cancelHoldTimer(): void {
    if (this.holdTimer !== -1) {
      clearTimeout(this.holdTimer);
      this.holdTimer = -1;
    }
  }

  private clearTimers(): void {
    if (this.clickTimer !== -1) {
      clearTimeout(this.clickTimer);
      this.clickTimer = -1;
    }
    this.cancelHoldTimer();
  }
}
