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
