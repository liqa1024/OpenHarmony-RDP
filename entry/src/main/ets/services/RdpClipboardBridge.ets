import { BusinessError } from '@kit.BasicServicesKit';
import { image } from '@kit.ImageKit';
import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RdpEventPayload } from 'libhmrdp.so';
import { RdpClipKind, RdpEvent } from '../model/RdpModels';
import { ClipboardCapabilities, ClipboardItem, ClipboardTransferDirection } from '../model/ClipboardModels';
import { ClipboardBridge, ClipboardBridgeSink } from './ClipboardBridge';
import { BackgroundEvent, BackgroundEvents, DecodedImage } from './BackgroundProtocol';
import { BackgroundWorker } from './BackgroundWorker';
import { RdpNative } from './RdpNative';
import { DibImage } from '../utils/ClipboardCodec';
import { FileDescriptorInput, FileTransferCodec } from '../utils/FileTransferCodec';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpRdpClip';

class ClipPick {
  kind: number = RdpClipKind.NONE;
  id: number = 0;
}

// RDP (cliprdr) transport: wire formats, format negotiation and the file stream.
// Inbound is pull-based: a FORMAT_LIST triggers a request for the best format.
export class RdpClipboardBridge implements ClipboardBridge {
  private native: RdpNative;
  private filesDir: string;
  private sink: ClipboardBridgeSink | undefined = undefined;
  private remoteText: string = '';
  private remoteHtml: string = '';
  private remotePixelMap: image.PixelMap | undefined = undefined;
  private remoteFileCount: number = 0;
  private remoteFolderCount: number = 0;
  // Only one data request may be in flight; a newer list is pulled once the
  // current one settles (remote responses carry no format id).
  private formatInFlight: boolean = false;
  private formatPending: boolean = false;
  private pendingId: number = 0;
  private pendingKind: number = RdpClipKind.NONE;
  private transferDirection: number = ClipboardTransferDirection.Download;
  private transferRunning: boolean = false;

  constructor(native: RdpNative, filesDir: string) {
    this.native = native;
    this.filesDir = filesDir;
  }

  capabilities(): ClipboardCapabilities {
    const caps: ClipboardCapabilities = new ClipboardCapabilities();
    caps.text = true;
    caps.html = true;
    caps.image = true;
    caps.files = true;
    return caps;
  }

  attach(sink: ClipboardBridgeSink): void {
    this.sink = sink;
  }

  isBlocked(): boolean {
    return false;
  }

  localStageDir(): string {
    const dir: string | undefined = this.makeSandboxDir('rdp_outbox');
    return dir === undefined ? '' : dir;
  }

  remoteItem(): ClipboardItem {
    const item: ClipboardItem = new ClipboardItem();
    if (this.remoteFileCount > 0 || this.remoteFolderCount > 0) {
      const count: number = this.remoteFileCount > 0 ? this.remoteFileCount : 1;
      for (let i = 0; i < count; i++) {
        item.files.push('');
      }
      return item;
    }
    if (this.remotePixelMap !== undefined) {
      item.image = this.remotePixelMap;
    } else if (this.remoteHtml.length > 0) {
      item.html = this.remoteHtml;
    } else if (this.remoteText.length > 0) {
      item.text = this.remoteText;
    }
    return item;
  }

  sendItem(item: ClipboardItem): void {
    if (item.hasFiles()) {
      const entries: FileDescriptorInput[] = [];
      for (const path of item.files) {
        entries.push(FileDescriptorInput.of(RdpClipboardBridge.baseName(path),
          RdpClipboardBridge.fileSizeOf(path)));
      }
      this.native.pushClipboardFileOffer(FileTransferCodec.buildFileGroupDescriptor(entries),
        item.files);
      return;
    }
    if (item.hasImage()) {
      const pixelMap: image.PixelMap | undefined = item.image;
      if (pixelMap !== undefined) {
        this.pushImage(pixelMap);
      }
      return;
    }
    if (item.hasHtml()) {
      BackgroundWorker.pushHtml(this.native.handleId(), item.html)
        .then((ok: boolean): void => {
          this.message(ok ? '已放入远端剪贴板' : '提交本机富文本失败');
        });
      return;
    }
    if (item.hasText()) {
      BackgroundWorker.pushText(this.native.handleId(), item.text)
        .then((ok: boolean): void => {
          this.message(ok ? '已放入远端剪贴板' : '提交本机文本失败');
        });
    }
  }

