import { REVEAL_HINT_FADE_MS, TOOLBAR_ZONE_NONE, TOOLBAR_ZONE_TRIGGER } from '../components/SessionChrome';

export interface ToolbarAutoHideHost {
  isArmed: () => boolean;
  isFullscreen: () => boolean;
  isConnected: () => boolean;
  isPinned: () => boolean;
  hoverDelay: () => number;
  hideDelay: () => number;
  show: () => boolean;
  setShow: (show: boolean) => void;
  revealFill: () => number;
  animateReveal: (fill: number, durationMs: number, curve: Curve) => void;
}

export class ToolbarAutoHide {
  private readonly host: ToolbarAutoHideHost;
  private pointerZone: number = TOOLBAR_ZONE_NONE;
  private revealTimer: number = -1;
  private hideTimer: number = -1;

  constructor(host: ToolbarAutoHideHost) {
    this.host = host;
  }

  setPointerZone(zone: number): void {
    if (this.pointerZone === zone) {
      return;
    }
    this.pointerZone = zone;
    this.refresh();
  }

  refresh(): void {
    if (!this.host.isArmed()) {
      return;
    }
    if (!this.willReveal()) {
      this.hideRevealHint();
    }
    if (!this.host.isFullscreen() || !this.host.isConnected()) {
      this.clearTimers();
      return;
    }
    if (this.host.isPinned()) {
      this.pin();
      return;
    }
    if (this.pointerZone !== TOOLBAR_ZONE_NONE) {
      this.cancelHideTimer();
    }
    if (this.willReveal()) {
      if (this.revealTimer === -1) {
        this.revealTimer = setTimeout(() => {
          this.revealTimer = -1;
          this.host.setShow(true);
          this.hideRevealHint();
        }, this.host.hoverDelay());
        this.showRevealHint(this.host.hoverDelay());
      }
      return;
    }
    this.cancelRevealTimer();
    if (this.pointerZone === TOOLBAR_ZONE_NONE) {
      this.scheduleHide();
      return;
    }
    this.host.setShow(true);
  }

  dispose(): void {
    this.clearTimers();
  }

  private willReveal(): boolean {
    return this.host.isFullscreen()
      && this.host.isConnected()
      && !this.host.isPinned()
      && this.pointerZone === TOOLBAR_ZONE_TRIGGER
      && !this.host.show();
  }

  private showRevealHint(duration: number): void {
    this.host.animateReveal(1, duration, Curve.Linear);
  }

  private hideRevealHint(): void {
    if (this.host.revealFill() === 0) {
      return;
    }
    this.host.animateReveal(0, REVEAL_HINT_FADE_MS, Curve.FastOutSlowIn);
  }

  private pin(): void {
    this.clearTimers();
    if (!this.host.show()) {
      this.host.setShow(true);
    }
  }

  private scheduleHide(): void {
    if (this.host.show() && this.hideTimer === -1) {
      this.hideTimer = setTimeout(() => {
        this.hideTimer = -1;
        this.host.setShow(false);
      }, this.host.hideDelay());
    }
  }

  private clearTimers(): void {
    this.cancelRevealTimer();
    this.cancelHideTimer();
  }

  private cancelRevealTimer(): void {
    if (this.revealTimer !== -1) {
      clearTimeout(this.revealTimer);
      this.revealTimer = -1;
    }
  }

  private cancelHideTimer(): void {
    if (this.hideTimer !== -1) {
      clearTimeout(this.hideTimer);
      this.hideTimer = -1;
    }
  }
}
