// HmRdp - Moonlight video import (fragment stage).
//
// The decoded buffer is imported as an R8 "byte image" over the whole allocation, and this pass
// re-interprets those bytes as the picture the shared tail consumes. Two layouts are addressable,
// and both are a *re-interpretation* rather than a colour conversion: the frame's values already
// carry the colour the host encoded, and the picture is declared in the same encoding (the convert
// pass's target format follows the output stage, see kPictureFormat / kHdrPictureFormat).
//
//   * interleaved 8-bit 4:2:0 (NV12 / NV21), `plane.z` = 0:
//       byte (x, y)                = luma of pixel (x, y)
//       byte (x & ~1, chromaRow)   = first chroma byte of the pair at (x/2, y/2)
//       byte (x & ~1 | 1, chroma*) = second chroma byte of the same pair
//     `plane.y` selects the order: 1 = Cb first (NV12), 0 = Cr first (NV21).
//     The matrix rows (coefficients + offset, already scaled for 0..255 samples) come in as a uniform
//     buffer, so the range/model come from the buffer's own colour space and nothing is driver-defined.
//     Chroma is upsampled bilinearly between the plane's own samples, and in Cb/Cr rather than in RGB:
//     a pixel at byte coordinate p belongs to chroma sample p/2, so an even coordinate reads one sample
//     and an odd one blends it with the next. The taps stay within the chroma samples the *visible*
//     rect covers, so the picture's last row/column replicate rather than reaching into the alignment
//     padding that follows the plane. No chroma siting offset is applied - a sample keeps the byte pair
//     it owns.
//
//   * packed 10-bit RGB (RGBA1010102), `plane.z` = 1:
//     one 32-bit word per pixel, R in the low 10 bits - the same layout the HDR picture itself is
//     declared in (A2B10G10R10_UNORM_PACK32), so the values pass through unchanged. There is no chroma
//     plane and the row pitch is already bytes, so the word starts at byte `x * 4`.
//
// The one conversion that would need real transfer maths - HDR (BT.2020/PQ) frames into an SDR
// picture, i.e. the host in HDR while this window could not offer HDR10 - is not attempted: the
// caller keeps the output stage from claiming HDR for such a session instead of showing a picture
// nobody can colour-correct here (see VkRenderer::PresentVideo and the `hdr-inactive` warning).
#version 450
layout(location = 0) in vec2 vByte;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uBytes;
layout(set = 0, binding = 1) uniform Frame {
  vec4 frame;  // xy = visible size in pixels, zw = sampling origin inside the byte image (crop)
  vec4 plane; // x = first row of the chroma plane, y = 1 when Cb comes first (NV12), z = 1 when packed
  vec4 rowR;   // (cY, cCb, cCr, offset)
  vec4 rowG;
  vec4 rowB;
  vec4 color;  // x = the sample's word (255 for 8-bit, 1023 for 10-bit)
} uFrame;

// One byte of the imported image at byte coordinate (x, y).
float byteAt(ivec2 at) {
  return texelFetch(uBytes, at, 0).r;
}

// One byte worth 0..255 (the imported image is UNORM, so a fetch is normalised).
float byteValue(ivec2 at) {
  return byteAt(at) * 255.0;
}

// (Cb, Cr) of one chroma sample, from the two adjacent bytes interleaving gives it. `chromaSample` is
// an index in the plane's own grid; the chroma plane's first byte row is added here.
vec2 chromaAt(ivec2 chromaSample) {
  const ivec2 chroma = ivec2(chromaSample.x * 2, int(uFrame.plane.x) + chromaSample.y);
  const float first = byteValue(chroma);
  const float second = byteValue(ivec2(chroma.x + 1, chroma.y));
  return uFrame.plane.y > 0.5 ? vec2(first, second) : vec2(second, first);
}

void main() {
  const ivec2 origin = ivec2(uFrame.frame.zw);
  const ivec2 visible = ivec2(uFrame.frame.xy);
  const ivec2 pixel = ivec2(clamp(vByte, vec2(0.0), uFrame.frame.xy - 1.0)) + origin;

  if (uFrame.plane.z > 0.5) {
    // Packed 10-bit RGB: reassemble the little-endian word from its four bytes and split the 10-bit
    // lanes. Alpha is dropped; the picture is opaque.
    const int bx = pixel.x * 4;
    const uint word = uint(byteValue(ivec2(bx + 0, pixel.y)))
                    | (uint(byteValue(ivec2(bx + 1, pixel.y))) << 8)
                    | (uint(byteValue(ivec2(bx + 2, pixel.y))) << 16)
                    | (uint(byteValue(ivec2(bx + 3, pixel.y))) << 24);
    const float scale = uFrame.color.x;
    oColor = vec4(float(word & 1023u) / scale,
                  float((word >> 10) & 1023u) / scale,
                  float((word >> 20) & 1023u) / scale,
                  1.0);
    return;
  }

  const float luma = byteValue(pixel);
  const ivec2 base = pixel >> 1;
  const vec2 frac = vec2(pixel) * 0.5 - vec2(base);
  const ivec2 firstSample = origin >> 1;
  const ivec2 lastSample = (origin + visible - 1) >> 1;
  const vec2 top = mix(chromaAt(clamp(base, firstSample, lastSample)),
                       chromaAt(clamp(base + ivec2(1, 0), firstSample, lastSample)), frac.x);
  const vec2 bottom = mix(chromaAt(clamp(base + ivec2(0, 1), firstSample, lastSample)),
                          chromaAt(clamp(base + ivec2(1, 1), firstSample, lastSample)), frac.x);
  const vec3 ycc = vec3(luma, mix(top, bottom, frac.y));
  const vec3 rgb = vec3(dot(uFrame.rowR.xyz, ycc) + uFrame.rowR.w,
                        dot(uFrame.rowG.xyz, ycc) + uFrame.rowG.w,
                        dot(uFrame.rowB.xyz, ycc) + uFrame.rowB.w);
  oColor = vec4(clamp(rgb / 255.0, 0.0, 1.0), 1.0);
}
