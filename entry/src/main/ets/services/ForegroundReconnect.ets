import { common, ApplicationStateChangeCallback } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ConnectionState } from './SessionManager';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpForegroundReconnect';

// Native read loops only notice an aborted socket once they run again, so the
// reconnect check waits a grace period after the window is back.
const GRACE_MS: number = 400;

// Function-typed members so each session page can supply a typed object literal.
export interface ForegroundReconnectHost {
  context: () => common.Context | undefined;
  state: () => ConnectionState;
  reconnect: () => void;
}

class ForegroundStateCallback extends ApplicationStateChangeCallback {
  private onAway: () => void;
  private onReturn: () => void;

  constructor(onAway: () => void, onReturn: () => void) {
    super();
    this.onAway = onAway;
    this.onReturn = onReturn;
  }

  onApplicationForeground(): void {
    this.onReturn();
  }

  onApplicationBackground(): void {
    this.onAway();
  }
}

// Shared 回到前台自动重连: not keep-alive, only covers "already dropped while we
// were away" once. Only takes over Idle/Failed so a mid-session drop still needs
// the manual 重试. See doc_agent/session-layer.md §5.
export class ForegroundReconnect {
  private host: ForegroundReconnectHost;
  private enabled: boolean;
  private awayFromForeground: boolean = false;
  private timer: number = -1;
  private appContext: common.ApplicationContext | undefined = undefined;
  private callback: ForegroundStateCallback | undefined = undefined;

  constructor(host: ForegroundReconnectHost, enabled: boolean) {
    this.host = host;
    this.enabled = enabled;
    this.watchApplicationState();
  }

  onPageShow(): void {
    this.scheduleReconnect();
  }

  onPageHide(): void {
    this.awayFromForeground = true;
  }

  release(): void {
    if (this.timer !== -1) {
      clearTimeout(this.timer);
      this.timer = -1;
    }
    this.unwatchApplicationState();
  }

  private watchApplicationState(): void {
    if (this.callback !== undefined) {
      return;
    }
    try {
      const ctx: common.Context | undefined = this.host.context();
      if (ctx === undefined) {
        return;
      }
      const application: common.ApplicationContext = ctx.getApplicationContext();
      const callback: ForegroundStateCallback = new ForegroundStateCallback(() => {
        this.awayFromForeground = true;
      }, () => {
        this.scheduleReconnect();
      });
      application.on('applicationStateChange', callback);
      this.appContext = application;
      this.callback = callback;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'watch application state failed: %{public}s', JSON.stringify(error));
    }
  }

  private unwatchApplicationState(): void {
    const application: common.ApplicationContext | undefined = this.appContext;
    const callback: ForegroundStateCallback | undefined = this.callback;
    this.appContext = undefined;
    this.callback = undefined;
    if (application === undefined || callback === undefined) {
      return;
    }
    try {
      application.off('applicationStateChange', callback);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'unwatch application state failed: %{public}s', JSON.stringify(error));
    }
  }

  private scheduleReconnect(): void {
    if (!this.awayFromForeground || !this.enabled) {
      return;
    }
    if (this.timer !== -1) {
      clearTimeout(this.timer);
    }
    this.timer = setTimeout(() => {
      this.timer = -1;
      // The flag means "we are away"; running this check is what makes it stale.
      const wasAway: boolean = this.awayFromForeground;
      this.awayFromForeground = false;
      const state: ConnectionState = this.host.state();
      if (wasAway && (state === ConnectionState.Idle || state === ConnectionState.Failed)) {
        this.host.reconnect();
      }
    }, GRACE_MS);
  }
}
