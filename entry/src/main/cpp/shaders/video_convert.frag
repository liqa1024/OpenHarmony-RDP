// HmRdp - Moonlight video import (fragment stage).
//
// The decoded buffer is imported as an R8 "byte image" over the whole allocation, and this pass
// re-interprets those bytes as the picture the shared tail consumes. Three layouts are addressable;
// in every case the frame's values already carry the colour the host encoded, and the picture is
// declared in the same encoding (the convert pass's target format follows the output stage, see
// kPictureFormat / kHdrPictureFormat), so SDR and HDR are a *re-interpretation*, not a conversion.
//
//   * interleaved 4:2:0 (NV12 / NV21 / P010), `plane.z` = 0:
//       sample (x, y)              = luma of pixel (x, y)
//       sample (x & ~1, chromaRow) = first chroma sample of the pair at (x/2, y/2)
//       sample (x & ~1 | 1, ...)   = second chroma sample of the same pair
//     `plane.y` selects the order: 1 = Cb first (NV12 / P010), 0 = Cr first (NV21 / P010-Cr).
//     `plane.w` is the bytes one sample occupies: 1 for NV12/NV21, 2 for P010 (a 16-bit word whose 10
//     bits sit in the high bits, divided down by `color.z`). `color.x` is the sample's full code
//     value (255 / 1023), which the matrix rows and the final normalisation are both scaled for.
//     The matrix rows (coefficients + offset, already scaled for this layout's sample word) come in
//     as a uniform buffer, so the range/model come from the frame's own colour space and nothing is
//     driver-defined. Chroma is upsampled bilinearly between the plane's own samples, and in Cb/Cr
//     rather than in RGB: a pixel at coordinate p belongs to chroma sample p/2, so an even coordinate
//     reads one sample and an odd one blends it with the next. The taps stay within the chroma
//     samples the *visible* rect covers, so the picture's last row/column replicate rather than
//     reaching into the alignment padding that follows the plane. No chroma siting offset is applied
//     - a sample keeps the bytes it owns.
//
//   * packed 10-bit RGB (RGBA1010102), `plane.z` = 1:
//     one 32-bit word per pixel, R in the low 10 bits - the same layout the HDR picture itself is
//     declared in (A2B10G10R10_UNORM_PACK32), so the values pass through unchanged. There is no
//     chroma plane and the row pitch is already bytes, so the word starts at byte `x * 4`.
//
// The one conversion that needs real transfer maths is `color.y` = 1: an HDR (BT.2020 PQ) frame shown
// through an SDR picture, i.e. the host in HDR while this window could not offer HDR10. The frame is
// then folded onto SDR here (PQ -> linear, BT.2020 -> BT.709, onto the SDR reference white, sRGB),
// and the whole downstream tail stays SDR. This is the session's `hdr-degraded` case, a usable picture
// rather than a faithful tone map: highlights above the reference white clip (see VkRenderer and
// kSdrWhiteNits below).
#version 450
layout(location = 0) in vec2 vByte;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uBytes;
layout(set = 0, binding = 1) uniform Frame {
  vec4 frame;  // xy = visible size in pixels, zw = sampling origin inside the byte image (crop)
  vec4 plane; // x = first row of the chroma plane, y = 1 when Cb comes first, z = 1 when packed, w = bytes per sample
  vec4 rowR;   // (cY, cCb, cCr, offset)
  vec4 rowG;
  vec4 rowB;
  vec4 color;  // x = the sample's code value (255 / 1023), y = 1 when an HDR frame is shown as SDR, z = P010 word divisor
} uFrame;

// SMPTE ST 2084 (PQ) constants and the sRGB curve, matching the reference's video_hdr_metadata.h.
const float kPqM1 = 2610.0 / 16384.0;
const float kPqM2 = 2523.0 / 4096.0 * 128.0;
const float kPqC1 = 3424.0 / 4096.0;
const float kPqC2 = 2413.0 / 4096.0 * 32.0;
const float kPqC3 = 2392.0 / 4096.0 * 32.0;
const float kPqPeakNits = 10000.0;
// The white an HDR picture's range is folded onto when only SDR can be shown: the HDR10 reference
// (diffuse) white, matching the 203-nit normalization the reference uses. Everything brighter clips.
const float kSdrWhiteNits = 203.0;

