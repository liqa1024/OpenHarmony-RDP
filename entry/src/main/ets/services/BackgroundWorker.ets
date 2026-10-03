/*
 * HmRdp - UI-thread facade for the single background worker.
 *
 * Owns the worker instance, matches each reply to its request by id, and routes
 * the worker's unsolicited events to the session window that owns the handle.
 * Binary payloads are always passed through the transfer list (zero-copy), so
 * callers must not touch a buffer after the call. See doc_agent/architecture.md
 * §4.4 and services/BackgroundProtocol.
 */
import { ErrorEvent, MessageEvents, worker } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BackgroundOps, BackgroundEvent, BackgroundRequest, BackgroundResponse } from './BackgroundProtocol';
import { DibImage } from '../utils/ClipboardCodec';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpBg';

type ResponseSink = (response: BackgroundResponse) => void;
type EventSink = (event: BackgroundEvent) => void;

class BackgroundWorkerImpl {
  private thread: worker.ThreadWorker | undefined = undefined;
  private nextId: number = 1;
  private pending: Map<number, ResponseSink> = new Map<number, ResponseSink>();
  // Every session window registers; each sink filters the events by its own
  // current handle, so a retry (which changes the handle) needs no re-register.
  private eventSinks: EventSink[] = [];

  /** Lazily starts the one worker this process uses for background work. */
  private ensure(): worker.ThreadWorker {
    let existing: worker.ThreadWorker | undefined = this.thread;
    if (existing !== undefined) {
      return existing;
    }
    const thread: worker.ThreadWorker =
      new worker.ThreadWorker('entry/ets/workers/background.worker.ets', { name: 'hmrdp-bg' });
    thread.onmessage = (e: MessageEvents): void => {
      const event: BackgroundEvent = e.data as BackgroundEvent;
      if (event.op !== undefined) {
        for (const sink of this.eventSinks) {
          sink(event);
        }
        return;
      }
      const response: BackgroundResponse = e.data as BackgroundResponse;
      const sink: ResponseSink | undefined = this.pending.get(response.id);
      if (sink !== undefined) {
        this.pending.delete(response.id);
        sink(response);
      }
    };
    thread.onerror = (e: ErrorEvent): void => {
      hilog.error(DOMAIN, TAG, 'background worker error: %{public}s', e.message);
      // Settle every outstanding request as a failure so no caller (and no
      // toolbar spinner) waits forever on a worker that has gone away.
      const ids: number[] = [];
      this.pending.forEach((_sink: ResponseSink, id: number): void => {
        ids.push(id);
      });
      for (const id of ids) {
        const sink: ResponseSink | undefined = this.pending.get(id);
        if (sink !== undefined) {
          this.pending.delete(id);
          const failed: BackgroundResponse = { id: id, ok: false };
          sink(failed);
        }
      }
    };
    this.thread = thread;
    return thread;
  }

  private send(request: BackgroundRequest, transfer: ArrayBuffer[]): Promise<BackgroundResponse> {
    const thread: worker.ThreadWorker = this.ensure();
    return new Promise<BackgroundResponse>((resolve: ResponseSink) => {
      this.pending.set(request.id, resolve);
      thread.postMessage(request, transfer);
    });
  }

  /** Registers a session window's worker-event sink (progress/completion). */
  registerSession(sink: EventSink): void {
    this.ensure();
    if (this.eventSinks.indexOf(sink) < 0) {
      this.eventSinks.push(sink);
    }
  }

  unregisterSession(sink: EventSink): void {
    const index: number = this.eventSinks.indexOf(sink);
    if (index >= 0) {
      this.eventSinks.splice(index, 1);
    }
  }

  // --- clipboard codecs (request/reply) ------------------------------------

  /** Decodes a remote CF_DIB to top-down BGRA (the input buffer is transferred). */
  async decodeDib(dib: ArrayBuffer): Promise<DibImage | undefined> {
    const request: BackgroundRequest = {
      id: this.nextId, op: BackgroundOps.DECODE_DIB, dib: dib
    };
    this.nextId++;
    const response: BackgroundResponse = await this.send(request, [dib]);
    if (!response.ok || response.buffer === undefined) {
      return undefined;
    }
    const image: DibImage = new DibImage();
    image.width = response.width ?? 0;
    image.height = response.height ?? 0;
    image.bgra = response.buffer;
    return image;
  }

