/*
 * HmRdp - file-clipboard wire-format codec (ArkTS side).
 *
 * The native clipboard channel only stores and serves bytes; the
 * FILEGROUPDESCRIPTORW format lives here (doc_agent/architecture.md), the same
 * way the HTML / DIB / RTF codecs do. It builds the descriptor for a local file
 * offer and parses the server's descriptor list into `FileDescriptorInput`s.
 */

/** One file advertised on the clipboard. */
export class FileDescriptorInput {
  name: string = '';
  size: number = 0;
  sizeKnown: boolean = false;
  isDirectory: boolean = false;

  static of(name: string, size: number): FileDescriptorInput {
    const entry: FileDescriptorInput = new FileDescriptorInput();
    entry.name = name;
    entry.size = size;
    entry.sizeKnown = true;
    return entry;
  }
}

// [MS-RDPECLIP] 2.2.5.2 FILEDESCRIPTORW: fixed 592 bytes. Offsets of the fields
// this codec reads/writes.
const FD_ENTRY_BYTES: number = 592;
const FD_FLAGS_OFFSET: number = 0;
const FD_ATTRS_OFFSET: number = 36;
const FD_SIZE_HIGH_OFFSET: number = 64;
const FD_SIZE_LOW_OFFSET: number = 68;
const FD_NAME_OFFSET: number = 72;
const FD_NAME_CHARS: number = 260;
const FD_ATTRIBUTES: number = 0x00000004;
const FD_FILESIZE: number = 0x00000040;
const FD_ATTRIBUTE_DIRECTORY: number = 0x00000010;
const FD_FLAGS: number = FD_ATTRIBUTES | FD_FILESIZE; // written for a local offer
const FD_FILE_ATTRIBUTE_NORMAL: number = 0x00000080;
const U32_RANGE: number = 4294967296;

/** Reads a NUL-terminated UTF-16LE string of at most `maxChars` code units. */
function readUtf16(view: DataView, offset: number, maxChars: number): string {
  let out: string = '';
  for (let i = 0; i < maxChars; i++) {
    const unit: number = view.getUint16(offset + i * 2, true);
    if (unit === 0) {
      break;
    }
    out += String.fromCharCode(unit);
  }
  return out;
}

/**
 * A clipboard file name comes from the peer; never let one escape the target
 * directory. Mirrors the native SanitizeClipboardFileName (native keeps its own
 * copy as a defense-in-depth guard on the destination path).
 */
function sanitizeClipboardFileName(name: string): string {
  let out: string = '';
  for (let i = 0; i < name.length; i++) {
    const c: string = name[i];
    if (c === '/' || c === '\\' || c === ':' || c === '\u0000') {
      out += '_';
    } else {
      out += c;
    }
  }
  if (out.length === 0 || out === '.' || out === '..') {
    out = 'file';
  }
  return out;
}

export class FileTransferCodec {
  /**
   * Serialises a FILEGROUPDESCRIPTORW: a u32 count followed by `count` fixed
   * 592-byte FILEDESCRIPTORW entries (name as UTF-16LE, size split into two
   * u32). Byte-for-byte the native builder it replaces.
   */
  static buildFileGroupDescriptor(entries: FileDescriptorInput[]): ArrayBuffer {
    const count: number = entries.length;
    const buffer: ArrayBuffer = new ArrayBuffer(4 + count * FD_ENTRY_BYTES);
    const view: DataView = new DataView(buffer);
    view.setUint32(0, count, true);
    for (let i = 0; i < count; i++) {
      const base: number = 4 + i * FD_ENTRY_BYTES;
      view.setUint32(base + FD_FLAGS_OFFSET, FD_FLAGS, true);
      view.setUint32(base + FD_ATTRS_OFFSET, FD_FILE_ATTRIBUTE_NORMAL, true);
      const size: number = entries[i].size;
      view.setUint32(base + FD_SIZE_HIGH_OFFSET, Math.floor(size / U32_RANGE), true);
      view.setUint32(base + FD_SIZE_LOW_OFFSET, size % U32_RANGE, true);
      const name: string = entries[i].name;
      const chars: number = Math.min(name.length, FD_NAME_CHARS - 1);
      for (let j = 0; j < chars; j++) {
        view.setUint16(base + FD_NAME_OFFSET + j * 2, name.charCodeAt(j), true);
      }
      // The entry is zero-filled, so the name is already NUL-terminated.
    }
    return buffer;
  }

  /**
   * Parses a FILEGROUPDESCRIPTORW into descriptors (name sanitized, size split
   * from the two u32 parts). Returns an empty list for a malformed payload.
   * Mirrors the native parser it replaces; the order is preserved because it is
   * the FILECONTENTS listIndex.
   */
  static parseFileGroupDescriptor(fgd: ArrayBuffer): FileDescriptorInput[] {
    const result: FileDescriptorInput[] = [];
    if (fgd.byteLength < 4) {
      return result;
    }
    const view: DataView = new DataView(fgd);
    const count: number = view.getUint32(0, true);
    if (count === 0 || 4 + count * FD_ENTRY_BYTES > fgd.byteLength) {
      return result;
    }
    for (let i = 0; i < count; i++) {
      const base: number = 4 + i * FD_ENTRY_BYTES;
      const flags: number = view.getUint32(base + FD_FLAGS_OFFSET, true);
      const attrs: number = view.getUint32(base + FD_ATTRS_OFFSET, true);
      const entry: FileDescriptorInput = new FileDescriptorInput();
      entry.name = sanitizeClipboardFileName(
        readUtf16(view, base + FD_NAME_OFFSET, FD_NAME_CHARS));
      entry.isDirectory = (flags & FD_ATTRIBUTES) !== 0
        && (attrs & FD_ATTRIBUTE_DIRECTORY) !== 0;
      if ((flags & FD_FILESIZE) !== 0) {
        entry.size = view.getUint32(base + FD_SIZE_HIGH_OFFSET, true) * U32_RANGE
          + view.getUint32(base + FD_SIZE_LOW_OFFSET, true);
        entry.sizeKnown = true;
      }
      result.push(entry);
    }
    return result;
  }
}
