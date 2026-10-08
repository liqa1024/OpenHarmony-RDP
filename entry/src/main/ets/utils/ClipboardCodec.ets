/*
 * HmRdp - clipboard wire-format codecs (ArkTS side).
 *
 * The native clipboard channel only moves bytes; the format encoders/decoders
 * live here. This keeps protocol/format logic on the ArkTS side and the native
 * side a thin transport (doc_agent/architecture.md): the local-side encoders
 * (CF_DIB for images, CF_HTML + plain-text fallback for HTML) and the remote-side
 * decoders (CF_DIB, CF_HTML, RTF).
 */
import { util } from '@kit.ArkTS';

/** HarmonyOS image.PixelMapFormat values the clipboard image path handles. */
const PIXEL_FORMAT_ARGB_8888: number = 1;
const PIXEL_FORMAT_RGBA_8888: number = 3;
const PIXEL_FORMAT_BGRA_8888: number = 4;
const PIXEL_FORMAT_RGB_888: number = 5;

const BYTE_SPACE: number = 0x20;
const BYTE_GT: number = 0x3E;

const HTML_START_FRAGMENT: string = '<!--StartFragment-->';
const HTML_END_FRAGMENT: string = '<!--EndFragment-->';

function utf8(text: string): Uint8Array {
  return new util.TextEncoder().encodeInto(text);
}

function concatBytes(parts: Uint8Array[]): Uint8Array {
  let total: number = 0;
  for (const part of parts) {
    total += part.length;
  }
  const out: Uint8Array = new Uint8Array(total);
  let offset: number = 0;
  for (const part of parts) {
    out.set(part, offset);
    offset += part.length;
  }
  return out;
}

/**
 * ASCII case-insensitive search over UTF-8 bytes; the needles used here only
 * contain letters and punctuation whose bit 0x20 is already set, so ORing it is
 * safe.
 */
function findIgnoreCase(haystack: Uint8Array, needle: Uint8Array, from: number = 0): number {
  if (needle.length === 0 || haystack.length < needle.length) {
    return -1;
  }
  for (let i = from; i + needle.length <= haystack.length; i++) {
    let j: number = 0;
    while (j < needle.length) {
      if ((haystack[i + j] | BYTE_SPACE) !== (needle[j] | BYTE_SPACE)) {
        break;
      }
      j++;
    }
    if (j === needle.length) {
      return i;
    }
  }
  return -1;
}

function indexOfByte(haystack: Uint8Array, byte: number, from: number): number {
  for (let i = from; i < haystack.length; i++) {
    if (haystack[i] === byte) {
      return i;
    }
  }
  return -1;
}

/** Zero-pads a non-negative offset to the CF_HTML fixed 10-digit field. */
function pad10(value: number): string {
  return value.toString().padStart(10, '0');
}

/** Scales a channel extracted with `mask` from `value` up to 8 bits. */
function scaleMask(value: number, mask: number): number {
  if (mask === 0) {
    return 255;
  }
  let shift: number = 0;
  let shifted: number = mask >>> 0;
  while ((shifted & 1) === 0 && shift < 32) {
    shifted >>>= 1;
    shift++;
  }
  let bits: number = 0;
  while ((shifted & 1) !== 0 && bits < 32) {
    shifted >>>= 1;
    bits++;
  }
  if (bits <= 0) {
    return 255;
  }
  const raw: number = ((value & mask) >>> shift) >>> 0;
  if (bits >= 8) {
    return (raw >>> (bits - 8)) & 0xFF;
  }
  const maxValue: number = (1 << bits) - 1;
  return Math.floor((raw * 255 + Math.floor(maxValue / 2)) / maxValue) & 0xFF;
}

/** Decoded CF_DIB: top-down BGRA pixels plus their dimensions. */
export class DibImage {
  width: number = 0;
  height: number = 0;
  bgra: ArrayBuffer = new ArrayBuffer(0);
}

