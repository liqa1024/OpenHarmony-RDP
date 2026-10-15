// Sustained-overload detection for the per-frame latency readings a session toolbar shows. A single
// bad window is noise (a scene change, a resync), so a side counts as overloaded only after it stays
// past its red threshold for consecutive windows; it then stays quiet until the reading recovers, so
// a flapping number cannot repeat the notice. Sides are tracked separately: a local spike must not
// consume the host's warning.

export const OVERLOAD_NONE: number = 0;
export const OVERLOAD_LOCAL: number = 1;
export const OVERLOAD_HOST: number = 2;

// Two windows is enough to be sure, short enough to react while the user is still in the session.
const BAD_WINDOWS: number = 2;

class OverloadSide {
  private streak: number = 0;
  private armed: boolean = true;

  // True exactly once per overload episode, on the window that crosses the streak requirement.
  // Readings below 0 mean "no sample" and neither trip nor clear the side.
  sample(ms: number, badMs: number): boolean {
    if (ms < 0 || badMs <= 0) {
      return false;
    }
    if (ms < badMs) {
      this.streak = 0;
      this.armed = true;
      return false;
    }
    if (!this.armed) {
      return false;
    }
    this.streak++;
    if (this.streak < BAD_WINDOWS) {
      return false;
    }
    this.armed = false;
    return true;
  }
}

export class OverloadWatch {
  private local: OverloadSide = new OverloadSide();
  private host: OverloadSide = new OverloadSide();

  // One sample per toolbar window; returns which side just tripped (OVERLOAD_NONE when neither did).
  sample(localMs: number, hostMs: number, badMs: number): number {
    const local: boolean = this.local.sample(localMs, badMs);
    const host: boolean = this.host.sample(hostMs, badMs);
    // The local pipeline is the one the client owns, so it wins when both trip on one window.
    if (local) {
      return OVERLOAD_LOCAL;
    }
    if (host) {
      return OVERLOAD_HOST;
    }
    return OVERLOAD_NONE;
  }
}