  startDownload(item: ClipboardItem): void {
    if (this.remoteFolderCount > 0) {
      // Folders are unsupported, so a list with any directory is refused whole.
      this.message('不支持文件夹传输');
      return;
    }
    const dir: string | undefined = this.makeSandboxDir('rdp_inbox');
    if (dir === undefined) {
      return;
    }
    this.transferDirection = ClipboardTransferDirection.Download;
    this.transferRunning = true;
    if (this.sink !== undefined) {
      this.sink.onTransferProgress(ClipboardTransferDirection.Download, 0, 0, 0, this.remoteFileCount);
    }
    BackgroundWorker.startDownload(this.native.handleId(), dir);
  }

  needsCancelConfirm(): boolean {
    return this.transferRunning && this.transferDirection === ClipboardTransferDirection.Upload;
  }

  abortTransfer(): void {
    if (this.transferDirection === ClipboardTransferDirection.Upload) {
      this.native.cancelFileTransfer();
    } else {
      BackgroundWorker.cancelDownload(this.native.handleId());
    }
    this.transferRunning = false;
    this.message('已取消文件传输');
  }

  reset(): void {
    this.remoteText = '';
    this.remoteHtml = '';
    this.remotePixelMap = undefined;
    this.remoteFileCount = 0;
    this.remoteFolderCount = 0;
    this.formatInFlight = false;
    this.formatPending = false;
    this.pendingId = 0;
    this.pendingKind = RdpClipKind.NONE;
    this.transferRunning = false;
    this.notifyRemote();
  }

  // Returns true when the event belongs to the clipboard channel.
  handleEvent(event: RdpEvent, payload: RdpEventPayload): boolean {
    if (event === RdpEvent.ClipboardText) {
      const text: string = payload.text ?? '';
      hilog.info(DOMAIN, TAG, 'remote clipboard text: %{public}d chars', text.length);
      this.remoteText = text;
      this.remoteHtml = '';
      this.remotePixelMap = undefined;
      this.remoteFileCount = 0;
      this.remoteFolderCount = 0;
      this.notifyRemote();
      return true;
    } else if (event === RdpEvent.ClipboardHtml) {
      this.onRemoteHtml(payload);
      return true;
    } else if (event === RdpEvent.ClipboardRtf) {
      this.onRemoteRtf(payload);
      return true;
    } else if (event === RdpEvent.ClipboardImage) {
      hilog.info(DOMAIN, TAG, 'remote clipboard image (raw DIB)');
      this.remoteFileCount = 0;
      this.remoteFolderCount = 0;
      this.onRemoteImage(payload);
      return true;
    } else if (event === RdpEvent.ClipboardImagePng) {
      hilog.info(DOMAIN, TAG, 'remote clipboard image (PNG)');
      this.remoteFileCount = 0;
      this.remoteFolderCount = 0;
      this.onRemotePng(payload);
      return true;
    } else if (event === RdpEvent.RemoteFileListRaw) {
      this.onRemoteFileListRaw(payload);
      return true;
    } else if (event === RdpEvent.RemoteFileChunk) {
      BackgroundWorker.postFileChunk(this.native.handleId(), payload.listIndex ?? 0,
        payload.isSize ?? false, payload.sizeValue ?? 0, payload.failed ?? false,
        payload.dataHandle ?? 0);
      return true;
    } else if (event === RdpEvent.RemoteClipboardChanged) {
      this.remoteFileCount = 0;
      this.remoteFolderCount = 0;
      BackgroundWorker.postClipboardChanged(this.native.handleId());
      this.notifyRemote();
      return true;
    } else if (event === RdpEvent.FileTransferProgress) {
      this.onFileTransferProgress(payload);
      return true;
    } else if (event === RdpEvent.FileTransferDone) {
      this.transferRunning = false;
      if (this.sink !== undefined) {
        this.sink.onTransferDone();
      }
      this.message('已发送文件到远端');
      return true;
    } else if (event === RdpEvent.FileTransferFailed) {
      this.transferRunning = false;
      const reason: string = payload.reason ?? '';
      if (this.sink !== undefined) {
        this.sink.onTransferFailed(reason);
      }
      return true;
    } else if (event === RdpEvent.RemoteFormatList) {
      this.onRemoteFormatList(payload);
      return true;
    } else if (event === RdpEvent.RemoteFormatResponse) {
      this.onRemoteFormatResponse();
      return true;
    }
    return false;
  }

