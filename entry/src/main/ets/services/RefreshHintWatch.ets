// The decoder already attributes the driven part of the loss: `refreshDroppedFrames` counts only the
// frames given up while the panel's gear was below the stream rate (display-gate refusals, hand-off
// replacements and input-budget drops at that gear). So a sustained share of those names a screen that
// cannot hold the stream — and the only fix is a system setting the app cannot touch. A single window
// is noise (a gear switch, a resync), so the hint needs the share to hold for consecutive windows; it
// then stays quiet for the rest of the session, because the user was already told once.

// Two windows is enough to be sure, short enough to react while the user is still in the session.
const BAD_WINDOWS: number = 2;
// Below this share the occasional drop is the normal jitter around a matching gear.
const MIN_DROP_SHARE: number = 0.1;

export class RefreshHintWatch {
  private streak: number = 0;
  private fired: boolean = false;

  // One sample per telemetry window; true exactly once, on the window that crosses the streak.
  sample(presented: number, refreshDropped: number): boolean {
    if (this.fired) {
      return false;
    }
    const total: number = presented + refreshDropped;
    if (total <= 0 || refreshDropped / total <= MIN_DROP_SHARE) {
      this.streak = 0;
      return false;
    }
    this.streak++;
    if (this.streak < BAD_WINDOWS) {
      return false;
    }
    this.fired = true;
    return true;
  }
}