  /** Converts a remote RTF payload to an HTML fragment. */
  async decodeRtf(rtf: ArrayBuffer): Promise<string> {
    const request: BackgroundRequest = {
      id: this.nextId, op: BackgroundOps.DECODE_RTF, rtf: rtf
    };
    this.nextId++;
    const response: BackgroundResponse = await this.send(request, [rtf]);
    return response.ok ? (response.text ?? '') : '';
  }

  /** Extracts the HTML fragment from a remote CF_HTML payload. */
  async decodeCfHtml(cfHtml: ArrayBuffer): Promise<string> {
    const request: BackgroundRequest = {
      id: this.nextId, op: BackgroundOps.DECODE_CF_HTML, cfHtml: cfHtml
    };
    this.nextId++;
    const response: BackgroundResponse = await this.send(request, [cfHtml]);
    return response.ok ? (response.text ?? '') : '';
  }

  // --- local clipboard -> remote (encode + native push) --------------------

  /** Pushes local text; the native store/advertise runs on the worker. */
  async pushText(handle: number, text: string): Promise<boolean> {
    const request: BackgroundRequest = {
      id: this.nextId, op: BackgroundOps.PUSH_TEXT, handle: handle, text: text
    };
    this.nextId++;
    const response: BackgroundResponse = await this.send(request, []);
    return response.ok;
  }

  /** Encodes local HTML and pushes it (CF_HTML + plain fallback). */
  async pushHtml(handle: number, html: string): Promise<boolean> {
    const request: BackgroundRequest = {
      id: this.nextId, op: BackgroundOps.PUSH_HTML, handle: handle, html: html
    };
    this.nextId++;
    const response: BackgroundResponse = await this.send(request, []);
    return response.ok;
  }

  /** Encodes local pixels (transferred) to CF_DIB + PNG and pushes both. */
  async pushImage(handle: number, pixels: ArrayBuffer, width: number, height: number,
    pixelFormat: number): Promise<boolean> {
    const request: BackgroundRequest = {
      id: this.nextId, op: BackgroundOps.PUSH_IMAGE, handle: handle, pixels: pixels,
      width: width, height: height, pixelFormat: pixelFormat
    };
    this.nextId++;
    const response: BackgroundResponse = await this.send(request, [pixels]);
    return response.ok;
  }

  // --- file download (fire-and-forget) -------------------------------------

  /** Forwards the server's raw FILEGROUPDESCRIPTORW (buffer transferred). */
  postFileList(handle: number, descriptor: ArrayBuffer): void {
    const request: BackgroundRequest = {
      id: 0, op: BackgroundOps.FILE_LIST, handle: handle, data: descriptor
    };
    this.ensure().postMessage(request, [descriptor]);
  }

  /** Forwards one FILECONTENTS response (bytes stay native, read by handle). */
  postFileChunk(handle: number, listIndex: number, isSize: boolean, sizeValue: number,
    failed: boolean, dataHandle: number): void {
    const request: BackgroundRequest = {
      id: 0, op: BackgroundOps.FILE_CHUNK, handle: handle, listIndex: listIndex,
      isSize: isSize, sizeValue: sizeValue, failed: failed, dataHandle: dataHandle
    };
    this.ensure().postMessage(request, []);
  }

  /** The server's clipboard list changed; abort any running download. */
  postClipboardChanged(handle: number): void {
    const request: BackgroundRequest = {
      id: 0, op: BackgroundOps.CLIPBOARD_CHANGED, handle: handle
    };
    this.ensure().postMessage(request, []);
  }

  /** Starts the download into a UI-created sandbox directory. */
  startDownload(handle: number, dir: string): void {
    const request: BackgroundRequest = {
      id: 0, op: BackgroundOps.START_DOWNLOAD, handle: handle, dir: dir
    };
    this.ensure().postMessage(request, []);
  }

  /** Cancels the running download (partial copies are removed by the worker). */
  cancelDownload(handle: number): void {
    const request: BackgroundRequest = {
      id: 0, op: BackgroundOps.CANCEL_DOWNLOAD, handle: handle
    };
    this.ensure().postMessage(request, []);
  }
}

export const BackgroundWorker: BackgroundWorkerImpl = new BackgroundWorkerImpl();
