/*
 * HmRdp - message shapes for the single background worker (workers/background.worker.ets).
 *
 * The worker owns non-realtime heavy work: the clipboard wire-format codecs and,
 * since P1.5, the file-download state machine plus its file IO. The UI thread
 * talks to it with plain-object messages; every binary payload (an input buffer
 * and a result buffer) travels through the postMessage transfer list, so neither
 * side copies it. See doc_agent/architecture.md §4.4.
 */

/** Requested operations (UI -> worker). */
export class BackgroundOps {
  // Clipboard codecs (request/reply, correlated by id).
  static readonly DECODE_DIB: string = 'decodeDib';
  static readonly DECODE_RTF: string = 'decodeRtf';
  static readonly DECODE_CF_HTML: string = 'decodeCfHtml';
  static readonly BUILD_DIB: string = 'buildDib';
  static readonly BUILD_CF_HTML: string = 'buildCfHtml';
  // File download (fire-and-forget events / commands; the worker answers with a
  // BackgroundEvent, not a reply).
  static readonly FILE_LIST: string = 'fileList';
  static readonly FILE_CHUNK: string = 'fileChunk';
  static readonly CLIPBOARD_CHANGED: string = 'clipboardChanged';
  static readonly START_DOWNLOAD: string = 'startDownload';
  static readonly CANCEL_DOWNLOAD: string = 'cancelDownload';
}

/** Worker-originated notifications (worker -> UI). */
export class BackgroundEvents {
  static readonly FILE_LIST_SUMMARY: string = 'fileListSummary';
  static readonly DOWNLOAD_PROGRESS: string = 'downloadProgress';
  static readonly DOWNLOAD_DONE: string = 'downloadDone';
  static readonly DOWNLOAD_FAILED: string = 'downloadFailed';
}

/**
 * One request. `id > 0` expects a `BackgroundResponse` with the same id; `id == 0`
 * is a fire-and-forget command/event (the worker never replies by id). A binary
 * field listed in the postMessage transfer list must not be touched afterwards.
 */
export interface BackgroundRequest {
  id: number;
  op: string;
  // Clipboard codecs.
  dib?: ArrayBuffer;
  rtf?: ArrayBuffer;
  cfHtml?: ArrayBuffer;
  html?: string;
  pixels?: ArrayBuffer;
  width?: number;
  height?: number;
  pixelFormat?: number;
  // File download (handle-addressed). `data` carries the raw descriptor bytes
  // for FILE_LIST (transferred); chunk bytes stay native and are read by handle.
  data?: ArrayBuffer;
  handle?: number;
  dataHandle?: number;
  listIndex?: number;
  isSize?: boolean;
  sizeValue?: number;
  failed?: boolean;
  dir?: string;
}

/**
 * One reply to a correlated request. `ok` is false for an unsupported/malformed
 * input. `buffer` is the transferred result (BGRA for decodeDib, a CF_DIB /
 * CF_HTML payload for the builders); `text` carries a decoded string;
 * width/height describe the image.
 */
export interface BackgroundResponse {
  id: number;
  ok: boolean;
  width?: number;
  height?: number;
  text?: string;
  buffer?: ArrayBuffer;
}

/**
 * One unsolicited notification, addressed to a session handle. A session window
 * ignores events for other handles (several windows share the one worker).
 */
export interface BackgroundEvent {
  op: string;
  handle: number;
  fileCount?: number;
  folderCount?: number;
  totalBytes?: number;
  direction?: number;
  doneBytes?: number;
  fileIndex?: number;
  paths?: string[];
  reason?: string;
}
