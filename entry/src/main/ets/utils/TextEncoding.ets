import { util } from '@kit.ArkTS';

// Clipboard wire bytes carry no trusted charset field: the specs mandate one
// (CF_HTML = UTF-8) but Windows producers still emit the ANSI code page they
// declare inside the payload itself. Trust order therefore is
// BOM > strict UTF-8 > payload-declared charset > first legacy candidate that
// decodes cleanly. Only the last step is a guess, so it is logged by the caller.
const CHARSET_UTF8: string = 'utf-8';
const CHARSET_UTF16LE: string = 'utf-16le';
const CHARSET_UTF16BE: string = 'utf-16be';
const CHARSET_ANSI: string = 'windows-1252';
const LEGACY_FALLBACK: string[] = ['gb18030', 'big5', 'shift_jis', 'euc-kr', CHARSET_ANSI];
const REPLACEMENT_CHAR: string = '\uFFFD';
const SNIFF_LIMIT: number = 4096;

export class DecodedText {
  text: string = '';
  charset: string = '';
}

export class TextEncoding {
  // Names util.TextDecoder.create accepts; anything else falls through to the
  // legacy candidates instead of failing the paste.
  private static readonly ALIASES: Record<string, string> = {
    'utf8': CHARSET_UTF8,
    'utf-8': CHARSET_UTF8,
    'gb2312': 'gb2312',
    'gbk': 'gbk',
    'gb18030': 'gb18030',
    'cp936': 'gbk',
    'cp54936': 'gb18030',
    'big5': 'big5',
    'cp950': 'big5',
    'shift_jis': 'shift_jis',
    'shift-jis': 'shift_jis',
    'sjis': 'shift_jis',
    'cp932': 'shift_jis',
    'euc-kr': 'euc-kr',
    'euckr': 'euc-kr',
    'cp949': 'euc-kr',
    'iso-8859-1': 'iso-8859-1',
    'iso8859-1': 'iso-8859-1',
    'latin1': 'iso-8859-1',
    'windows-1252': CHARSET_ANSI,
    'cp1252': CHARSET_ANSI,
    'utf-16': CHARSET_UTF16LE,
    'utf16': CHARSET_UTF16LE,
    'unicode': CHARSET_UTF16LE,
    'utf-16le': CHARSET_UTF16LE,
    'utf-16be': CHARSET_UTF16BE
  };

  static decode(bytes: Uint8Array, declared: string = ''): DecodedText {
    const out: DecodedText = new DecodedText();
    if (bytes.length === 0) {
      out.charset = CHARSET_UTF8;
      return out;
    }
    // A UTF-8 BOM needs no branch: the decoder strips it by default.
    const bom: string = TextEncoding.bomCharset(bytes);
    if (bom.length > 0 && bom !== CHARSET_UTF8) {
      const wide: string | undefined = TextEncoding.decodeWith(bytes.subarray(2), bom);
      if (wide !== undefined) {
        out.text = wide;
        out.charset = bom;
        return out;
      }
    }
    const utf8Text: string | undefined = TextEncoding.decodeWith(bytes, CHARSET_UTF8);
    if (utf8Text !== undefined) {
      out.text = utf8Text;
      out.charset = CHARSET_UTF8;
      return out;
    }
    const hint: string = TextEncoding.normalizeCharset(declared);
    if (hint.length > 0) {
      const hinted: string | undefined = TextEncoding.decodeWith(bytes, hint);
      if (hinted !== undefined) {
        out.text = hinted;
        out.charset = hint;
        return out;
      }
    }
    for (const candidate of LEGACY_FALLBACK) {
      const text: string | undefined = TextEncoding.decodeWith(bytes, candidate);
      if (text !== undefined) {
        out.text = text;
        out.charset = candidate;
        return out;
      }
    }
    // Every strict attempt failed; a lossy UTF-8 read keeps the ASCII readable.
    out.text = TextEncoding.decodeLossy(bytes, CHARSET_UTF8);
    out.charset = CHARSET_UTF8;
    return out;
  }

  // RTF `\'hh` bytes are defined by the document's code page, so the declared
  // charset outranks the detection order; an unsupported one still recovers.
  static decodeDeclared(bytes: Uint8Array, charset: string): DecodedText {
    if (charset.length > 0) {
      const text: string | undefined = TextEncoding.decodeWith(bytes, charset);
      if (text !== undefined) {
        const out: DecodedText = new DecodedText();
        out.text = text;
        out.charset = charset;
        return out;
      }
    }
    return TextEncoding.decode(bytes, charset);
  }

  // RTF names its ANSI code page in the header; `\'hh` bytes and the `\uN`
  // fallback are written in it.
  static rtfCharset(bytes: Uint8Array): string {
    const head: string = TextEncoding.asciiHead(bytes, SNIFF_LIMIT);
    const at: number = head.indexOf('\\ansicpg');
    if (at >= 0) {
      const page: number = TextEncoding.readNumber(head, at + 8);
      if (page > 0) {
        return TextEncoding.codePageToCharset(page);
      }
    }
    return CHARSET_ANSI;
  }