  onBackgroundEvent(event: BackgroundEvent): void {
    if (event.handle !== this.native.handleId()) {
      return;
    }
    if (event.op === BackgroundEvents.FILE_LIST_SUMMARY) {
      this.remoteFileCount = event.fileCount ?? 0;
      this.remoteFolderCount = event.folderCount ?? 0;
      hilog.info(DOMAIN, TAG, 'remote clipboard files: %{public}d folders=%{public}d total=%{public}d',
        this.remoteFileCount, this.remoteFolderCount, event.totalBytes ?? 0);
      this.notifyRemote();
    } else if (event.op === BackgroundEvents.DOWNLOAD_PROGRESS) {
      this.transferDirection = ClipboardTransferDirection.Download;
      this.transferRunning = true;
      if (this.sink !== undefined) {
        this.sink.onTransferProgress(ClipboardTransferDirection.Download, event.doneBytes ?? 0,
          event.totalBytes ?? 0, event.fileIndex ?? 0, event.fileCount ?? 0);
      }
    } else if (event.op === BackgroundEvents.DOWNLOAD_DONE) {
      this.transferRunning = false;
      if (this.sink !== undefined) {
        this.sink.onDownloadedFiles(event.paths ?? []);
      }
    } else if (event.op === BackgroundEvents.DOWNLOAD_FAILED) {
      this.transferRunning = false;
      const reason: string = event.reason ?? '';
      if (this.sink !== undefined) {
        this.sink.onTransferFailed(reason);
      }
    }
  }

  private onFileTransferProgress(payload: RdpEventPayload): void {
    const direction: number = payload.direction ?? 0;
    const done: number = payload.doneBytes ?? 0;
    this.transferDirection = direction;
    this.transferRunning = true;
    // An upload only advertises the list (native sends a 0% placeholder); keep
    // the "waiting for the remote paste" hint until bytes move.
    if (direction === ClipboardTransferDirection.Upload && done === 0) {
      if (this.sink !== undefined) {
        this.sink.onTransferWaiting(direction, payload.fileIndex ?? 0, payload.fileCount ?? 0);
      }
      return;
    }
    if (this.sink !== undefined) {
      this.sink.onTransferProgress(direction, done, payload.totalBytes ?? 0,
        payload.fileIndex ?? 0, payload.fileCount ?? 0);
    }
  }

