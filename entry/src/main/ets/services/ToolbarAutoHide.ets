/*
 * HmRdp - fullscreen toolbar auto-hide.
 *
 * The visibility state machine of the floating session toolbar, extracted from
 * the page so both session pages share it: the pointer zone (fed by the surface
 * pointer and the bar's own hover), the reveal dwell and the hide delay, and the
 * pin-while-busy rule. It owns no UI: the host page supplies the live state it
 * depends on (fullscreen / connected / pinned / delays) and mirrors the results
 * (`show()` / the reveal-hint fill) back into its own @State, so the page keeps
 * owning rendering and animation.
 *
 * The single decision point is refresh(): a hide is never pending while the
 * pointer is on the bar, whatever triggered the call.
 */
import { REVEAL_HINT_FADE_MS, TOOLBAR_ZONE_NONE, TOOLBAR_ZONE_TRIGGER } from '../components/SessionChrome';

/** The page-supplied live state and result sinks the controller reads/writes. */
export interface ToolbarAutoHideHost {
  /** False once the page is tearing down: no timer may arm. */
  isArmed: () => boolean;
  isFullscreen: () => boolean;
  isConnected: () => boolean;
  /** A running or pending transfer pins the bar. */
  isPinned: () => boolean;
  hoverDelay: () => number;
  hideDelay: () => number;
  /** Current bar visibility (the page's @State). */
  show: () => boolean;
  setShow: (show: boolean) => void;
  /** Current reveal-hint fill, 0..1 (the page's @State). */
  revealFill: () => number;
  /** Animates the reveal-hint fill on the page (needs its UIContext). */
  animateReveal: (fill: number, durationMs: number, curve: Curve) => void;
}

export class ToolbarAutoHide {
  private readonly host: ToolbarAutoHideHost;
  private pointerZone: number = TOOLBAR_ZONE_NONE;
  // -1 = timer not running.
  private revealTimer: number = -1;
  private hideTimer: number = -1;

  constructor(host: ToolbarAutoHideHost) {
    this.host = host;
  }

  /** Reports which zone the pointer is in; zones come from the surface or the bar. */
  setPointerZone(zone: number): void {
    if (this.pointerZone === zone) {
      return;
    }
    this.pointerZone = zone;
    this.refresh();
  }

  /** Re-evaluates visibility; the one place the bar's show/hide is decided. */
  refresh(): void {
    if (!this.host.isArmed()) {
      return;
    }
    // Nothing but a pending reveal keeps the hint up, so anything else retracts it.
    if (!this.willReveal()) {
      this.hideRevealHint();
    }
    // Outside fullscreen the bar is either laid out above the picture (windowed
    // mode) or covered by the status overlay, so it cannot auto-hide.
    if (!this.host.isFullscreen() || !this.host.isConnected()) {
      this.clearTimers();
      return;
    }
    // A running or pending transfer pins the bar.
    if (this.host.isPinned()) {
      this.pin();
      return;
    }
    if (this.pointerZone !== TOOLBAR_ZONE_NONE) {
      this.cancelHideTimer();
    }
    if (this.willReveal()) {
      // Dwell in the top band reveals a hidden bar; leaving the band cancels the
      // dwell (the gesture was a pass-by).
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
    // Pointer on the bar itself: it stays up, whatever revealed it.
    this.host.setShow(true);
  }

  /** Releases the timers (page teardown). */
  dispose(): void {
    this.clearTimers();
  }

  /** True while the pointer dwells for a bar that is not up yet. */
  private willReveal(): boolean {
    return this.host.isFullscreen()
      && this.host.isConnected()
      && !this.host.isPinned()
      && this.pointerZone === TOOLBAR_ZONE_TRIGGER
      && !this.host.show();
  }

  /** Fills the top-edge hint over `duration`, i.e. over the pending dwell. */
  private showRevealHint(duration: number): void {
    this.host.animateReveal(1, duration, Curve.Linear);
  }

  /** Retracts the hint: the bar will not appear after all. */
  private hideRevealHint(): void {
    if (this.host.revealFill() === 0) {
      return;
    }
    this.host.animateReveal(0, REVEAL_HINT_FADE_MS, Curve.FastOutSlowIn);
  }

  /** Pins the bar for as long as a transfer is running or pending. */
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
