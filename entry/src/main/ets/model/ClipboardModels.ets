import { image } from '@kit.ImageKit';

export class ClipboardCapabilities {
  text: boolean = false;
  html: boolean = false;
  image: boolean = false;
  files: boolean = false;
}

export enum ClipboardTransferDirection {
  Download = 0,
  Upload = 1
}

// `files` holds sandbox paths: a remote item carries the not-yet-downloaded
// names, a local item the staged paths handed to the remote.
export class ClipboardItem {
  text: string = '';
  html: string = '';
  image: image.PixelMap | undefined = undefined;
  files: string[] = [];

  hasText(): boolean {
    return this.text.length > 0;
  }

  hasHtml(): boolean {
    return this.html.length > 0;
  }

  hasImage(): boolean {
    return this.image !== undefined;
  }

  hasFiles(): boolean {
    return this.files.length > 0;
  }

  isEmpty(): boolean {
    return !this.hasText() && !this.hasHtml() && !this.hasImage() && !this.hasFiles();
  }
}

export class LocalReadResult {
  item: ClipboardItem | undefined = undefined;
  folderRejected: boolean = false;
  failed: boolean = false;
}

// User-facing clipboard messages, shared so both protocols read identically.
// The verb matches the toolbar button: 粘贴 (local -> remote) / 复制 (remote -> local).
// Convention: conclusion + concrete object, no trailing punctuation (ui-design.md §4.3).
export class ClipboardText {
  static readonly TEXT: string = '文本';
  static readonly HTML: string = '富文本';
  static readonly IMAGE: string = '图片';
  static readonly FILE: string = '文件';
  static readonly REMOTE_UNSUPPORTED: string = '远端不支持剪贴板同步';
  static readonly LOCAL_EMPTY: string = '本机剪贴板为空';
  static readonly REMOTE_EMPTY: string = '远端剪贴板为空';

  static pasted(type: string): string {
    return '已粘贴' + type + '到远端';
  }

  static pasteFailed(type: string): string {
    return '无法粘贴' + type + '到远端';
  }

  static pasteFailedReason(type: string, reason: string): string {
    return '无法粘贴' + type + '到远端：' + reason;
  }

  static copied(type: string): string {
    return '已复制' + type + '到本机';
  }

  static copyFailed(type: string): string {
    return '无法复制' + type + '到本机';
  }
}
