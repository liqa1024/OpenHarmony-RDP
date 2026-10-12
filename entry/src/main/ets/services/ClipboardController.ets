import { ClipboardCapabilities, ClipboardItem, ClipboardTransferDirection,
  LocalReadResult } from '../model/ClipboardModels';
import { ClipboardBridge, ClipboardBridgeSink } from './ClipboardBridge';
import { LocalPasteboard, LocalPasteboardHost, LocalReadOptions } from '../utils/LocalPasteboard';

// Function-typed properties so a page can supply a closure object.
export interface ClipboardControllerHost {
  toast: (message: string) => void;
  // Show the "cancel upload?" confirmation and invoke `onConfirm` only if the
  // user agrees (the dialog API has no cancel callback).
  confirmCancelUpload: (onConfirm: () => void) => void;
  setTransferProgress: (direction: number, percent: number, text: string,
    speedBytesPerSecond: number, fileIndex: number, fileCount: number) => void;
  setTransferWaiting: (direction: number, fileIndex: number, fileCount: number) => void;
  clearTransfer: () => void;
  onRemoteChanged: () => void;
}

const MSG_BUSY: string = '正在传输文件，请先取消或等待完成';
const MSG_EMPTY_REMOTE: string = '远端剪贴板为空';
const MSG_EMPTY_LOCAL: string = '本机剪贴板为空';
const MSG_FOLDER: string = '不支持文件夹传输';
const MSG_READ_FAILED: string = '读取本机剪贴板失败';

// Manual clipboard flow shared by both protocols: the remote content is applied
// only on demand and the local content is read only on demand.
export class ClipboardController implements ClipboardBridgeSink, LocalPasteboardHost {
  private bridge: ClipboardBridge;
  private host: ClipboardControllerHost;
  private caps: ClipboardCapabilities;
  private pasteboardBusy: boolean = false;
  private transferActive: boolean = false;
  private transferWaiting: boolean = false;
  private transferLastDone: number = 0;
  private transferLastAt: number = 0;

  constructor(bridge: ClipboardBridge, host: ClipboardControllerHost) {
    this.bridge = bridge;
    this.host = host;
    this.caps = bridge.capabilities();
    bridge.attach(this);
  }

  toast(message: string): void {
    this.host.toast(message);
  }

  canCopy(): boolean {
    const item: ClipboardItem | undefined = this.bridge.remoteItem();
    return item !== undefined && !item.isEmpty();
  }

  canPaste(): boolean {
    return this.caps.text || this.caps.image || this.caps.html || this.caps.files;
  }

  isTransferBlocking(): boolean {
    return this.transferActive || this.transferWaiting;
  }

  async copyRemoteToLocal(): Promise<void> {
    if (this.pasteboardBusy) {
      return;
    }
    if (this.isTransferBlocking() || this.bridge.isBlocked()) {
      this.host.toast(MSG_BUSY);
      return;
    }
    const item: ClipboardItem | undefined = this.bridge.remoteItem();
    if (item === undefined || item.isEmpty()) {
      this.host.toast(MSG_EMPTY_REMOTE);
      return;
    }
    if (item.hasFiles()) {
      if (!this.caps.files) {
        this.host.toast('不支持文件传输');
        return;
      }
      this.bridge.startDownload(item);
      return;
    }
    this.pasteboardBusy = true;
    await LocalPasteboard.write(item, ClipboardController.copyLabel(item), this);
    this.pasteboardBusy = false;
  }

  async pasteLocalToRemote(): Promise<void> {
    if (this.isTransferBlocking() || this.bridge.isBlocked()) {
      this.host.toast(MSG_BUSY);
      return;
    }
    const options: LocalReadOptions = new LocalReadOptions();
    options.allowFiles = this.caps.files;
    options.allowHtml = this.caps.html;
    // Only stage a sandbox directory when the clipboard actually holds files,
    // so a plain text paste does not create an empty transfer bucket.
    options.fileDir = this.caps.files && LocalPasteboard.hasFileUri() ? this.bridge.localStageDir() : '';
    const result: LocalReadResult = await LocalPasteboard.read(options);
    if (result.failed) {
      this.host.toast(MSG_READ_FAILED);
      return;
    }
    if (result.folderRejected) {
      this.host.toast(MSG_FOLDER);
      return;
    }
    const item: ClipboardItem | undefined = result.item;
    if (item === undefined || item.isEmpty()) {
      this.host.toast(MSG_EMPTY_LOCAL);
      return;
    }
    this.bridge.sendItem(item);
  }

  cancelTransfer(): void {
    if (!this.isTransferBlocking()) {
      return;
    }
    if (this.bridge.needsCancelConfirm()) {
      // Aborting an upload leaves the remote listing unreadable, so confirm first.
      this.host.confirmCancelUpload((): void => {
        this.doAbort();
      });
      return;
    }
    this.doAbort();
  }

  private doAbort(): void {
    this.bridge.abortTransfer();
    this.clearTransfer();
  }

  onRemoteChanged(): void {
    this.host.onRemoteChanged();
  }

  onMessage(text: string): void {
    this.host.toast(text);
  }

  onTransferProgress(direction: number, done: number, total: number, fileIndex: number,
    fileCount: number): void {
    this.transferWaiting = false;
    this.transferActive = true;
    let speed: number = 0;
    const now: number = Date.now();
    if (this.transferLastAt > 0 && now > this.transferLastAt && done >= this.transferLastDone) {
      speed = (done - this.transferLastDone) * 1000 / (now - this.transferLastAt);
    }
    this.transferLastDone = done;
    this.transferLastAt = now;
    const percent: number = total > 0 ? Math.min(100, Math.floor(done * 100 / total)) : 0;
    const text: string = direction === ClipboardTransferDirection.Upload
      ? '正在发送文件…' : '正在接收文件…';
    this.host.setTransferProgress(direction, percent, text, speed, fileIndex, fileCount);
  }

  onTransferWaiting(direction: number, fileIndex: number, fileCount: number): void {
    this.transferActive = false;
    this.transferWaiting = true;
    this.host.setTransferWaiting(direction, fileIndex, fileCount);
  }

  onTransferDone(): void {
    this.clearTransfer();
  }

  onTransferFailed(reason: string): void {
    this.clearTransfer();
    this.host.toast(reason.length > 0 ? reason : '文件传输失败');
  }

  onDownloadedFiles(paths: string[]): void {
    this.clearTransfer();
    if (paths.length === 0) {
      this.host.toast('远端文件为空');
      return;
    }
    const item: ClipboardItem = new ClipboardItem();
    item.files = paths;
    const label: string = paths.length > 1 ? ` ${paths.length} 个文件` : '文件';
    this.pasteboardBusy = true;
    LocalPasteboard.write(item, label, this).then((_ok: boolean): void => {
      this.pasteboardBusy = false;
    });
  }

  private clearTransfer(): void {
    this.transferActive = false;
    this.transferWaiting = false;
    this.transferLastDone = 0;
    this.transferLastAt = 0;
    this.host.clearTransfer();
  }

  private static copyLabel(item: ClipboardItem): string {
    if (item.hasImage()) {
      return '图片';
    }
    if (item.hasFiles()) {
      return '文件';
    }
    if (item.hasHtml()) {
      return '富文本';
    }
    return '文本';
  }
}
