import { display, window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { setDisplayRefreshRateHz } from 'libhmrdp.so';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpDisplayRate';

// The shared display-rate authority (hmrdp_display_rate.*) reads the *default* display, but a session
// window can sit on another one (multi-monitor 2in1), and both the present gate and the refresh hint
// must follow the screen the picture is actually on. Only this layer can say which display that is
// (window properties), so it resolves the rate and pushes it down; the native side falls back to the
// default display while nothing is pushed.
export class SessionDisplayRate {
  private win: window.Window | undefined = undefined;
  private displayId: number = -1;
  private pushedHz: number = 0;
  private displayChange: (id: number) => void = (id: number): void => {};
  private rectChange: (options: window.RectChangeOptions) => void =
    (options: window.RectChangeOptions): void => {};

  attach(win: window.Window): void {
    this.win = win;
    this.displayId = this.windowDisplayId();
    this.displayChange = (id: number): void => {
      if (id === this.displayId) {
        this.push();
      }
    };
    // A window moved to another monitor is the one case the display event cannot report.
    this.rectChange = (options: window.RectChangeOptions): void => {
      const moved: number = this.windowDisplayId();
      if (moved !== this.displayId) {
        this.displayId = moved;
        this.push();
      }
    };
    try {
      display.on('change', this.displayChange);
      win.on('windowRectChange', this.rectChange);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'watch display change failed: %{public}s', JSON.stringify(error));
    }
    this.push();
  }

  release(): void {
    const win: window.Window | undefined = this.win;
    if (win !== undefined) {
      try {
        display.off('change', this.displayChange);
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'off display change failed: %{public}s', JSON.stringify(error));
      }
      try {
        win.off('windowRectChange', this.rectChange);
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'off rect change failed: %{public}s', JSON.stringify(error));
      }
    }
    this.win = undefined;
    this.displayId = -1;
    this.pushedHz = 0;
    setDisplayRefreshRateHz(0);
  }

  private windowDisplayId(): number {
    const win: window.Window | undefined = this.win;
    if (win === undefined) {
      return -1;
    }
    const id: number | undefined = win.getWindowProperties().displayId;
    return id !== undefined ? id : -1;
  }

  private push(): void {
    if (this.displayId < 0) {
      return;
    }
    let hz: number = 0;
    try {
      hz = display.getDisplayByIdSync(this.displayId).refreshRate;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'display %{public}d refresh rate unavailable: %{public}s',
        this.displayId, JSON.stringify(error));
      return;
    }
    if (hz === this.pushedHz) {
      return;
    }
    this.pushedHz = hz;
    setDisplayRefreshRateHz(hz);
    hilog.info(DOMAIN, TAG, 'window display %{public}d runs at %{public}d Hz', this.displayId, hz);
  }
}