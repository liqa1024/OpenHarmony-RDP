/*
 * HmRdp - remote-file download state machine, worker side.
 *
 * Runs on the single background worker so the UI thread never does the per-chunk
 * file IO. The native side only relays each FILECONTENTS response as a buffer
 * handle (process-global, shared with the worker) plus scalar fields; this class
 * owns the descriptor list, offset/size bookkeeping, the sandbox file handles and
 * the one-request-in-flight sequencing, and reports progress/completion/failure
 * back to the UI through `emit`. Behaviour mirrors the state machine it replaces
 * (doc_agent/session-and-input.md §4.1): files stream into a per-transfer sandbox
 * directory, a missing descriptor size is resolved with a SIZE request first, and
 * a list that advertises any folder is refused whole (folders are unsupported in
 * both directions).
 */
import { fileIo } from '@kit.CoreFileKit';
import { bufferRead, bufferRelease, requestRemoteFileContents } from 'libhmrdp.so';
import { FileDescriptorInput, FileTransferCodec } from '../utils/FileTransferCodec';
import { BackgroundEvent, BackgroundRequest } from '../services/BackgroundProtocol';

const FILE_CHUNK_BYTES: number = 256 * 1024;

class DownloadState {
  files: FileDescriptorInput[] = [];
  paths: string[] = [];
  total: number = 0;
  index: number = 0;
  offset: number = 0;
  size: number = 0;
  sizeKnown: boolean = false;
  file: fileIo.File | undefined = undefined;
  done: number = 0;
  active: boolean = false;
}

export class FileDownloader {
  private states: Map<number, DownloadState> = new Map<number, DownloadState>();
  private emit: (event: BackgroundEvent) => void;

  constructor(emit: (event: BackgroundEvent) => void) {
    this.emit = emit;
  }

  private state(handle: number): DownloadState {
    let found: DownloadState | undefined = this.states.get(handle);
    if (found === undefined) {
      found = new DownloadState();
      this.states.set(handle, found);
    }
    return found;
  }

  /** Parses a raw FILEGROUPDESCRIPTORW (a transferred copy) and caches it. */
  onFileList(handle: number, descriptor: ArrayBuffer): void {
    const state: DownloadState = this.state(handle);
    state.files = FileTransferCodec.parseFileGroupDescriptor(descriptor);
    let fileCount: number = 0;
    let folderCount: number = 0;
    let totalBytes: number = 0;
    for (const entry of state.files) {
      if (entry.isDirectory) {
        folderCount++;
      } else {
        fileCount++;
      }
      totalBytes += entry.size;
    }
    const event: BackgroundEvent = {
      op: 'fileListSummary', handle: handle, fileCount: fileCount,
      folderCount: folderCount, totalBytes: totalBytes
    };
    this.emit(event);
  }

  /** One FILECONTENTS response relayed by native. */
  onChunk(request: BackgroundRequest): void {
    const handle: number = request.handle ?? 0;
    const dataHandle: number = request.dataHandle ?? 0;
    const state: DownloadState = this.state(handle);
    if (!state.active) {
      // Late response after a cancel/abort: release its buffer, do not write.
      if (dataHandle > 0) {
        bufferRelease(dataHandle);
      }
      return;
    }
    if (request.failed ?? false) {
      if (dataHandle > 0) {
        bufferRelease(dataHandle);
      }
      this.abort(handle, (request.isSize ?? false) ? '文件大小响应无效' : '远端拒绝提供文件内容');
      return;
    }
    if (request.isSize ?? false) {
      const size: number = request.sizeValue ?? 0;
      state.size = size;
      state.sizeKnown = true;
      if (state.index < state.files.length) {
        const info: FileDescriptorInput = state.files[state.index];
        state.total = state.total - info.size + size;
        info.size = size;
        info.sizeKnown = true;
      }
      this.publishProgress(handle);
      this.next(handle);
      return;
    }
    if (dataHandle <= 0) {
      // An empty range means the server has nothing more for this file.
      state.offset = state.size;
      this.next(handle);
      return;
    }
    const bytes: ArrayBuffer | undefined = bufferRead(dataHandle);
    bufferRelease(dataHandle);
    const file: fileIo.File | undefined = state.file;
    if (bytes === undefined || file === undefined) {
      this.abort(handle, '写入文件失败');
      return;
    }
    try {
      const written: number = fileIo.writeSync(file.fd, bytes);
      if (written !== bytes.byteLength) {
        this.abort(handle, '写入文件失败');
        return;
      }
      state.offset += written;
      state.done += written;
    } catch (error) {
      this.abort(handle, '写入文件失败');
      return;
    }
    this.publishProgress(handle);
    this.next(handle);
  }

