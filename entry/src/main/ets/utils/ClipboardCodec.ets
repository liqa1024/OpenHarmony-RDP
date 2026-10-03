/*
 * HmRdp - clipboard wire-format codecs (ArkTS side).
 *
 * The native clipboard channel only moves bytes; the format encoders/decoders
 * live here. This keeps protocol/format logic on the ArkTS side and the native
 * side a thin transport (doc_agent/architecture.md). What is migrated so far:
 * the CF_DIB encoder for a local image push.
 */

/** HarmonyOS image.PixelMapFormat values the clipboard image path handles. */
const PIXEL_FORMAT_ARGB_8888: number = 1;
const PIXEL_FORMAT_RGBA_8888: number = 3;
const PIXEL_FORMAT_BGRA_8888: number = 4;
const PIXEL_FORMAT_RGB_888: number = 5;

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
}
