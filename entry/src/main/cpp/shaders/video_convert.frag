// HmRdp - Moonlight video import (fragment stage).
//
// The decoded buffer is imported as an R8 "byte image" (row pitch x plane rows), so the shader owns
// the YUV -> RGB conversion instead of the driver's external-format path:
//   byte (x, y)                = luma of pixel (x, y)
//   byte (x & ~1, chromaRow)   = first chroma byte of the pair at (x/2, y/2)
//   byte (x & ~1 | 1, chroma*) = second chroma byte of the same pair
// `plane.y` selects the order: 1 = Cb first (NV12), 0 = Cr first (NV21).
// The matrix rows (coefficients + offset, already scaled for 0..255 samples) come in as a uniform
// buffer, so the range/model come from the buffer's own colour space and nothing is driver-defined.
#version 450
layout(location = 0) in vec2 vByte;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uBytes;
layout(set = 0, binding = 1) uniform Frame {
  vec4 frame;  // xy = visible size in pixels, zw = sampling origin inside the byte image (crop)
  vec4 plane; // x = first row of the chroma plane, y = 1 when Cb comes first (NV12)
  vec4 rowR;   // (cY, cCb, cCr, offset)
  vec4 rowG;
  vec4 rowB;
} uFrame;

void main() {
  const ivec2 origin = ivec2(uFrame.frame.zw);
  const ivec2 pixel = ivec2(clamp(vByte, vec2(0.0), uFrame.frame.xy - 1.0)) + origin;
  const float luma = texelFetch(uBytes, pixel, 0).r * 255.0;
  const ivec2 chromaPos = ivec2(pixel.x & ~1, int(uFrame.plane.x) + (pixel.y >> 1));
  const float first = texelFetch(uBytes, chromaPos, 0).r * 255.0;
  const float second = texelFetch(uBytes, ivec2(chromaPos.x + 1, chromaPos.y), 0).r * 255.0;
  const bool cbFirst = uFrame.plane.y > 0.5;
  const float cb = cbFirst ? first : second;
  const float cr = cbFirst ? second : first;
  const vec3 ycc = vec3(luma, cb, cr);
  const vec3 rgb = vec3(dot(uFrame.rowR.xyz, ycc) + uFrame.rowR.w,
                        dot(uFrame.rowG.xyz, ycc) + uFrame.rowG.w,
                        dot(uFrame.rowB.xyz, ycc) + uFrame.rowB.w);
  oColor = vec4(clamp(rgb / 255.0, 0.0, 1.0), 1.0);
}