  /** The server's clipboard changed: an in-flight download can never finish. */
  onClipboardChanged(handle: number): void {
    const state: DownloadState = this.state(handle);
    if (state.active) {
      this.abort(handle, '远端剪贴板已改变，传输已中止');
    }
  }

  /** Starts a download into the given (UI-created) sandbox directory. */
  start(handle: number, dir: string): void {
    const state: DownloadState = this.state(handle);
    if (state.active) {
      return;
    }
    if (state.files.length === 0) {
      this.fail(handle, '没有可下载的文件');
      return;
    }
    // Folders are not supported in either direction: a list that advertises any
    // directory is refused whole.
    for (const file of state.files) {
      if (file.isDirectory) {
        this.fail(handle, '不支持文件夹传输');
        return;
      }
    }
    state.paths = [];
    state.total = 0;
    for (const file of state.files) {
      state.paths.push(`${dir}/${file.name}`);
      state.total += file.size;
    }
    state.active = true;
    state.index = 0;
    state.offset = 0;
    state.size = 0;
    state.sizeKnown = false;
    state.file = undefined;
    state.done = 0;
    this.publishProgress(handle);
    this.next(handle);
  }

  /** User cancelled the download; clean partials and report it as a failure. */
  cancel(handle: number): void {
    const state: DownloadState = this.state(handle);
    if (state.active) {
      this.abort(handle, '已取消文件传输');
    }
  }

  /**
   * Drives the download: skip directories, resolve a missing size with a SIZE
   * request, then pull the next RANGE. One request is in flight at a time (the
   * response carries no listIndex, so native tags it from the pending request).
   */
  private next(handle: number): void {
    const state: DownloadState = this.state(handle);
    for (;;) {
      if (!state.active) {
        return;
      }
      if (state.index >= state.files.length) {
        this.finish(handle);
        return;
      }
      const info: FileDescriptorInput = state.files[state.index];
      const path: string = state.paths[state.index];
      if (!state.sizeKnown && info.sizeKnown) {
        state.size = info.size;
        state.sizeKnown = true;
      }
      if (!state.sizeKnown) {
        // The descriptor carried no FD_FILESIZE: ask for the size first.
        if (!requestRemoteFileContents(handle, state.index, true, 0, 8)) {
          this.abort(handle, '文件传输请求失败');
        }
        return;
      }
      if (state.file === undefined) {
        try {
          state.file = fileIo.openSync(path, fileIo.OpenMode.CREATE
            | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC);
          state.offset = 0;
        } catch (error) {
          this.abort(handle, '无法创建文件');
          return;
        }
      }
      if (state.offset >= state.size) {
        // Current file complete (also covers a zero-byte file).
        this.closeFile(state);
        state.index++;
        this.resetFile(state);
        continue;
      }
      const remaining: number = state.size - state.offset;
      const chunk: number = remaining < FILE_CHUNK_BYTES ? remaining : FILE_CHUNK_BYTES;
      if (!requestRemoteFileContents(handle, state.index, false, state.offset, chunk)) {
        this.abort(handle, '文件传输请求失败');
      }
      return;
    }
  }

  private publishProgress(handle: number): void {
    const state: DownloadState = this.state(handle);
    let fileCount: number = 0;
    for (const file of state.files) {
      if (!file.isDirectory) {
        fileCount++;
      }
    }
    const event: BackgroundEvent = {
      op: 'downloadProgress', handle: handle, direction: 0,
      doneBytes: state.done, totalBytes: state.total,
      fileIndex: state.index + 1, fileCount: fileCount
    };
    this.emit(event);
  }

  private finish(handle: number): void {
    const state: DownloadState = this.state(handle);
    this.closeFile(state);
    const paths: string[] = state.paths;
    state.active = false;
    state.paths = [];
    const event: BackgroundEvent = { op: 'downloadDone', handle: handle, paths: paths };
    this.emit(event);
  }

  /** Aborts the download and removes this transfer's partial copies. */
  private abort(handle: number, reason: string): void {
    const state: DownloadState = this.state(handle);
    this.closeFile(state);
    for (const path of state.paths) {
      if (path.length === 0) {
        continue;
      }
      try {
        fileIo.unlinkSync(path);
      } catch (error) {
        // Already gone or never created: nothing to clean.
      }
    }
    state.active = false;
    state.paths = [];
    this.fail(handle, reason);
  }

  private fail(handle: number, reason: string): void {
    const event: BackgroundEvent = { op: 'downloadFailed', handle: handle, reason: reason };
    this.emit(event);
  }

  private closeFile(state: DownloadState): void {
    if (state.file !== undefined) {
      try {
        fileIo.closeSync(state.file);
      } catch (error) {
        // Nothing useful to do; the transfer is ending anyway.
      }
      state.file = undefined;
    }
  }

  private resetFile(state: DownloadState): void {
    state.offset = 0;
    state.size = 0;
    state.sizeKnown = false;
  }
}