function indexOfBytes(haystack: Uint8Array, needle: Uint8Array, from: number): number {
  if (needle.length === 0 || haystack.length < needle.length) {
    return -1;
  }
  for (let i = from; i + needle.length <= haystack.length; i++) {
    let j: number = 0;
    while (j < needle.length && haystack[i + j] === needle[j]) {
      j++;
    }
    if (j === needle.length) {
      return i;
    }
  }
  return -1;
}

/** Reads a fixed-width decimal field ("Key:0000001234") from a byte header. */
function readOffset(header: Uint8Array, key: string): number {
  const keyBytes: Uint8Array = utf8(key);
  const pos: number = indexOfBytes(header, keyBytes, 0);
  if (pos < 0) {
    return -1;
  }
  let begin: number = pos + keyBytes.length;
  while (begin < header.length && header[begin] === 0x20) {
    begin++;
  }
  let end: number = begin;
  while (end < header.length && header[end] >= 0x30 && header[end] <= 0x39) {
    end++;
  }
  if (end === begin) {
    return -1;
  }
  let value: number = 0;
  for (let i = begin; i < end; i++) {
    value = value * 10 + (header[i] - 0x30);
  }
  return value;
}

function decodeUtf8(bytes: Uint8Array): string {
  try {
    return util.TextDecoder.create('utf-8').decodeToString(bytes);
  } catch (error) {
    return '';
  }
}

function isAlphaByte(b: number): boolean {
  return (b >= 0x41 && b <= 0x5A) || (b >= 0x61 && b <= 0x7A);
}

function hexVal(b: number): number {
  if (b >= 0x30 && b <= 0x39) {
    return b - 0x30;
  }
  if (b >= 0x61 && b <= 0x66) {
    return b - 0x61 + 10;
  }
  if (b >= 0x41 && b <= 0x46) {
    return b - 0x41 + 10;
  }
  return 0;
}

function byteHex(value: number): string {
  const v: number = value & 0xFF;
  const hex: string = v.toString(16).toUpperCase();
  return v < 16 ? '0' + hex : hex;
}

function rtfColorHex(r: number, g: number, b: number): string {
  return '#' + byteHex(r) + byteHex(g) + byteHex(b);
}

/** Run formatting of the RTF group currently open. */
class RtfRunStyle {
  bold: boolean = false;
  italic: boolean = false;
  underline: boolean = false;
  strike: boolean = false;
  colorIndex: number = 0;
  highlightIndex: number = 0;
  fontSizeHalf: number = 0;
}

function cloneRtfStyle(style: RtfRunStyle): RtfRunStyle {
  const copy: RtfRunStyle = new RtfRunStyle();
  copy.bold = style.bold;
  copy.italic = style.italic;
  copy.underline = style.underline;
  copy.strike = style.strike;
  copy.colorIndex = style.colorIndex;
  copy.highlightIndex = style.highlightIndex;
  copy.fontSizeHalf = style.fontSizeHalf;
  return copy;
}

function rtfStyleToCss(style: RtfRunStyle, colors: string[]): string {
  let css: string = '';
  if (style.bold) {
    css += 'font-weight:bold;';
  }
  if (style.italic) {
    css += 'font-style:italic;';
  }
  let decoration: string = '';
  if (style.underline) {
    decoration += 'underline ';
  }
  if (style.strike) {
    decoration += 'line-through ';
  }
  if (decoration.length > 0) {
    css += 'text-decoration:' + decoration + ';';
  }
  if (style.colorIndex > 0 && style.colorIndex < colors.length
    && colors[style.colorIndex].length > 0) {
    css += 'color:' + colors[style.colorIndex] + ';';
  }
  if (style.highlightIndex > 0 && style.highlightIndex < colors.length
    && colors[style.highlightIndex].length > 0) {
    css += 'background-color:' + colors[style.highlightIndex] + ';';
  }
  if (style.fontSizeHalf > 0) {
    css += 'font-size:' + (style.fontSizeHalf / 2).toString() + 'pt;';
  }
  return css;
}

