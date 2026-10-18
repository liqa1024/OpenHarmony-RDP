import { ComponentContent, UIContext } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpDialog';

// Owns the open/close/dispose lifecycle of one custom dialog. The modal queue
// decides when a dialog starts; this class only guarantees that the content
// node is never closed twice or disposed while still on screen.
export class DialogSession<T extends Object> {
  private ui: UIContext;
  private content: ComponentContent<T> | undefined = undefined;
  private opened: boolean = false;
  private disposed: boolean = false;

  constructor(ui: UIContext) {
    this.ui = ui;
  }

  open(content: ComponentContent<T>, onFailed: () => void): void {
    if (!this.ui.isAvailable()) {
      this.disposeContent(content);
      onFailed();
      return;
    }
    this.content = content;
    this.ui.getPromptAction().openCustomDialog(content,
      { isModal: true, autoCancel: false })
      .then(() => {
        this.opened = true;
        if (this.disposed) {
          this.closeContent(content);
        }
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'open dialog failed: %{public}s', JSON.stringify(error));
        this.disposeNode();
        onFailed();
      });
  }

  close(after: () => void): void {
    if (this.disposed) {
      after();
      return;
    }
    this.disposed = true;
    const content: ComponentContent<T> | undefined = this.content;
    if (content === undefined || !this.opened) {
      this.disposeNode();
      after();
      return;
    }
    this.closeContent(content, after);
  }

  dispose(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    const content: ComponentContent<T> | undefined = this.content;
    if (content === undefined || !this.opened) {
      this.disposeNode();
      return;
    }
    this.closeContent(content, (): void => {});
  }

  private closeContent(content: ComponentContent<T>, after: () => void = (): void => {}): void {
    this.ui.getPromptAction().closeCustomDialog(content)
      .then(() => {
        this.disposeContent(content);
        after();
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'close dialog failed: %{public}s', JSON.stringify(error));
        this.disposeContent(content);
        after();
      });
  }

  private disposeNode(): void {
    const content: ComponentContent<T> | undefined = this.content;
    if (content === undefined) {
      return;
    }
    this.content = undefined;
    this.disposeContent(content);
  }

  private disposeContent(content: ComponentContent<T>): void {
    if (this.content === content) {
      this.content = undefined;
    }
    try {
      content.dispose();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'dispose dialog failed: %{public}s', JSON.stringify(error));
    }
  }
}