  private onRemoteHtml(payload: RdpEventPayload): void {
    const dataHandle: number = payload.dataHandle ?? 0;
    if (dataHandle <= 0) {
      return;
    }
    const cfHtml: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(dataHandle);
    if (cfHtml === undefined) {
      return;
    }
    BackgroundWorker.decodeCfHtml(cfHtml)
      .then((html: string): void => {
        this.setRemoteHtml(html);
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'decode cf_html failed: %{public}s', JSON.stringify(error));
      });
  }

  private onRemoteRtf(payload: RdpEventPayload): void {
    const dataHandle: number = payload.dataHandle ?? 0;
    if (dataHandle <= 0) {
      return;
    }
    const rtf: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(dataHandle);
    if (rtf === undefined) {
      return;
    }
    BackgroundWorker.decodeRtf(rtf)
      .then((html: string): void => {
        this.setRemoteHtml(html);
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'decode rtf failed: %{public}s', JSON.stringify(error));
      });
  }

  private setRemoteHtml(html: string): void {
    this.remoteHtml = html;
    this.remoteText = '';
    this.remotePixelMap = undefined;
    this.remoteFileCount = 0;
    this.remoteFolderCount = 0;
    this.notifyRemote();
  }

  private onRemoteImage(payload: RdpEventPayload): void {
    const imageHandle: number = payload.imageHandle ?? 0;
    if (imageHandle <= 0) {
      return;
    }
    const dib: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(imageHandle);
    if (dib === undefined) {
      return;
    }
    BackgroundWorker.decodeDib(dib)
      .then((parsed: DibImage | undefined): void => {
        if (parsed === undefined || parsed.width <= 0 || parsed.height <= 0) {
          hilog.warn(DOMAIN, TAG, 'unsupported remote DIB');
          return;
        }
        this.setRemotePixelMap(parsed.bgra, parsed.width, parsed.height,
          image.PixelMapFormat.BGRA_8888);
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'decode dib failed: %{public}s', JSON.stringify(error));
      });
  }

  private onRemotePng(payload: RdpEventPayload): void {
    const imageHandle: number = payload.imageHandle ?? 0;
    if (imageHandle <= 0) {
      return;
    }
    const png: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(imageHandle);
    if (png === undefined) {
      return;
    }
    BackgroundWorker.decodePng(png)
      .then((decoded: DecodedImage | undefined): void => {
        if (decoded === undefined || decoded.width <= 0 || decoded.height <= 0) {
          hilog.warn(DOMAIN, TAG, 'unsupported remote PNG');
          return;
        }
        this.setRemotePixelMap(decoded.bgra, decoded.width, decoded.height,
          RdpClipboardBridge.toPixelFormat(decoded.pixelFormat));
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'decode png failed: %{public}s', JSON.stringify(error));
      });
  }

  private setRemotePixelMap(bgra: ArrayBuffer, width: number, height: number,
    format: image.PixelMapFormat): void {
    const options: image.InitializationOptions = {
      editable: false,
      pixelFormat: format,
      size: { width: width, height: height }
    };
    try {
      this.remotePixelMap = image.createPixelMapSync(bgra, options);
      this.remoteText = '';
      this.remoteHtml = '';
      this.notifyRemote();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'create clipboard pixelmap failed: %{public}s',
        JSON.stringify(error));
    }
  }

  private onRemoteFileListRaw(payload: RdpEventPayload): void {
    const dataHandle: number = payload.dataHandle ?? 0;
    if (dataHandle <= 0) {
      return;
    }
    const fgd: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(dataHandle);
    if (fgd === undefined) {
      return;
    }
    this.remoteText = '';
    this.remoteHtml = '';
    this.remotePixelMap = undefined;
    BackgroundWorker.postFileList(this.native.handleId(), fgd);
  }

  private onRemoteFormatList(payload: RdpEventPayload): void {
    const pick: ClipPick | undefined =
      RdpClipboardBridge.pickRemoteFormat(payload.formatCandidates ?? '');
    if (pick === undefined) {
      return;
    }
    if (this.formatInFlight) {
      this.formatPending = true;
      this.pendingId = pick.id;
      this.pendingKind = pick.kind;
      return;
    }
    this.issueRemoteFormat(pick.id, pick.kind);
  }

  private issueRemoteFormat(id: number, kind: number): void {
    this.formatInFlight = this.native.requestClipboardFormat(id, kind);
  }

  private onRemoteFormatResponse(): void {
    this.formatInFlight = false;
    if (this.formatPending) {
      this.formatPending = false;
      this.issueRemoteFormat(this.pendingId, this.pendingKind);
    }
  }

  private static pickRemoteFormat(candidates: string): ClipPick | undefined {
    let best: ClipPick | undefined = undefined;
    let bestRank: number = 99;
    for (const entry of candidates.split(',')) {
      const bar: number = entry.indexOf(':');
      if (bar <= 0) {
        continue;
      }
      const kind: number = Number.parseInt(entry.substring(0, bar), 10);
      const id: number = Number.parseInt(entry.substring(bar + 1), 10);
      if (id <= 0) {
        continue;
      }
      const rank: number = RdpClipboardBridge.clipKindRank(kind);
      if (rank < bestRank) {
        bestRank = rank;
        const pick: ClipPick = new ClipPick();
        pick.kind = kind;
        pick.id = id;
        best = pick;
      }
    }
    return best;
  }

  private static clipKindRank(kind: number): number {
    if (kind === RdpClipKind.FILES) {
      return 0;
    }
    if (kind === RdpClipKind.IMAGE) {
      return 1;
    }
    // DIB is the proven path; PNG only fills in when the server offers no DIB.
    if (kind === RdpClipKind.PNG) {
      return 2;
    }
    if (kind === RdpClipKind.HTML) {
      return 3;
    }
    if (kind === RdpClipKind.RTF) {
      return 4;
    }
    if (kind === RdpClipKind.TEXT) {
      return 5;
    }
    return 99;
  }

  // The pixel read is a PixelMap API (UI thread); CF_DIB + PNG encode and the
  // native push run on the worker.
  private pushImage(pixelMap: image.PixelMap): void {
    pixelMap.getImageInfo()
      .then((info: image.ImageInfo): void => {
        const width: number = info.size.width;
        const height: number = info.size.height;
        if (width <= 0 || height <= 0) {
          this.message('本机图片为空');
          return;
        }
        const buffer: ArrayBuffer = new ArrayBuffer(width * height * 4);
        pixelMap.readPixelsToBuffer(buffer)
          .then((): void => {
            BackgroundWorker.pushImage(this.native.handleId(), buffer, width, height,
              info.pixelFormat)
              .then((pushed: boolean): void => {
                if (!pushed) {
                  hilog.warn(DOMAIN, TAG, 'unsupported clipboard pixel format or dead session: %{public}d',
                    info.pixelFormat);
                  this.message('提交本机图片失败');
                  return;
                }
                this.message('已放入远端剪贴板');
              });
          })
          .catch((error: BusinessError): void => {
            hilog.warn(DOMAIN, TAG, 'read image pixels failed: %{public}s', JSON.stringify(error));
            this.message('提交本机图片失败');
          });
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'get image info failed: %{public}s', JSON.stringify(error));
        this.message('提交本机图片失败');
      });
  }

  private makeSandboxDir(name: string): string | undefined {
    if (this.filesDir.length === 0) {
      this.message('无法访问应用目录');
      return undefined;
    }
    const stamp: string = `${Date.now()}`;
    const root: string = `${this.filesDir}/${name}`;
    const dir: string = `${root}/${stamp}`;
    try {
      fileIo.mkdirSync(dir, true);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'mkdir %{public}s failed: %{public}s', dir, JSON.stringify(error));
      this.message('无法创建文件目录');
      return undefined;
    }
    RdpClipboardBridge.purgeStaleTransferDirs(root, stamp);
    return dir;
  }

  // The clipboard only references the newest transfer directory, so the previous one is purged.
  private static purgeStaleTransferDirs(root: string, keep: string): void {
    let names: string[] = [];
    try {
      names = fileIo.listFileSync(root);
    } catch (error) {
      return;
    }
    for (const name of names) {
      if (name === keep) {
        continue;
      }
      const path: string = `${root}/${name}`;
      try {
        // Unlink contents first, in case rmdirSync does not remove a non-empty directory.
        for (const child of fileIo.listFileSync(path)) {
          try {
            fileIo.unlinkSync(`${path}/${child}`);
          } catch (error) {
          }
        }
        fileIo.rmdirSync(path);
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'purge %{public}s failed: %{public}s', path, JSON.stringify(error));
      }
    }
  }

  private static baseName(path: string): string {
    const slash: number = path.lastIndexOf('/');
    return slash < 0 ? path : path.substring(slash + 1);
  }

  private static fileSizeOf(path: string): number {
    try {
      return fileIo.statSync(path).size;
    } catch (error) {
      return 0;
    }
  }

  private static toPixelFormat(value: number): image.PixelMapFormat {
    if (value === image.PixelMapFormat.RGB_565) {
      return image.PixelMapFormat.RGB_565;
    }
    if (value === image.PixelMapFormat.RGBA_8888) {
      return image.PixelMapFormat.RGBA_8888;
    }
    if (value === image.PixelMapFormat.RGB_888) {
      return image.PixelMapFormat.RGB_888;
    }
    if (value === image.PixelMapFormat.ALPHA_8) {
      return image.PixelMapFormat.ALPHA_8;
    }
    if (value === image.PixelMapFormat.RGBA_F16) {
      return image.PixelMapFormat.RGBA_F16;
    }
    if (value === image.PixelMapFormat.NV21) {
      return image.PixelMapFormat.NV21;
    }
    if (value === image.PixelMapFormat.NV12) {
      return image.PixelMapFormat.NV12;
    }
    return image.PixelMapFormat.BGRA_8888;
  }

  private notifyRemote(): void {
    if (this.sink !== undefined) {
      this.sink.onRemoteChanged();
    }
  }

  private message(text: string): void {
    if (this.sink !== undefined) {
      this.sink.onMessage(text);
    }
  }
}
