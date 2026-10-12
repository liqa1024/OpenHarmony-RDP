import { ClipboardCapabilities, ClipboardItem } from '../model/ClipboardModels';

// Bridge -> controller callbacks. The bridge reports state; the controller owns
// the system pasteboard and the UI.
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

// Protocol seam. The bridge owns the wire format and transport and declares what
// it can carry; the controller never drives an unsupported path.
export interface ClipboardBridge {
  capabilities(): ClipboardCapabilities;
  attach(sink: ClipboardBridgeSink): void;
  // Protocol-level blocking the controller cannot see (it also tracks its own transfer state).
  isBlocked(): boolean;
  // Sandbox directory for local clipboard files; empty when files are unsupported.
  localStageDir(): string;
  // Current remote content, or an empty item.
  remoteItem(): ClipboardItem;
  // Local -> remote; the bridge picks the richest flavour it supports.
  sendItem(item: ClipboardItem): void;
  // Remote -> local files (only called when capabilities().files is true).
  startDownload(item: ClipboardItem): void;
  // Whether cancelling now would leave the remote listing unreadable.
  needsCancelConfirm(): boolean;
  abortTransfer(): void;
}