// Linear-light BT.2020 -> BT.709, for a PQ frame whose primaries are Rec.2020. GLSL builds mat3 from
// columns, so the argument order is the matrix read column by column.
const mat3 kBt2020ToBt709 = mat3(
  1.660491, -0.124550, -0.018151,
  -0.587641, 1.132900, -0.100579,
  -0.072850, -0.008348, 1.118730);

// PQ signal (0..1) -> linear light (1.0 = 10000 nits).
vec3 pqToLinear(vec3 pq) {
  const vec3 p = pow(clamp(pq, 0.0, 1.0), vec3(1.0 / kPqM2));
  const vec3 num = max(p - kPqC1, vec3(0.0));
  const vec3 den = max(kPqC2 - kPqC3 * p, vec3(1.0e-6));
  return pow(num / den, vec3(1.0 / kPqM1));
}

// Linear light (0..1) -> sRGB encoded.
vec3 linearToSrgb(vec3 c) {
  const vec3 lo = clamp(c, 0.0, 1.0) * 12.92;
  const vec3 hi = 1.055 * pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), c));
}

// One HDR (BT.2020 PQ) value shown through an SDR picture.
vec3 hdrToSdr(vec3 pq) {
  vec3 lin = kBt2020ToBt709 * pqToLinear(pq);
  lin *= kPqPeakNits / kSdrWhiteNits;
  return linearToSrgb(lin);
}

// One byte of the imported image at byte coordinate (x, y).
float byteAt(ivec2 at) {
  return texelFetch(uBytes, at, 0).r;
}

// One byte worth 0..255 (the imported image is UNORM, so a fetch is normalised).
float byteValue(ivec2 at) {
  return byteAt(at) * 255.0;
}

// One luma / chroma sample as its own code value, read from `plane.w` bytes at byte coordinate
// (`byteX`, `y`) little-endian. P010 keeps its 10 bits in the high bits of a 16-bit word, so the
// assembled word is divided down by `color.z`; for the 8-bit layouts that divisor is 1.
float sampleAt(int byteX, int y) {
  float value = byteValue(ivec2(byteX, y));
  if (uFrame.plane.w > 1.5) {
    value += byteValue(ivec2(byteX + 1, y)) * 256.0;
    value = floor(value / uFrame.color.z);
  }
  return value;
}

// (Cb, Cr) of one chroma sample, from the two adjacent samples interleaving gives it. `chromaSample`
// is an index in the plane's own grid; the chroma plane's first byte row is added here.
vec2 chromaAt(ivec2 chromaSample) {
  const int bytes = int(uFrame.plane.w);
  const int byteX = chromaSample.x * 2 * bytes;
  const int row = int(uFrame.plane.x) + chromaSample.y;
  const float first = sampleAt(byteX, row);
  const float second = sampleAt(byteX + bytes, row);
  return uFrame.plane.y > 0.5 ? vec2(first, second) : vec2(second, first);
}

void main() {
  const ivec2 origin = ivec2(uFrame.frame.zw);
  const ivec2 visible = ivec2(uFrame.frame.xy);
  const ivec2 pixel = ivec2(clamp(vByte, vec2(0.0), uFrame.frame.xy - 1.0)) + origin;
  const bool degrade = uFrame.color.y > 0.5;

  if (uFrame.plane.z > 0.5) {
    // Packed 10-bit RGB: reassemble the little-endian word from its four bytes and split the 10-bit
    // lanes. Alpha is dropped; the picture is opaque.
    const int bx = pixel.x * 4;
    const uint word = uint(byteValue(ivec2(bx + 0, pixel.y)))
                    | (uint(byteValue(ivec2(bx + 1, pixel.y))) << 8)
                    | (uint(byteValue(ivec2(bx + 2, pixel.y))) << 16)
                    | (uint(byteValue(ivec2(bx + 3, pixel.y))) << 24);
    const float scale = uFrame.color.x;
    vec3 rgb = vec3(float(word & 1023u) / scale,
                    float((word >> 10) & 1023u) / scale,
                    float((word >> 20) & 1023u) / scale);
    oColor = vec4(degrade ? hdrToSdr(rgb) : rgb, 1.0);
    return;
  }

  const int bytes = int(uFrame.plane.w);
  const float luma = sampleAt(pixel.x * bytes, pixel.y);
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
  const vec3 color = clamp(rgb / uFrame.color.x, 0.0, 1.0);
  oColor = vec4(degrade ? hdrToSdr(color) : color, 1.0);
}
