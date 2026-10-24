// HmRdp - Moonlight video import (fragment stage).
//
// The decoded buffer is imported as an R8 "byte image" over the whole allocation, and this pass
// re-interprets those bytes as the picture the shared tail consumes. The frame is 8-bit interleaved
// 4:2:0 (NV12 / NV21): the values already carry the colour the host encoded, and the picture is
// declared in the same encoding, so this is a *re-interpretation*, not a conversion.
//
//   * interleaved 4:2:0 (NV12 / NV21):
//       sample (x, y)              = luma of pixel (x, y)
//       sample (x & ~1, chromaRow) = first chroma sample of the pair at (x/2, y/2)
//       sample (x & ~1 | 1, ...)   = second chroma sample of the same pair
//     `plane.y` selects the order: 1 = Cb first (NV12), 0 = Cr first (NV21).
//     The matrix rows (coefficients + offset) come in as a uniform buffer, so the range/model come
//     from the frame's own colour space and nothing is driver-defined. Chroma is upsampled bilinearly
//     between the plane's own samples, and in Cb/Cr rather than in RGB: a pixel at coordinate p
//     belongs to chroma sample p/2, so an even coordinate reads one sample and an odd one blends it
//     with the next. The taps stay within the chroma samples the *visible* rect covers, so the
//     picture's last row/column replicate rather than reaching into the alignment padding that
//     follows the plane. No chroma siting offset is applied - a sample keeps the bytes it owns.
//
// An HDR stream never reaches this pass: it is presented by the platform's own decode path instead
// (see moonlight-video.md §2), so there is no PQ / HLG fold here.
#version 450
layout(location = 0) in vec2 vByte;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uBytes;
layout(set = 0, binding = 1) uniform Frame {
  vec4 frame;  // xy = visible size in pixels, zw = sampling origin inside the byte image (crop)
  vec4 plane; // x = first row of the chroma plane, y = 1 when Cb comes first
  vec4 rowR;   // (cY, cCb, cCr, offset)
  vec4 rowG;
  vec4 rowB;
} uFrame;

// One byte of the imported image at byte coordinate (x, y).
float byteAt(ivec2 at) {
  return texelFetch(uBytes, at, 0).r;
}

// One byte worth 0..255 (the imported image is UNORM, so a fetch is normalised).
float byteValue(ivec2 at) {
  return byteAt(at) * 255.0;
}

// One luma / chroma sample as its own code value at byte coordinate (`byteX`, `y`).
float sampleAt(int byteX, int y) {
  return byteValue(ivec2(byteX, y));
}

// (Cb, Cr) of one chroma sample, from the two adjacent samples interleaving gives it. `chromaSample`
// is an index in the plane's own grid; the chroma plane's first byte row is added here.
vec2 chromaAt(ivec2 chromaSample) {
  const int byteX = chromaSample.x * 2;
  const int row = int(uFrame.plane.x) + chromaSample.y;
  const float first = sampleAt(byteX, row);
  const float second = sampleAt(byteX + 1, row);
  return uFrame.plane.y > 0.5 ? vec2(first, second) : vec2(second, first);
}

void main() {
  const ivec2 origin = ivec2(uFrame.frame.zw);
  const ivec2 visible = ivec2(uFrame.frame.xy);
  const ivec2 pixel = ivec2(clamp(vByte, vec2(0.0), uFrame.frame.xy - 1.0)) + origin;

  const float luma = sampleAt(pixel.x, pixel.y);
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