/** Parses the RTF color table; index 0 means "auto" (no explicit color). */
function parseRtfColorTable(bytes: Uint8Array): string[] {
  const colors: string[] = [];
  const pos: number = indexOfBytes(bytes, utf8('\\colortbl'), 0);
  if (pos < 0) {
    return colors;
  }
  let r: number = -1;
  let g: number = -1;
  let b: number = -1;
  let depth: number = 0;
  let i: number = pos + 9;
  while (i < bytes.length) {
    const c: number = bytes[i];
    if (c === 0x7B) {
      depth++;
    } else if (c === 0x7D) {
      if (depth === 0) {
        break;
      }
      depth--;
    } else if (c === 0x3B) {
      colors.push(r >= 0 ? rtfColorHex(r, g, b) : '');
      r = -1;
      g = -1;
      b = -1;
    } else if (c === 0x5C) {
      let j: number = i + 1;
      let word: string = '';
      while (j < bytes.length && isAlphaByte(bytes[j])) {
        word += String.fromCharCode(bytes[j]);
        j++;
      }
      let value: number = 0;
      while (j < bytes.length && bytes[j] >= 0x30 && bytes[j] <= 0x39) {
        value = value * 10 + (bytes[j] - 0x30);
        j++;
      }
      if (word === 'red') {
        r = value;
      } else if (word === 'green') {
        g = value;
      } else if (word === 'blue') {
        b = value;
      }
      i = j - 1;
    }
    i++;
  }
  return colors;
}

export class ClipboardCodec {
  /**
   * Serialises a HarmonyOS PixelMap buffer (top-down, `pixelFormat`) into a
   * CF_DIB payload: a 40-byte BITMAPINFOHEADER (BI_RGB, 32bpp) followed by
   * bottom-up BGRA rows. Returns undefined for an unsupported format or a short
   * buffer.
   */
  static buildDib(pixels: ArrayBuffer, width: number, height: number,
    pixelFormat: number): ArrayBuffer | undefined {
    if (width <= 0 || height <= 0) {
      return undefined;
    }
    const count: number = width * height;
    const src: Uint8Array = new Uint8Array(pixels);
    const bgra: Uint8Array = new Uint8Array(count * 4);
    if (pixelFormat === PIXEL_FORMAT_RGBA_8888) {
      if (src.byteLength < count * 4) {
        return undefined;
      }
      for (let i = 0; i < count; i++) {
        const s: number = i * 4;
        bgra[s] = src[s + 2];
        bgra[s + 1] = src[s + 1];
        bgra[s + 2] = src[s];
        bgra[s + 3] = src[s + 3];
      }
    } else if (pixelFormat === PIXEL_FORMAT_ARGB_8888
      || pixelFormat === PIXEL_FORMAT_BGRA_8888) {
      if (src.byteLength < count * 4) {
        return undefined;
      }
      bgra.set(src.subarray(0, count * 4));
    } else if (pixelFormat === PIXEL_FORMAT_RGB_888) {
      if (src.byteLength < count * 3) {
        return undefined;
      }
      for (let i = 0; i < count; i++) {
        const s: number = i * 3;
        bgra[i * 4] = src[s + 2];
        bgra[i * 4 + 1] = src[s + 1];
        bgra[i * 4 + 2] = src[s];
        bgra[i * 4 + 3] = 255;
      }
    } else {
      return undefined;
    }
    const stride: number = width * 4;
    const dib: ArrayBuffer = new ArrayBuffer(40 + stride * height);
    const header: DataView = new DataView(dib);
    header.setUint32(0, 40, true);
    header.setUint32(4, width, true);
    header.setUint32(8, height, true);
    header.setUint16(12, 1, true);
    header.setUint16(14, 32, true);
    header.setUint32(16, 0, true);
    header.setUint32(20, stride * height, true);
    const out: Uint8Array = new Uint8Array(dib);
    for (let y = 0; y < height; y++) {
      const rowStart: number = (height - 1 - y) * stride;
      out.set(bgra.subarray(rowStart, rowStart + stride), 40 + y * stride);
    }
    return dib;
  }

