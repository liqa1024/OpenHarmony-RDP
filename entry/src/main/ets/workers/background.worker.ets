/*
 * HmRdp - the single background worker.
 *
 * Non-realtime work that would otherwise block the UI thread runs here: the
 * clipboard DIB/RTF/HTML codecs and the file-download state machine. It is
 * deliberately not the audio thread - a realtime audio renderer callback must
 * not queue behind a large image decode or a file write, so audio gets its own
 * worker (see doc_agent/architecture.md §4.4).
 *
 * Clipboard codecs are request/reply (correlated by id); file-download traffic
 * is fire-and-forget commands/events (id 0) answered by unsolicited events.
 * Binary payloads travel through the postMessage transfer list, so neither the
 * input nor the result is copied.
 */
import { MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import { ClipboardCodec, DibImage } from '../utils/ClipboardCodec';
import { BackgroundOps, BackgroundEvent, BackgroundRequest, BackgroundResponse } from '../services/BackgroundProtocol';
import { FileDownloader } from './FileDownloader';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

const downloads: FileDownloader = new FileDownloader((event: BackgroundEvent): void => {
  workerPort.postMessage(event, []);
});

function handleCodec(req: BackgroundRequest): void {
  const res: BackgroundResponse = { id: req.id, ok: false };
  if (req.op === BackgroundOps.DECODE_DIB && req.dib !== undefined) {
    const parsed: DibImage | undefined = ClipboardCodec.parseDib(req.dib);
    if (parsed !== undefined) {
      res.ok = true;
      res.width = parsed.width;
      res.height = parsed.height;
      res.buffer = parsed.bgra;
    }
  } else if (req.op === BackgroundOps.DECODE_RTF && req.rtf !== undefined) {
    res.ok = true;
    res.text = ClipboardCodec.parseRtf(req.rtf);
  } else if (req.op === BackgroundOps.DECODE_CF_HTML && req.cfHtml !== undefined) {
    res.ok = true;
    res.text = ClipboardCodec.parseCfHtml(req.cfHtml);
  } else if (req.op === BackgroundOps.BUILD_DIB && req.pixels !== undefined) {
    const dib: ArrayBuffer | undefined = ClipboardCodec.buildDib(req.pixels,
      req.width ?? 0, req.height ?? 0, req.pixelFormat ?? 0);
    if (dib !== undefined) {
      res.ok = true;
      res.buffer = dib;
    }
  } else if (req.op === BackgroundOps.BUILD_CF_HTML) {
    res.ok = true;
    res.buffer = ClipboardCodec.buildCfHtml(req.html ?? '');
  }
  const transfer: ArrayBuffer[] = [];
  if (res.buffer !== undefined) {
    transfer.push(res.buffer);
  }
  workerPort.postMessage(res, transfer);
}

function handleFileOp(req: BackgroundRequest): void {
  const handle: number = req.handle ?? 0;
  if (req.op === BackgroundOps.FILE_LIST && req.data !== undefined) {
    downloads.onFileList(handle, req.data);
  } else if (req.op === BackgroundOps.FILE_CHUNK) {
    downloads.onChunk(req);
  } else if (req.op === BackgroundOps.CLIPBOARD_CHANGED) {
    downloads.onClipboardChanged(handle);
  } else if (req.op === BackgroundOps.START_DOWNLOAD) {
    downloads.start(handle, req.dir ?? '');
  } else if (req.op === BackgroundOps.CANCEL_DOWNLOAD) {
    downloads.cancel(handle);
  }
}

workerPort.onmessage = (e: MessageEvents) => {
  const req: BackgroundRequest = e.data as BackgroundRequest;
  if (req.id > 0) {
    handleCodec(req);
    return;
  }
  handleFileOp(req);
};
