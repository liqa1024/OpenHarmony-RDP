import { util } from '@kit.ArkTS';
import { image } from '@kit.ImageKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { moonlightSendClipboardData } from 'libhmrdp.so';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { ClipboardCapabilities, ClipboardItem, ClipboardText } from '../model/ClipboardModels';
import { ClipboardBridge, ClipboardBridgeSink } from './ClipboardBridge';
import { BackgroundWorker } from './BackgroundWorker';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMlClip';

// Wire frame v1 (little-endian): u8 version, u8 kind, u32 token, u32 length, bytes payload.
const WIRE_VERSION: number = 1;
const KIND_TEXT: number = 1;
const KIND_PNG: number = 2;
const KIND_REF: number = 3;
// At/above this the payload goes out-of-band over HTTPS instead of inline.
const INLINE_THRESHOLD: number = 60000;
const MIME_TEXT: string = 'text/plain';
const MIME_PNG: string = 'image/png';
// Echo suppression: a sent frame's token is remembered briefly.
const TOKEN_TTL_MS: number = 5000;
const TOKEN_CAPACITY: number = 16;

class WireFrame {
  kind: number = 0;
  token: number = 0;
  payload: ArrayBuffer = new ArrayBuffer(0);
}

// Moonlight clipboard transport (foundation-sunshine 0x5508): opaque pushed
// frames, text + single PNG only.
export class MoonlightClipboardBridge implements ClipboardBridge {
  private host: string;
  private httpsPort: number;
  private filesDir: string;
  private sink: ClipboardBridgeSink | undefined = undefined;
  private remoteText: string = '';
  private remoteImage: image.PixelMap | undefined = undefined;
  private pendingToken: number = 0;
  private sentTokens: Map<number, number> = new Map<number, number>();
  // Inbound frames are processed strictly in order (compound pairing and blob
  // fetches must not race each other).
  private queue: Promise<void> = Promise.resolve();
  private busy: boolean = false;

  constructor(host: string, httpsPort: number, filesDir: string) {
    this.host = host;
    this.httpsPort = httpsPort;
    this.filesDir = filesDir;
  }

  capabilities(): ClipboardCapabilities {
    const caps: ClipboardCapabilities = new ClipboardCapabilities();
    caps.text = true;
    caps.image = true;
    caps.html = false;
    caps.files = false;
    return caps;
  }

  attach(sink: ClipboardBridgeSink): void {
    this.sink = sink;
  }

  isBlocked(): boolean {
    return this.busy;
  }

  localStageDir(): string {
    return '';
  }

  remoteItem(): ClipboardItem {
    const item: ClipboardItem = new ClipboardItem();
    if (this.remoteImage !== undefined) {
      item.image = this.remoteImage;
      item.text = this.remoteText;
    } else if (this.remoteText.length > 0) {
      item.text = this.remoteText;
    }
    return item;
  }

  sendItem(item: ClipboardItem): void {
    if (item.hasImage()) {
      const pixelMap: image.PixelMap | undefined = item.image;
      if (pixelMap !== undefined) {
        this.sendImage(pixelMap);
        return;
      }
    }
    if (item.hasText()) {
      this.sendText(item.text);
    }
  }

  startDownload(item: ClipboardItem): void {
    // Unreachable: capabilities().files is false.
  }

  needsCancelConfirm(): boolean {
    return false;
  }

  abortTransfer(): void {
  }

  onIncoming(bytes: ArrayBuffer): void {
    this.queue = this.queue.then((): Promise<void> => this.processFrame(bytes));
  }

  private async processFrame(bytes: ArrayBuffer): Promise<void> {
    const frame: WireFrame | undefined = MoonlightClipboardBridge.parseFrame(bytes);
    if (frame === undefined) {
      return;
    }
    if (frame.token !== 0 && this.isRecentToken(frame.token)) {
      return;
    }
    this.busy = true;
    try {
      if (frame.kind === KIND_REF) {
        await this.applyRef(frame);
      } else if (frame.kind === KIND_TEXT) {
        this.applyText(frame.token, MoonlightClipboardBridge.decodeUtf8(frame.payload));
      } else if (frame.kind === KIND_PNG) {
        await this.applyPng(frame.token, frame.payload);
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'clipboard frame failed: %{public}s', JSON.stringify(error));
    } finally {
      this.busy = false;
    }
  }