  /**
   * Builds a CF_HTML payload around `input` ("HTML Clipboard Format", MSDN): an
   * ASCII header of byte offsets followed by the document. The offsets are
   * UTF-8 byte counts, so all work happens on bytes. A document is kept whole
   * (fragment markers point inside <body>), a bare fragment is wrapped, and an
   * input that already carries markers keeps them. Byte-for-byte the payload the
   * native encoder produced.
   */
  static buildCfHtml(input: string): ArrayBuffer {
    const inputBytes: Uint8Array = utf8(input);
    const startMarker: Uint8Array = utf8(HTML_START_FRAGMENT);
    const endMarker: Uint8Array = utf8(HTML_END_FRAGMENT);
    const headerLen: number = utf8('Version:0.9\r\n'
      + 'StartHTML:0000000000\r\n'
      + 'EndHTML:0000000000\r\n'
      + 'StartFragment:0000000000\r\n'
      + 'EndFragment:0000000000\r\n').length;

    let document: Uint8Array;
    let fragmentOffset: number = 0;
    let fragmentLength: number = 0;

    const existingStart: number = findIgnoreCase(inputBytes, startMarker);
    const existingEnd: number = findIgnoreCase(inputBytes, endMarker);
    if (existingStart >= 0 && existingEnd > existingStart + startMarker.length) {
      document = inputBytes;
      fragmentOffset = existingStart + startMarker.length;
      fragmentLength = existingEnd - fragmentOffset;
    } else {
      const isDocument: boolean = findIgnoreCase(inputBytes, utf8('<html')) >= 0
        || findIgnoreCase(inputBytes, utf8('<!doctype')) >= 0
        || findIgnoreCase(inputBytes, utf8('<body')) >= 0;
      if (isDocument) {
        const bodyOpen: number = findIgnoreCase(inputBytes, utf8('<body'));
        let bodyInner: number = -1;
        if (bodyOpen >= 0) {
          const tagEnd: number = indexOfByte(inputBytes, BYTE_GT, bodyOpen);
          if (tagEnd >= 0) {
            bodyInner = tagEnd + 1;
          }
        }
        const bodyClose: number = bodyInner >= 0
          ? findIgnoreCase(inputBytes, utf8('</body>'), bodyInner) : -1;
        if (bodyInner >= 0 && bodyClose >= 0) {
          const fragment: Uint8Array = inputBytes.subarray(bodyInner, bodyClose);
          document = concatBytes([inputBytes.subarray(0, bodyInner), startMarker,
            fragment, endMarker, inputBytes.subarray(bodyClose)]);
          fragmentOffset = bodyInner + startMarker.length;
          fragmentLength = fragment.length;
        } else {
          document = concatBytes([startMarker, inputBytes, endMarker]);
          fragmentOffset = startMarker.length;
          fragmentLength = inputBytes.length;
        }
      } else {
        const bodyStart: Uint8Array = utf8('<html><body>' + HTML_START_FRAGMENT);
        document = concatBytes([bodyStart, inputBytes, endMarker, utf8('</body></html>')]);
        fragmentOffset = bodyStart.length;
        fragmentLength = inputBytes.length;
      }
    }

    const startHtml: number = headerLen;
    const endHtml: number = headerLen + document.length;
    const startFragment: number = headerLen + fragmentOffset;
    const endFragment: number = startFragment + fragmentLength;
    const header: Uint8Array = utf8('Version:0.9\r\n'
      + 'StartHTML:' + pad10(startHtml) + '\r\n'
      + 'EndHTML:' + pad10(endHtml) + '\r\n'
      + 'StartFragment:' + pad10(startFragment) + '\r\n'
      + 'EndFragment:' + pad10(endFragment) + '\r\n');
    const full: Uint8Array = concatBytes([header, document]);
    return full.buffer as ArrayBuffer;
  }

