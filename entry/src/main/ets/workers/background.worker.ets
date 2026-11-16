import { MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import { image } from '@kit.ImageKit';
import { setClipboardHtmlRaw, setClipboardImage, setClipboardText } from 'libhmrdp.so';
import { ClipboardCodec, DibImage } from '../utils/ClipboardCodec';
import { BackgroundOps, BackgroundEvent, BackgroundRequest, BackgroundResponse, DecodedImage } from '../services/BackgroundProtocol';
import { FileDownloader } from './FileDownloader';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

const downloads: FileDownloader = new FileDownloader((event: BackgroundEvent): void => {
  workerPort.postMessage(event, []);
});

// Best-effort: any failure (or a device without the encoder in this context)
// just means the push falls back to CF_DIB alone.
// `pixels` is BGRA_8888; createPixelMapSync's default `srcPixelFormat` is BGRA
// too, but declare both formats so the two can never drift apart.
async function encodePng(pixels: ArrayBuffer, width: number, height: number): Promise<ArrayBuffer | undefined> {
  if (width <= 0 || height <= 0) {
    return undefined;
  }
  try {
    const options: image.InitializationOptions = {
      editable: false,
      srcPixelFormat: image.PixelMapFormat.BGRA_8888,
      pixelFormat: image.PixelMapFormat.RGBA_8888,
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

// Runs on the worker so a large decode never stalls the UI thread. Read through
// the area API: it is the one read that always yields BGRA_8888.
async function decodePng(png: ArrayBuffer): Promise<DecodedImage | undefined> {
  try {
    const source: image.ImageSource = image.createImageSource(png);
    const info: image.ImageInfo = await source.getImageInfo();
    const width: number = info.size.width;
    const height: number = info.size.height;
    if (width <= 0 || height <= 0) {
      source.release();
      return undefined;
    }
    const options: image.DecodingOptions = { editable: false };
    const bitmap: image.PixelMap = await source.createPixelMap(options);
    source.release();
    const area: image.PositionArea = {
      pixels: new ArrayBuffer(width * height * 4),
      offset: 0,
      stride: width * 4,
      region: { size: { width: width, height: height }, x: 0, y: 0 }
    };
    await bitmap.readPixels(area);
    const decoded: DecodedImage = new DecodedImage();
    decoded.width = width;
    decoded.height = height;
    decoded.bgra = area.pixels;
    bitmap.release();
    return decoded;
  } catch (err) {
    return undefined;
  }
}

// The push must run here, not on the UI thread: the native store copy and the
// advertise (a channel send) can both block on a whole-screen image.
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
    } else if (req.op === BackgroundOps.ENCODE_PNG && req.pixels !== undefined) {
      const png: ArrayBuffer | undefined = await encodePng(req.pixels, req.width ?? 0,
        req.height ?? 0);
      if (png !== undefined && png.byteLength > 0) {
        res.ok = true;
        res.buffer = png;
      }
    } else if (req.op === BackgroundOps.DECODE_PNG && req.png !== undefined) {
      const decoded: DecodedImage | undefined = await decodePng(req.png);
      if (decoded !== undefined) {
        res.ok = true;
        res.width = decoded.width;
        res.height = decoded.height;
        res.buffer = decoded.bgra;
      }
    } else if (req.op === BackgroundOps.PUSH_TEXT && req.text !== undefined) {
      // The native store + advertise is what makes the remote able to paste, so
      // the reply carries its success (a dead session must not report "sent").
      res.ok = setClipboardText(handle, req.text);
    } else if (req.op === BackgroundOps.PUSH_HTML) {
      // The plain half is prepared on the UI thread (the platform HTML
      // conversion is stage-model only); an empty one just leaves the remote
      // with nothing to paste as text.
      const html: string = req.html ?? '';
      res.ok = setClipboardHtmlRaw(handle, ClipboardCodec.buildCfHtml(html), req.plain ?? '');
    } else if (req.op === BackgroundOps.PUSH_IMAGE && req.pixels !== undefined) {
      const width: number = req.width ?? 0;
      const height: number = req.height ?? 0;
      const dib: ArrayBuffer | undefined = ClipboardCodec.buildDib(req.pixels, width, height);
      if (dib !== undefined) {
        const png: ArrayBuffer | undefined = await encodePng(req.pixels, width, height);
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
  try {
    workerPort.postMessage(res, transfer);
  } catch (error) {
  }
}

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
