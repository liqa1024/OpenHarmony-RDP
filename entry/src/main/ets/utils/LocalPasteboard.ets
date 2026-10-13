import { pasteboard, SuppressWarnings, SuppressWarningsType } from '@kit.BasicServicesKit';
import { image } from '@kit.ImageKit';
import { fileIo, fileUri } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ClipboardItem, ClipboardText, LocalReadResult } from '../model/ClipboardModels';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpClipboard';

export interface LocalPasteboardHost {
  toast(message: string): void;
}

export class LocalReadOptions {
  allowFiles: boolean = false;
  allowHtml: boolean = false;
  fileDir: string = '';
}

class FileReadResult {
  ok: boolean = false;
  paths: string[] = [];
  folder: boolean = false;
}

export class LocalPasteboard {
  static hasFileUri(): boolean {
    try {
      return pasteboard.getSystemPasteboard().hasDataType(pasteboard.MIMETYPE_TEXT_URI);
    } catch (error) {
      return false;
    }
  }

  @SuppressWarnings({ rules: [SuppressWarningsType.PERMISSION] })
  static async read(options: LocalReadOptions): Promise<LocalReadResult> {
    const result: LocalReadResult = new LocalReadResult();
    if (options.allowFiles && options.fileDir.length > 0 && LocalPasteboard.hasFileUri()) {
      const files: FileReadResult = await LocalPasteboard.readFiles(options.fileDir);
      if (!files.ok) {
        result.failed = true;
        return result;
      }
      if (files.folder) {
        result.folderRejected = true;
        return result;
      }
      if (files.paths.length > 0) {
        const item: ClipboardItem = new ClipboardItem();
        item.files = files.paths;
        result.item = item;
        return result;
      }
    }
    try {
      const data: pasteboard.PasteData = await pasteboard.getSystemPasteboard().getData();
      result.item = await LocalPasteboard.classify(data, options.allowHtml);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'read pasteboard failed: %{public}s', JSON.stringify(error));
      result.failed = true;
    }
    return result;
  }

  // The system copies clipboard files into our sandbox (no READ_PASTEBOARD permission);
  // a file URI that produced nothing is a folder, which getDataWithProgress cannot copy.
  @SuppressWarnings({ rules: [SuppressWarningsType.PERMISSION] })
  private static async readFiles(dir: string): Promise<FileReadResult> {
    const result: FileReadResult = new FileReadResult();
    const params: pasteboard.GetDataParams = {
      destUri: fileUri.getUriFromPath(dir),
      fileConflictOptions: pasteboard.FileConflictOptions.OVERWRITE,
      progressIndicator: pasteboard.ProgressIndicator.DEFAULT
    };
    try {
      const data: pasteboard.PasteData =
        await pasteboard.getSystemPasteboard().getDataWithProgress(params);
      const paths: string[] = LocalPasteboard.listFiles(dir);
      if (paths.length === 0) {
        result.ok = true;
        result.folder = LocalPasteboard.clipboardUriIsFile(data);
        return result;
      }
      result.ok = true;
      result.paths = paths;
      return result;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'read clipboard files failed: %{public}s', JSON.stringify(error));
      return result;
    }
  }

  private static clipboardUriIsFile(data: pasteboard.PasteData): boolean {
    try {
      const uri: string = data.getPrimaryUri();
      if (uri.length === 0) {
        return false;
      }
      return !uri.startsWith('http://') && !uri.startsWith('https://');
    } catch (error) {
      return false;
    }
  }

  private static async classify(data: pasteboard.PasteData,
    allowHtml: boolean): Promise<ClipboardItem> {
    const item: ClipboardItem = new ClipboardItem();
    let pixelMap: image.PixelMap | undefined = undefined;
    try {
      pixelMap = data.getPrimaryPixelMap();
    } catch (error) {
      pixelMap = undefined;
    }
    if (pixelMap !== undefined) {
      item.image = pixelMap;
      return item;
    }
    if (allowHtml) {
      const html: string = await LocalPasteboard.findHtmlEntry(data);
      if (html.length > 0) {
        item.html = html;
        return item;
      }
    }
    try {
      item.text = data.getPrimaryText();
    } catch (error) {
      item.text = '';
    }
    return item;
  }

  // Do NOT gate on getMimeTypes(): it may only list a record's default MIME,
  // while HTML lives in a non-default Entry.
  private static async findHtmlEntry(data: pasteboard.PasteData): Promise<string> {
    const count: number = data.getRecordCount();
    for (let i = 0; i < count; i++) {
      try {
        const record: pasteboard.PasteDataRecord = data.getRecord(i);
        const value: pasteboard.ValueType = await record.getData(pasteboard.MIMETYPE_TEXT_HTML);
        if (typeof value === 'string' && value.length > 0) {
          return value;
        }
      } catch (error) {
      }
    }
    return '';
  }

  static async write(item: ClipboardItem, label: string, host: LocalPasteboardHost): Promise<boolean> {
    let data: pasteboard.PasteData | undefined = undefined;
    try {
      if (item.hasImage()) {
        const pixelMap: image.PixelMap | undefined = item.image;
        if (pixelMap !== undefined) {
          data = pasteboard.createData(pasteboard.MIMETYPE_PIXELMAP, pixelMap);
        }
      } else if (item.hasFiles()) {
        data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_URI,
          fileUri.getUriFromPath(item.files[0]));
        for (let i = 1; i < item.files.length; i++) {
          data.addRecord(pasteboard.MIMETYPE_TEXT_URI, fileUri.getUriFromPath(item.files[i]));
        }
      } else if (item.hasHtml()) {
        const htmlDoc: string = LocalPasteboard.ensureHtmlDocument(item.html);
        data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_HTML, htmlDoc);
        const plain: string = LocalPasteboard.htmlToPlainText(htmlDoc);
        if (plain.length > 0) {
          // Same Record, extra Entry: one fragment in two formats, so targets
          // do not treat them as separate items.
          try {
            const record: pasteboard.PasteDataRecord = data.getRecord(0);
            record.addEntry(pasteboard.MIMETYPE_TEXT_PLAIN, plain);
          } catch (error) {
            hilog.warn(DOMAIN, TAG, 'add plain entry failed: %{public}s', JSON.stringify(error));
          }
        }
      } else if (item.hasText()) {
        data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, item.text);
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'build pasteboard failed: %{public}s', JSON.stringify(error));
    }
    if (data === undefined) {
      host.toast(ClipboardText.copyFailed(label));
      return false;
    }
    try {
      await pasteboard.getSystemPasteboard().setData(data);
      host.toast(ClipboardText.copied(label));
      return true;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'write pasteboard failed: %{public}s', JSON.stringify(error));
      host.toast(ClipboardText.copyFailed(label));
      return false;
    }
  }

  static htmlToPlainText(html: string): string {
    const lower: string = html.toLowerCase();
    let out: string = '';
    let i: number = 0;
    while (i < html.length) {
      const lt: number = html.indexOf('<', i);
      if (lt < 0) {
        out += html.substring(i);
        break;
      }
      out += html.substring(i, lt);
      const gt: number = html.indexOf('>', lt);
      if (gt < 0) {
        break;
      }
      const tag: string = lower.substring(lt + 1, gt).trim();
      let skipUntil: string = '';
      if (tag.startsWith('script')) {
        skipUntil = '</script';
      } else if (tag.startsWith('style')) {
        skipUntil = '</style';
      } else if (tag.startsWith('head')) {
        skipUntil = '</head';
      } else if (tag.startsWith('!--')) {
        skipUntil = '-->';
      }
      if (skipUntil.length > 0) {
        const close: number = lower.indexOf(skipUntil, gt + 1);
        if (close < 0) {
          break;
        }
        const closeGt: number = html.indexOf('>', close);
        i = closeGt < 0 ? html.length : closeGt + 1;
        continue;
      }
      if (tag.startsWith('br') || tag.startsWith('p') || tag.startsWith('/p') ||
        tag.startsWith('div') || tag.startsWith('/div') || tag.startsWith('li') ||
        tag.startsWith('tr') || tag.startsWith('h1') || tag.startsWith('h2') ||
        tag.startsWith('h3')) {
        out += '\n';
      }
      i = gt + 1;
    }
    return LocalPasteboard.decodeHtmlEntities(out).trim();
  }

  static ensureHtmlDocument(html: string): string {
    if (html.toLowerCase().indexOf('<html') >= 0) {
      return html;
    }
    return '<html><body>' + html + '</body></html>';
  }

  private static decodeHtmlEntities(text: string): string {
    return text.split('&nbsp;').join(' ')
      .split('&amp;').join('&')
      .split('&lt;').join('<')
      .split('&gt;').join('>')
      .split('&quot;').join('"')
      .split('&#39;').join('\'');
  }

  private static listFiles(dir: string): string[] {
    const names: string[] = [];
    try {
      const entries: string[] = fileIo.listFileSync(dir);
      for (const name of entries) {
        if (name.startsWith('.')) {
          continue;
        }
        names.push(dir + '/' + name);
      }
    } catch (error) {
    }
    return names;
  }
}