  /**
   * Minimal HTML -> plain text fallback for the CF_UNICODETEXT half of an HTML
   * push. Not a renderer: it only needs to be readable in a plain text target
   * (tag names map to newlines, then the common entities are unescaped).
   */
  static stripHtmlToText(html: string): string {
    let out: string = '';
    for (let i = 0; i < html.length; i++) {
      if (html[i] !== '<') {
        out += html[i];
        continue;
      }
      const close: number = html.indexOf('>', i);
      if (close < 0) {
        break;
      }
      const tag: string = html.substring(i + 1, close);
      const first: string = tag.length === 0
        ? '' : String.fromCharCode(tag.charCodeAt(0) | BYTE_SPACE);
      if (first === 'b' && tag.length >= 2 && (tag[1] === 'r' || tag[1] === 'R')) {
        out += '\n';
      } else if (first === 'p' || first === 'd' || first === 'l' || first === 't') {
        out += '\n';
      }
      i = close;
    }
    const codes: string[] = ['&amp;', '&lt;', '&gt;', '&quot;', '&#39;', '&nbsp;'];
    const chars: string[] = ['&', '<', '>', '"', '\'', ' '];
    for (let k = 0; k < codes.length; k++) {
      out = out.split(codes[k]).join(chars[k]);
    }
    return out;
  }

  /**
   * Parses a CF_DIB into top-down BGRA. Handles the layouts Windows actually
   * sends: BITMAPINFOHEADER / V4 / V5 headers, 16/24/32bpp, BI_RGB and
   * BI_BITFIELDS (channel masks; BGRX default for 32bpp BI_RGB). Returns
   * undefined for an unsupported or truncated payload.
   */
  static parseDib(dib: ArrayBuffer): DibImage | undefined {
    const size: number = dib.byteLength;
    if (size < 40) {
      return undefined;
    }
    const view: DataView = new DataView(dib);
    const bytes: Uint8Array = new Uint8Array(dib);
    const headerSize: number = view.getUint32(0, true);
    const widthRaw: number = view.getInt32(4, true);
    const heightRaw: number = view.getInt32(8, true);
    const bpp: number = view.getUint16(14, true);
    const compression: number = view.getUint32(16, true);
    if (headerSize < 40 || headerSize > size || widthRaw <= 0 || heightRaw === 0) {
      return undefined;
    }
    if (bpp !== 16 && bpp !== 24 && bpp !== 32) {
      return undefined;
    }
    const w: number = widthRaw;
    const h: number = heightRaw < 0 ? -heightRaw : heightRaw;
    if (w === 0 || h === 0 || w > 16384 || h > 16384) {
      return undefined;
    }
    const topDown: boolean = heightRaw < 0;
    const stride: number = Math.floor((w * bpp + 31) / 32) * 4;
    let redMask: number = 0;
    let greenMask: number = 0;
    let blueMask: number = 0;
    let alphaMask: number = 0;
    let pixelOffset: number = headerSize;
    if (compression === 0) {
      if (bpp === 32) {
        redMask = 0x00FF0000;
        greenMask = 0x0000FF00;
        blueMask = 0x000000FF;
      } else if (bpp === 16) {
        redMask = 0x7C00;
        greenMask = 0x03E0;
        blueMask = 0x001F;
      }
    } else if (compression === 3 || compression === 6) {
      if (size < 40 + 12) {
        return undefined;
      }
      redMask = view.getUint32(40, true);
      greenMask = view.getUint32(44, true);
      blueMask = view.getUint32(48, true);
      if (compression === 6 && size >= 40 + 16) {
        alphaMask = view.getUint32(52, true);
      }
      pixelOffset = headerSize >= 108 ? headerSize : (40 + (compression === 6 ? 16 : 12));
    } else {
      return undefined;
    }
    if (pixelOffset + stride * h > size) {
      return undefined;
    }
    const bgra: Uint8Array = new Uint8Array(w * h * 4);
    let anyAlpha: boolean = false;
    for (let y = 0; y < h; y++) {
      const srcY: number = topDown ? y : (h - 1 - y);
      const row: number = pixelOffset + srcY * stride;
      const dstRow: number = y * w * 4;
      for (let x = 0; x < w; x++) {
        let r: number = 0;
        let g: number = 0;
        let b: number = 0;
        let a: number = 255;
        if (bpp === 24) {
          const px: number = row + x * 3;
          b = bytes[px];
          g = bytes[px + 1];
          r = bytes[px + 2];
        } else if (bpp === 32) {
          const px: number = view.getUint32(row + x * 4, true);
          r = scaleMask(px, redMask);
          g = scaleMask(px, greenMask);
          b = scaleMask(px, blueMask);
          if (alphaMask !== 0) {
            a = scaleMask(px, alphaMask);
          }
        } else {
          const px: number = view.getUint16(row + x * 2, true);
          r = scaleMask(px, redMask);
          g = scaleMask(px, greenMask);
          b = scaleMask(px, blueMask);
          if (alphaMask !== 0) {
            a = scaleMask(px, alphaMask);
          }
        }
        if (a !== 0) {
          anyAlpha = true;
        }
        const d: number = dstRow + x * 4;
        bgra[d] = b;
        bgra[d + 1] = g;
        bgra[d + 2] = r;
        bgra[d + 3] = a;
      }
    }
    if (alphaMask !== 0 && !anyAlpha) {
      for (let i = 3; i < bgra.length; i += 4) {
        bgra[i] = 255;
      }
    }
    const out: DibImage = new DibImage();
    out.width = w;
    out.height = h;
    out.bgra = bgra.buffer as ArrayBuffer;
    return out;
  }

