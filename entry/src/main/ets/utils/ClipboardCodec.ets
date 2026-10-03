/*
 * HmRdp - clipboard wire-format codecs (ArkTS side).
 *
 * The native clipboard channel only moves bytes; the format encoders/decoders
 * live here. This keeps protocol/format logic on the ArkTS side and the native
 * side a thin transport (doc_agent/architecture.md). Migrated so far: the
 * CF_DIB encoder (local image) and the CF_HTML encoder + plain-text fallback
 * (local HTML).
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
 * safe. Mirrors the native FindIgnoreCase it replaces.
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

export class ClipboardCodec {
  /**
   * Serialises a HarmonyOS PixelMap buffer (top-down, `pixelFormat`) into a
   * CF_DIB payload: a 40-byte BITMAPINFOHEADER (BI_RGB, 32bpp) followed by
   * bottom-up BGRA rows. Returns undefined for an unsupported format or a short
   * buffer, matching the native encoder it replaces.
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
   * push. Not a renderer: it only needs to be readable in a plain text target.
   * Mirrors the native StripHtmlToText it replaces (tag names map to newlines,
   * then the common entities are unescaped).
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
}
