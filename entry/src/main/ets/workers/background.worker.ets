/*
 * HmRdp - the single background worker.
 *
 * Non-realtime work that would otherwise block the UI thread runs here: the
 * clipboard DIB/RTF/HTML/PNG codecs, the local clipboard encode + native push,
 * and the file-download state machine. It is deliberately not the audio thread -
 * a realtime audio renderer callback must not queue behind a large image decode,
 * a file write, or a clipboard channel send, so audio gets its own worker (see
 * doc_agent/architecture.md §4.4).
 *
 * Correlated requests (id > 0) are the clipboard codec / push calls; id 0 is a
 * fire-and-forget event/command (file download). Binary payloads travel through
 * the postMessage transfer list, so neither the input nor the result is copied.
 */
import { MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import { image } from '@kit.ImageKit';
import { setClipboardHtmlRaw, setClipboardImage, setClipboardText } from 'libhmrdp.so';
import { ClipboardCodec, DibImage } from '../utils/ClipboardCodec';
import { BackgroundOps, BackgroundEvent, BackgroundRequest, BackgroundResponse } from '../services/BackgroundProtocol';
import { FileDownloader } from './FileDownloader';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

const downloads: FileDownloader = new FileDownloader((event: BackgroundEvent): void => {
  workerPort.postMessage(event, []);
});

/**
 * Encodes the same pixels to a registered "PNG" payload. Best-effort: any
 * failure (or a device without the encoder in this context) just means the push
 * falls back to CF_DIB alone.
 */
async function encodePng(pixels: ArrayBuffer, width: number, height: number,
  pixelFormat: number): Promise<ArrayBuffer | undefined> {
  if (width <= 0 || height <= 0) {
    return undefined;
  }
  try {
    const options: image.InitializationOptions = {
      editable: false,
      pixelFormat: pixelFormat,
      size: { width: width, height: height }
    };
    const bitmap: image.PixelMap = image.createPixelMapSync(pixels, options);
    const packer: image.ImagePacker = image.createImagePacker();
    const packing: image.PackingOption = { format: 'image/png', quality: 100 };
    const png: ArrayBuffer = await packer.packToData(bitmap, packing);
    packer.release();
    bitmap.release();
    return png;
  } catch (err) {
    return undefined;
  }
}

/**
 * Clipboard requests: decode a remote payload, or encode a local one and push it
 * natively. The push must run here, not on the UI thread: the native store copy
 * (a whole image) and the advertise (a channel send on the calling thread) can
 * block, and a large image was the worst case on a slow link.
 */
async function handleRequest(req: BackgroundRequest): Promise<void> {
  const res: BackgroundResponse = { id: req.id, ok: false };
  const handle: number = req.handle ?? 0;
  try {
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
    } else if (req.op === BackgroundOps.PUSH_TEXT && req.text !== undefined) {
      // The native store + advertise is what makes the remote able to paste, so
      // the reply carries its success (a dead session must not report "sent").
      res.ok = setClipboardText(handle, req.text);
    } else if (req.op === BackgroundOps.PUSH_HTML) {
      const html: string = req.html ?? '';
      res.ok = setClipboardHtmlRaw(handle, ClipboardCodec.buildCfHtml(html),
        ClipboardCodec.stripHtmlToText(html));
    } else if (req.op === BackgroundOps.PUSH_IMAGE && req.pixels !== undefined) {
      const width: number = req.width ?? 0;
      const height: number = req.height ?? 0;
      const format: number = req.pixelFormat ?? 0;
      const dib: ArrayBuffer | undefined = ClipboardCodec.buildDib(req.pixels, width, height, format);
      if (dib !== undefined) {
        const png: ArrayBuffer | undefined = await encodePng(req.pixels, width, height, format);
        res.ok = setClipboardImage(handle, dib, png ?? new ArrayBuffer(0));
      }
    }
  } catch (err) {
    res.ok = false;
  }
  const transfer: ArrayBuffer[] = [];
  if (res.buffer !== undefined) {
    transfer.push(res.buffer);
  }
  workerPort.postMessage(res, transfer);
}

/** File-download events/commands (fire-and-forget; answers are BackgroundEvents). */
function handleEvent(req: BackgroundRequest): void {
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
    handleRequest(req);
    return;
  }
  handleEvent(req);
};