  /**
   * Extracts the HTML fragment from a CF_HTML payload. Preference order: the
   * marked fragment (self-contained: inline styles + tags), then the
   * StartFragment/EndFragment offsets, then the whole StartHTML..EndHTML
   * document, then the raw payload decoded as UTF-8.
   */
  static parseCfHtml(cfHtml: ArrayBuffer): string {
    const bytes: Uint8Array = new Uint8Array(cfHtml);
    const startMarker: Uint8Array = utf8(HTML_START_FRAGMENT);
    const endMarker: Uint8Array = utf8(HTML_END_FRAGMENT);
    const markerBegin: number = indexOfBytes(bytes, startMarker, 0);
    const markerEnd: number = indexOfBytes(bytes, endMarker, 0);
    if (markerBegin >= 0 && markerEnd > markerBegin + startMarker.length) {
      const begin: number = markerBegin + startMarker.length;
      return decodeUtf8(bytes.subarray(begin, markerEnd));
    }
    const headerLimit: number = bytes.length < 512 ? bytes.length : 512;
    const header: Uint8Array = bytes.subarray(0, headerLimit);
    const startFragment: number = readOffset(header, 'StartFragment:');
    const endFragment: number = readOffset(header, 'EndFragment:');
    if (startFragment >= 0 && endFragment > startFragment && endFragment <= bytes.length) {
      return decodeUtf8(bytes.subarray(startFragment, endFragment));
    }
    const startHtml: number = readOffset(header, 'StartHTML:');
    const endHtml: number = readOffset(header, 'EndHTML:');
    if (startHtml >= 0 && endHtml > startHtml && endHtml <= bytes.length) {
      return decodeUtf8(bytes.subarray(startHtml, endHtml));
    }
    return decodeUtf8(bytes);
  }