  private applyText(token: number, text: string): void {
    if (text.length === 0) {
      return;
    }
    this.remoteText = text;
    this.remoteImage = undefined;
    this.pendingToken = token;
    this.notify();
  }

  private async applyPng(token: number, png: ArrayBuffer): Promise<void> {
    const pixelMap: image.PixelMap | undefined = await this.decodePng(png);
    if (pixelMap === undefined) {
      return;
    }
    this.remoteImage = pixelMap;
    if (token === 0 || token !== this.pendingToken) {
      // Standalone image (or a new group): the text no longer belongs to it.
      this.remoteText = '';
    }
    this.pendingToken = 0;
    this.notify();
  }

  private async applyRef(frame: WireFrame): Promise<void> {
    const json: string = MoonlightClipboardBridge.decodeUtf8(frame.payload);
    const id: string = MoonlightClipboardBridge.readJsonField(json, 'id');
    const mime: string = MoonlightClipboardBridge.readJsonField(json, 'mime');
    if (id.length === 0) {
      return;
    }
    const bytes: ArrayBuffer | undefined =
      await MoonlightClient.getClipboardBlob(this.filesDir, this.host, this.httpsPort, id);
    if (bytes === undefined) {
      return;
    }
    if (mime.startsWith('text/')) {
      this.applyText(frame.token, MoonlightClipboardBridge.decodeUtf8(bytes));
    } else if (mime === MIME_PNG) {
      await this.applyPng(frame.token, bytes);
    }
  }

