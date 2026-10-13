import { ClipboardCapabilities, ClipboardItem } from '../model/ClipboardModels';

export interface ClipboardBridgeSink {
  onMessage(text: string): void;
  onRemoteChanged(): void;
  onTransferProgress(direction: number, done: number, total: number, fileIndex: number,
    fileCount: number): void;
  onTransferWaiting(direction: number, fileIndex: number, fileCount: number): void;
  onTransferDone(): void;
  onTransferFailed(reason: string): void;
  onDownloadedFiles(paths: string[]): void;
}

export interface ClipboardBridge {
  capabilities(): ClipboardCapabilities;
  attach(sink: ClipboardBridgeSink): void;
  // Protocol-level blocking the controller cannot see (it also tracks its own transfer state).
  isBlocked(): boolean;
  localStageDir(): string;
  remoteItem(): ClipboardItem;
  sendItem(item: ClipboardItem): void;
  startDownload(item: ClipboardItem): void;
  // Whether cancelling now would leave the remote listing unreadable.
  needsCancelConfirm(): boolean;
  abortTransfer(): void;
}
