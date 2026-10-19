import { promptAction, UIContext } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MaterialSupport } from './MaterialSupport';

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

export enum DialogResult {
  Cancel = 0,
  Confirm = 1,
  Dismissed = 2
}

// 交给窗口内 DialogHost 渲染的确认/通知弹窗参数。宿主用系统 CustomDialog
// （customStyle:false）承载，这是唯一能吃沉浸光感的弹窗通道。
export interface DialogRequest {
  title: string;
  message: string;
  cancelLabel: string;
  confirmLabel: string;
  danger: boolean;
  onResult: (result: DialogResult) => void;
}

export type DialogPresentFn = (request: DialogRequest) => void;
export type DialogDismissFn = () => void;

class DialogBinding {
  present: DialogPresentFn;
  dismiss: DialogDismissFn;
  // 登记时该宿主所在页面的路由索引（从 1 开始）。router.back() 会先让上一页 onPageShow、
  // 再销毁被弹出的页面，所以"栈顶"不等于"当前可见页"，必须按索引挑当前可见页的宿主。
  page: number;

  constructor(present: DialogPresentFn, dismiss: DialogDismissFn, page: number) {
    this.present = present;
    this.dismiss = dismiss;
    this.page = page;
  }
}

// One host per window/UIContext. It serializes modals and keeps transient
// feedback scoped to the window that produced it.
export class FeedbackHost {
  private ui: UIContext;
  private active: ModalRequest | undefined = undefined;
  private pending: ModalRequest[] = [];
  private modalOpen: boolean = false;
  private toastAt: Map<string, number> = new Map<string, number>();
  // 同一窗口内可能同时挂着多个 DialogHost（router 页面栈：被压住的页面组件不会销毁、
  // aboutToAppear 也不会重跑），所以用栈：顶层页面的宿主接弹窗，弹出后自动回落到下层。
  private bindings: DialogBinding[] = [];

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
      const options: promptAction.ShowToastOptions = {
        message: message,
        duration: FeedbackHost.durationOf(tier)
      };
      if (!MaterialSupport.applyToast(options)) {
        options.backgroundColor = $r('app.color.chrome_bg');
        options.textColor = $r('app.color.chrome_text_primary');
        options.backgroundBlurStyle = BlurStyle.NONE;
        options.shadow = {
          radius: 16,
          color: $r('app.color.shadow_2'),
          offsetX: 0,
          offsetY: 4
        };
      }
      this.ui.getPromptAction().showToast(options);
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

  // 窗口内 DialogHost 挂载/卸载时登记。后挂载的（= 栈顶页面）优先接弹窗。
  attachDialog(present: DialogPresentFn, dismiss: DialogDismissFn): void {
    this.bindings.push(new DialogBinding(present, dismiss, this.pageIndex()));
  }

  detachDialog(present: DialogPresentFn): void {
    const kept: DialogBinding[] = [];
    for (let i = 0; i < this.bindings.length; i++) {
      if (this.bindings[i].present !== present) {
        kept.push(this.bindings[i]);
      }
    }
    this.bindings = kept;
  }

  // 返回是否已交付给宿主；false 表示该窗口没有 DialogHost。
  presentDialog(request: DialogRequest): boolean {
    const binding: DialogBinding | undefined = this.visibleBinding();
    if (binding === undefined) {
      return false;
    }
    binding.present(request);
    return true;
  }

  dismissDialog(): void {
    const binding: DialogBinding | undefined = this.visibleBinding();
    if (binding !== undefined) {
      binding.dismiss();
    }
  }

  // 当前可见页面（路由栈顶）上的宿主；没有匹配时退回最后登记的。
  private visibleBinding(): DialogBinding | undefined {
    const top: number = this.pageIndex();
    for (let i = this.bindings.length - 1; i >= 0; i--) {
      if (this.bindings[i].page === top) {
        return this.bindings[i];
      }
    }
    return this.topBinding();
  }

  private topBinding(): DialogBinding | undefined {
    return this.bindings.length > 0 ? this.bindings[this.bindings.length - 1] : undefined;
  }

  private pageIndex(): number {
    try {
      return this.ui.getRouter().getState().index;
    } catch (error) {
      return -1;
    }
  }

  release(): void {
    const active: ModalRequest | undefined = this.active;
    this.active = undefined;
    this.modalOpen = false;
    this.bindings = [];
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

// getUIContext() 每次返回的 UIContext 包装对象并不稳定（同一窗口多次调用
// 也不 === ），所以宿主不能按对象身份做键；改用窗口的稳定标识。
function windowKey(ui: UIContext): string {
  const id: number | undefined = ui.getWindowId();
  if (id !== undefined) {
    return `w${id}`;
  }
  const name: string | undefined = ui.getWindowName();
  if (name !== undefined && name.length > 0) {
    return `n${name}`;
  }
  return 'default';
}

const HOSTS: Map<string, FeedbackHost> = new Map<string, FeedbackHost>();

export function feedbackHost(ui: UIContext): FeedbackHost {
  const key: string = windowKey(ui);
  let host: FeedbackHost | undefined = HOSTS.get(key);
  if (host === undefined) {
    host = new FeedbackHost(ui);
    HOSTS.set(key, host);
  }
  return host;
}

export function releaseFeedback(ui: UIContext): void {
  const key: string = windowKey(ui);
  const host: FeedbackHost | undefined = HOSTS.get(key);
  if (host === undefined) {
    return;
  }
  HOSTS.delete(key);
  host.release();
}