  // CF_HTML may declare a charset in the context (<meta charset>,
  // content="text/html; charset=...", or an XML declaration) that is not UTF-8.
  static markupCharset(bytes: Uint8Array): string {
    const head: string = TextEncoding.asciiHead(bytes, SNIFF_LIMIT);
    let name: string = TextEncoding.tokenAfter(head, head.indexOf('charset='), 8);
    if (name.length === 0) {
      name = TextEncoding.tokenAfter(head, head.indexOf('encoding='), 9);
    }
    return TextEncoding.normalizeCharset(name);
  }

  static normalizeCharset(name: string): string {
    const lower: string = name.trim().toLowerCase();
    if (lower.length === 0) {
      return '';
    }
    const mapped: string | undefined = TextEncoding.ALIASES[lower];
    return mapped === undefined ? lower : mapped;
  }

  private static bomCharset(bytes: Uint8Array): string {
    if (bytes.length >= 3 && bytes[0] === 0xEF && bytes[1] === 0xBB && bytes[2] === 0xBF) {
      return CHARSET_UTF8;
    }
    if (bytes.length >= 2 && bytes[0] === 0xFF && bytes[1] === 0xFE) {
      return CHARSET_UTF16LE;
    }
    if (bytes.length >= 2 && bytes[0] === 0xFE && bytes[1] === 0xFF) {
      return CHARSET_UTF16BE;
    }
    return '';
  }

  private static codePageToCharset(page: number): string {
    if (page === 65001) {
      return CHARSET_UTF8;
    }
    if (page === 936) {
      return 'gbk';
    }
    if (page === 54936) {
      return 'gb18030';
    }
    if (page === 950) {
      return 'big5';
    }
    if (page === 932) {
      return 'shift_jis';
    }
    if (page === 949) {
      return 'euc-kr';
    }
    if (page === 1200) {
      return CHARSET_UTF16LE;
    }
    if (page === 1201) {
      return CHARSET_UTF16BE;
    }
    if (page === 28591 || page === 819) {
      return 'iso-8859-1';
    }
    if (page >= 1250 && page <= 1258) {
      return 'windows-' + page.toString();
    }
    return CHARSET_ANSI;
  }

  private static decodeWith(bytes: Uint8Array, charset: string): string | undefined {
    try {
      const options: util.TextDecoderOptions = { fatal: true };
      const text: string = util.TextDecoder.create(charset, options).decodeToString(bytes);
      // fatal is not enforced by every runtime, so a replacement character also
      // counts as "this charset is wrong".
      return text.indexOf(REPLACEMENT_CHAR) >= 0 ? undefined : text;
    } catch (error) {
      return undefined;
    }
  }

  private static decodeLossy(bytes: Uint8Array, charset: string): string {
    try {
      return util.TextDecoder.create(charset).decodeToString(bytes);
    } catch (error) {
      return '';
    }
  }

  // Lowercased ASCII view of the payload head: non-ASCII bytes become spaces, so
  // the declaration keywords of a multi-byte document stay matchable.
  private static asciiHead(bytes: Uint8Array, limit: number): string {
    const count: number = bytes.length < limit ? bytes.length : limit;
    let head: string = '';
    for (let i = 0; i < count; i++) {
      const b: number = bytes[i];
      if (b >= 0x20 && b < 0x7F) {
        head += String.fromCharCode(b >= 0x41 && b <= 0x5A ? b + 0x20 : b);
      } else {
        head += ' ';
      }
    }
    return head;
  }

  private static tokenAfter(head: string, at: number, keyLength: number): string {
    if (at < 0) {
      return '';
    }
    let i: number = at + keyLength;
    while (i < head.length && head[i] === ' ') {
      i++;
    }
    if (i < head.length && (head[i] === '"' || head[i] === '\'')) {
      i++;
    }
    let value: string = '';
    while (i < head.length && TextEncoding.isCharsetChar(head.charCodeAt(i))) {
      value += head[i];
      i++;
    }
    return value;
  }

  private static isCharsetChar(code: number): boolean {
    if (code >= 0x61 && code <= 0x7A) {
      return true;
    }
    if (code >= 0x30 && code <= 0x39) {
      return true;
    }
    return code === 0x2D || code === 0x5F || code === 0x2E;
  }

  private static readNumber(text: string, at: number): number {
    let value: number = 0;
    let digits: number = 0;
    let i: number = at;
    while (i < text.length && text[i] >= '0' && text[i] <= '9') {
      value = value * 10 + (text.charCodeAt(i) - 0x30);
      i++;
      digits++;
    }
    return digits === 0 ? -1 : value;
  }
}