  /**
   * Converts a CF_RTF payload into a simple HTML fragment. Many Windows apps
   * only offer RTF (no HTML) for rich text, so this keeps the local clipboard
   * rich. Operates on the raw bytes (mirroring the native RtfToHtml it
   * replaces): control words map to bold/italic/underline/strike/color/size,
   * destinations are skipped, and `\uN` / `\'XX` are UTF-8 encoded.
   */
  static parseRtf(rtf: ArrayBuffer): string {
    const bytes: Uint8Array = new Uint8Array(rtf);
    const len: number = bytes.length;
    const colors: string[] = parseRtfColorTable(bytes);
    const destinations: string[] = ['fonttbl', 'colortbl', 'stylesheet', 'info',
      'pict', 'header', 'footer', 'footerl', 'footerr', 'footnote', 'listtable',
      'listoverridetable', 'xmlnstbl', 'generator', 'themedata', 'colorschememapping',
      'latentstyles', 'datastore', 'field', 'fldinst', 'bkmkstart', 'bkmkend',
      'object', 'shppict', 'nonshppict'];

    const out: number[] = [];
    const push = (s: string): void => {
      for (let k = 0; k < s.length; k++) {
        out.push(s.charCodeAt(k));
      }
    };
    const pushByte = (b: number): void => {
      out.push(b & 0xFF);
    };
    const appendUtf8 = (codepoint: number): void => {
      const cp: number = codepoint < 0 ? codepoint + 0x10000 : codepoint;
      if (cp <= 0) {
        return;
      }
      if (cp < 0x80) {
        pushByte(cp);
      } else if (cp < 0x800) {
        pushByte(0xC0 | (cp >> 6));
        pushByte(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        pushByte(0xE0 | (cp >> 12));
        pushByte(0x80 | ((cp >> 6) & 0x3F));
        pushByte(0x80 | (cp & 0x3F));
      } else {
        pushByte(0xF0 | (cp >> 18));
        pushByte(0x80 | ((cp >> 12) & 0x3F));
        pushByte(0x80 | ((cp >> 6) & 0x3F));
        pushByte(0x80 | (cp & 0x3F));
      }
    };
    const escapePush = (c: number): void => {
      if (c === 0x26) {
        push('&amp;');
      } else if (c === 0x3C) {
        push('&lt;');
      } else if (c === 0x3E) {
        push('&gt;');
      } else {
        pushByte(c);
      }
    };

    push('<p>');
    let openCss: string = '';
    let style: RtfRunStyle = new RtfRunStyle();
    const styleStack: RtfRunStyle[] = [cloneRtfStyle(style)];
    const skipStack: boolean[] = [false];

    const skipping = (): boolean => {
      for (let k = skipStack.length - 1; k >= 0; k--) {
        if (skipStack[k]) {
          return true;
        }
      }
      return false;
    };
    const closeRun = (): void => {
      if (openCss.length > 0) {
        push('</span>');
        openCss = '';
      }
    };
    const ensureRun = (): void => {
      const css: string = rtfStyleToCss(style, colors);
      if (css === openCss) {
        return;
      }
      if (openCss.length > 0) {
        push('</span>');
      }
      openCss = css;
      if (css.length > 0) {
        push('<span style="' + css + '">');
      }
    };

    for (let i = 0; i < len; i++) {
      const c: number = bytes[i];
      if (c === 0x7B) {
        styleStack.push(cloneRtfStyle(style));
        skipStack.push(false);
        let j: number = i + 1;
        while (j < len && (bytes[j] === 0x0D || bytes[j] === 0x0A || bytes[j] === 0x20)) {
          j++;
        }
        if (j < len && bytes[j] === 0x5C) {
          let k: number = j + 1;
          if (k < len && bytes[k] === 0x2A) {
            skipStack[skipStack.length - 1] = true;
          } else {
            let word: string = '';
            while (k < len && isAlphaByte(bytes[k])) {
              word += String.fromCharCode(bytes[k]);
              k++;
            }
            for (const dest of destinations) {
              if (word === dest) {
                skipStack[skipStack.length - 1] = true;
                break;
              }
            }
          }
        }
        continue;
      }
      if (c === 0x7D) {
        closeRun();
        const restored: RtfRunStyle | undefined = styleStack.pop();
        if (restored !== undefined) {
          style = restored;
        }
        if (skipStack.length > 0) {
          skipStack.pop();
        }
        continue;
      }
      if (c === 0x5C) {
        i++;
        if (i >= len) {
          break;
        }
        const nc: number = bytes[i];
        if (nc === 0x5C || nc === 0x7B || nc === 0x7D) {
          if (!skipping()) {
            ensureRun();
            pushByte(nc);
          }
          continue;
        }
        if (nc === 0x27) {
          if (i + 2 < len) {
            const value: number = hexVal(bytes[i + 1]) * 16 + hexVal(bytes[i + 2]);
            if (!skipping()) {
              ensureRun();
              appendUtf8(value);
            }
            i += 2;
          }
          continue;
        }
        if (nc === 0x7E) {
          if (!skipping()) {
            ensureRun();
            push('&nbsp;');
          }
          continue;
        }
        if (nc === 0x2D || nc === 0x5F) {
          continue;
        }
        if (nc === 0x2A) {
          skipStack[skipStack.length - 1] = true;
          continue;
        }
        let j: number = i;
        let word: string = '';
        while (j < len && isAlphaByte(bytes[j])) {
          word += String.fromCharCode(bytes[j]);
          j++;
        }
        let negative: boolean = false;
        if (j < len && bytes[j] === 0x2D) {
          negative = true;
          j++;
        }
        let value: number = 0;
        let hasValue: boolean = false;
        while (j < len && bytes[j] >= 0x30 && bytes[j] <= 0x39) {
          value = value * 10 + (bytes[j] - 0x30);
          j++;
          hasValue = true;
        }
        if (negative) {
          value = -value;
        }
        i = (j < len && bytes[j] === 0x20) ? j : j - 1;
        if (skipping()) {
          continue;
        }
        if (word === 'b' || word === 'ab') {
          style.bold = hasValue ? (value !== 0) : true;
        } else if (word === 'i' || word === 'ai') {
          style.italic = hasValue ? (value !== 0) : true;
        } else if (word === 'ul') {
          style.underline = hasValue ? (value !== 0) : true;
        } else if (word === 'ulnone') {
          style.underline = false;
        } else if (word === 'strike' || word === 'striked') {
          style.strike = true;
        } else if (word === 'cf') {
          style.colorIndex = value;
        } else if (word === 'highlight') {
          style.highlightIndex = value;
        } else if (word === 'fs') {
          style.fontSizeHalf = value;
        } else if (word === 'plain') {
          style = new RtfRunStyle();
        } else if (word === 'par' || word === 'pard') {
          closeRun();
          push('</p><p>');
        } else if (word === 'line') {
          closeRun();
          push('<br>');
        } else if (word === 'tab') {
          ensureRun();
          pushByte(0x09);
        } else if (word === 'u') {
          ensureRun();
          appendUtf8(value);
          // Skip the single `\uc` fallback character that follows \uN.
          const fallback: number = i + 1;
          if (fallback + 2 < len && bytes[fallback] === 0x5C && bytes[fallback + 1] === 0x27) {
            i = fallback + 3;
          } else if (fallback < len) {
            i = fallback;
          }
        }
        continue;
      }
      if (c === 0x0D || c === 0x0A) {
        continue;
      }
      if (!skipping()) {
        ensureRun();
        escapePush(c);
      }
    }
    closeRun();
    push('</p>');
    const result: Uint8Array = new Uint8Array(out.length);
    for (let k = 0; k < out.length; k++) {
      result[k] = out[k];
    }
    return decodeUtf8(result);
  }
}
