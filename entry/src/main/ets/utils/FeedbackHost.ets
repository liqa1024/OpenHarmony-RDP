import { UIContext } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpFeedback';
const TOAST_DEDUPE_MS: number = 1000;
const TOAST_DEDUPE_MAX: number = 64;
const TOAST_DEDUPE_PRUNE_MS: number = 5000;

export enum ToastTier {
  Short = 0,
  Default = 1,
  Long = 2
}

export interface ModalRequest {
  start(done: () => void, closed: () => void): void;
  abort(): void;
}

// One host per window/UIContext. It serializes modals and keeps transient
// feedback scoped to the window that produced it.
export class FeedbackHost {
  private ui: UIContext;
  private active: ModalRequest | undefined = undefined;
  private pending: ModalRequest[] = [];
  private modalOpen: boolean = false;
  private toastAt: Map<string, number> = new Map<string, number>();

  constructor(ui: UIContext) {
    this.ui = ui;
  }

  toast(message: string, tier: ToastTier = ToastTier.Default): void {
    if (!this.ui.isAvailable() || this.modalOpen) {
      return;
    }
    const now: number = Date.now();
    const last: number | undefined = this.toastAt.get(message);
    if (last !== undefined && now - last < TOAST_DEDUPE_MS) {
      return;
    }
    this.toastAt.set(message, now);
    this.pruneToasts(now);
    try {
      this.ui.getPromptAction().showToast({
        message: message,
        duration: FeedbackHost.durationOf(tier),
        backgroundColor: $r('app.color.chrome_bg'),
        textColor: $r('app.color.chrome_text_primary'),
        backgroundBlurStyle: BlurStyle.NONE,
        shadow: {
          radius: 16,
          color: $r('app.color.shadow_2'),
          offsetX: 0,
          offsetY: 4
        }
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'show toast failed: %{public}s', JSON.stringify(error));
    }
  }

  enqueue(request: ModalRequest): void {
    if (!this.ui.isAvailable()) {
      request.abort();
      return;
    }
    if (this.active !== undefined) {
      this.pending.push(request);
      return;
    }
    this.start(request);
  }

  release(): void {
    const active: ModalRequest | undefined = this.active;
    this.active = undefined;
    this.modalOpen = false;
    if (active !== undefined) {
      active.abort();
    }
    const pending: ModalRequest[] = this.pending;
    this.pending = [];
    for (let i = 0; i < pending.length; i++) {
      pending[i].abort();
    }
    this.toastAt.clear();
  }

  private start(request: ModalRequest): void {
    this.active = request;
    this.modalOpen = true;
    try {
      request.start((): void => {
        this.finish(request);
      }, (): void => {
        if (this.active === request) {
          this.modalOpen = false;
        }
      });
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'start modal failed: %{public}s', JSON.stringify(error));
      this.active = undefined;
      this.modalOpen = false;
      request.abort();
      this.startNext();
    }
  }

  private finish(request: ModalRequest): void {
    if (this.active !== request) {
      return;
    }
    this.active = undefined;
    this.modalOpen = false;
    this.startNext();
  }

  private startNext(): void {
    if (this.active !== undefined || this.pending.length === 0) {
      return;
    }
    const next: ModalRequest | undefined = this.pending.shift();
    if (next === undefined) {
      return;
    }
    this.start(next);
  }

  private pruneToasts(now: number): void {
    if (this.toastAt.size <= TOAST_DEDUPE_MAX) {
      return;
    }
    const stale: string[] = [];
    this.toastAt.forEach((at: number, message: string): void => {
      if (now - at > TOAST_DEDUPE_PRUNE_MS) {
        stale.push(message);
      }
    });
    for (let i = 0; i < stale.length; i++) {
      this.toastAt.delete(stale[i]);
    }
  }

  private static durationOf(tier: ToastTier): number {
    if (tier === ToastTier.Short) {
      return 1500;
    }
    if (tier === ToastTier.Long) {
      return 2500;
    }
    return 2000;
  }
}

const HOSTS: Map<UIContext, FeedbackHost> = new Map<UIContext, FeedbackHost>();

export function feedbackHost(ui: UIContext): FeedbackHost {
  let host: FeedbackHost | undefined = HOSTS.get(ui);
  if (host === undefined) {
    host = new FeedbackHost(ui);
    HOSTS.set(ui, host);
  }
  return host;
}

export function releaseFeedback(ui: UIContext): void {
  const host: FeedbackHost | undefined = HOSTS.get(ui);
  if (host === undefined) {
    return;
  }
  HOSTS.delete(ui);
  host.release();
}
