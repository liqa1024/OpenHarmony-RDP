export class BackgroundOps {
  static readonly DECODE_DIB: string = 'decodeDib';
  static readonly DECODE_RTF: string = 'decodeRtf';
  static readonly DECODE_CF_HTML: string = 'decodeCfHtml';
  // Fire-and-forget: the native store + advertise must not block the UI.
  static readonly PUSH_TEXT: string = 'pushText';
  static readonly PUSH_HTML: string = 'pushHtml';
  static readonly PUSH_IMAGE: string = 'pushImage';
  static readonly FILE_LIST: string = 'fileList';
  static readonly FILE_CHUNK: string = 'fileChunk';
  static readonly CLIPBOARD_CHANGED: string = 'clipboardChanged';
  static readonly START_DOWNLOAD: string = 'startDownload';
  static readonly CANCEL_DOWNLOAD: string = 'cancelDownload';
}

export class BackgroundEvents {
  static readonly FILE_LIST_SUMMARY: string = 'fileListSummary';
  static readonly DOWNLOAD_PROGRESS: string = 'downloadProgress';
  static readonly DOWNLOAD_DONE: string = 'downloadDone';
  static readonly DOWNLOAD_FAILED: string = 'downloadFailed';
}

export interface BackgroundRequest {
  id: number;
  op: string;
  dib?: ArrayBuffer;
  rtf?: ArrayBuffer;
  cfHtml?: ArrayBuffer;
  text?: string;
  html?: string;
  pixels?: ArrayBuffer;
  width?: number;
  height?: number;
  pixelFormat?: number;
  data?: ArrayBuffer;
  handle?: number;
  dataHandle?: number;
  listIndex?: number;
  isSize?: boolean;
  sizeValue?: number;
  failed?: boolean;
  dir?: string;
}

export interface BackgroundResponse {
  id: number;
  ok: boolean;
  width?: number;
  height?: number;
  text?: string;
  buffer?: ArrayBuffer;
}

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