  private async sendImage(pixelMap: image.PixelMap): Promise<void> {
    try {
      const info: image.ImageInfo = await pixelMap.getImageInfo();
      const width: number = info.size.width;
      const height: number = info.size.height;
      if (width <= 0 || height <= 0) {
        this.message(ClipboardText.LOCAL_EMPTY);
        return;
      }
      const buffer: ArrayBuffer = new ArrayBuffer(width * height * 4);
      await pixelMap.readPixelsToBuffer(buffer);
      const png: ArrayBuffer | undefined =
        await BackgroundWorker.encodePng(buffer, width, height, info.pixelFormat);
      if (png === undefined || png.byteLength === 0) {
        this.message(ClipboardText.pasteFailed(ClipboardText.IMAGE));
        return;
      }
      await this.sendPayload(ClipboardText.IMAGE, KIND_PNG, png, MIME_PNG);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'send image failed: %{public}s', JSON.stringify(error));
      this.message(ClipboardText.pasteFailed(ClipboardText.IMAGE));
    }
  }

  private async sendText(text: string): Promise<void> {
    const bytes: ArrayBuffer = MoonlightClipboardBridge.encodeUtf8(text);
    await this.sendPayload(ClipboardText.TEXT, KIND_TEXT, bytes, MIME_TEXT);
  }

  private async sendPayload(type: string, kind: number, payload: ArrayBuffer,
    mime: string): Promise<void> {
    const token: number = MoonlightClipboardBridge.newToken();
    this.rememberToken(token);
    if (payload.byteLength <= INLINE_THRESHOLD) {
      this.sendFrame(type, kind, token, payload);
      return;
    }
    const id: string | undefined =
      await MoonlightClient.postClipboardBlob(this.filesDir, this.host, this.httpsPort, payload, mime);
    if (id === undefined) {
      this.message(ClipboardText.pasteFailed(type));
      return;
    }
    const json: string = `{"id":"${id}","mime":"${mime}","size":${payload.byteLength}}`;
    this.sendFrame(type, KIND_REF, token, MoonlightClipboardBridge.encodeUtf8(json));
  }

  private sendFrame(type: string, kind: number, token: number, payload: ArrayBuffer): void {
    const frame: ArrayBuffer = MoonlightClipboardBridge.buildFrame(kind, token, payload);
    const code: number = moonlightSendClipboardData(frame);
    if (code === 0) {
      this.message(ClipboardText.pasted(type));
      return;
    }
    hilog.warn(DOMAIN, TAG, 'send clipboard frame failed: %{public}d', code);
    this.message(code === -2
      ? ClipboardText.pasteFailedReason(type, ClipboardText.REMOTE_UNSUPPORTED)
      : ClipboardText.pasteFailed(type));
  }

  private notify(): void {
    if (this.sink !== undefined) {
      this.sink.onRemoteChanged();
    }
  }

  private message(text: string): void {
    if (this.sink !== undefined) {
      this.sink.onMessage(text);
    }
  }

  private isRecentToken(token: number): boolean {
    const at: number | undefined = this.sentTokens.get(token);
    return at !== undefined && Date.now() - at < TOKEN_TTL_MS;
  }

  private rememberToken(token: number): void {
    this.sentTokens.set(token, Date.now());
    // Drop expired tokens; the sent flood is tiny, so a scan is fine.
    const expired: number[] = [];
    this.sentTokens.forEach((at: number, key: number): void => {
      if (Date.now() - at >= TOKEN_TTL_MS) {
        expired.push(key);
      }
    });
    for (const key of expired) {
      this.sentTokens.delete(key);
    }
    if (this.sentTokens.size > TOKEN_CAPACITY) {
      let oldestKey: number = -1;
      let oldestAt: number = Date.now();
      this.sentTokens.forEach((at: number, key: number): void => {
        if (at < oldestAt) {
          oldestAt = at;
          oldestKey = key;
        }
      });
      if (oldestKey >= 0) {
        this.sentTokens.delete(oldestKey);
      }
    }
  }

  private async decodePng(png: ArrayBuffer): Promise<image.PixelMap | undefined> {
    try {
      const source: image.ImageSource = image.createImageSource(png);
      const options: image.DecodingOptions = { editable: false };
      const pixelMap: image.PixelMap = await source.createPixelMap(options);
      source.release();
      return pixelMap;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'decode png failed: %{public}s', JSON.stringify(error));
      return undefined;
    }
  }

  private static buildFrame(kind: number, token: number, payload: ArrayBuffer): ArrayBuffer {
    const total: number = 10 + payload.byteLength;
    const out: ArrayBuffer = new ArrayBuffer(total);
    const view: DataView = new DataView(out);
    view.setUint8(0, WIRE_VERSION);
    view.setUint8(1, kind);
    view.setUint32(2, token, true);
    view.setUint32(6, payload.byteLength, true);
    new Uint8Array(out, 10).set(new Uint8Array(payload));
    return out;
  }

  private static parseFrame(bytes: ArrayBuffer): WireFrame | undefined {
    if (bytes.byteLength < 10) {
      return undefined;
    }
    const view: DataView = new DataView(bytes);
    if (view.getUint8(0) !== WIRE_VERSION) {
      return undefined;
    }
    const frame: WireFrame = new WireFrame();
    frame.kind = view.getUint8(1);
    frame.token = view.getUint32(2, true);
    const length: number = view.getUint32(6, true);
    if (10 + length > bytes.byteLength) {
      return undefined;
    }
    frame.payload = bytes.slice(10, 10 + length);
    return frame;
  }

  private static encodeUtf8(text: string): ArrayBuffer {
    const bytes: Uint8Array = new util.TextEncoder().encodeInto(text);
    const out: ArrayBuffer = new ArrayBuffer(bytes.length);
    new Uint8Array(out).set(bytes);
    return out;
  }

  private static decodeUtf8(bytes: ArrayBuffer): string {
    try {
      return util.TextDecoder.create('utf-8').decodeToString(new Uint8Array(bytes));
    } catch (error) {
      return '';
    }
  }

  private static readJsonField(json: string, field: string): string {
    const key: string = `"${field}"`;
    const at: number = json.indexOf(key);
    if (at < 0) {
      return '';
    }
    const colon: number = json.indexOf(':', at + key.length);
    if (colon < 0) {
      return '';
    }
    const open: number = json.indexOf('"', colon + 1);
    if (open < 0) {
      return '';
    }
    const close: number = json.indexOf('"', open + 1);
    if (close < 0) {
      return '';
    }
    return json.substring(open + 1, close);
  }

  private static newToken(): number {
    return (Math.floor(Math.random() * 0x7FFFFFFF) + 1) >>> 0;
  }
}
