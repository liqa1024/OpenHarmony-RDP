// HmRdp - Moonlight video import (fragment stage).
//
// The decoded buffer is imported as an R8 "byte image" (row pitch x plane rows), so the shader owns
// the YUV -> RGB conversion instead of the driver's external-format path:
//   byte (x, y)                = luma of pixel (x, y)
//   byte (x & ~1, chromaRow)   = Cb of the pair at (x/2, y/2)
//   byte (x & ~1 | 1, chroma*) = Cr of the same pair
// The matrix rows (coefficients + offset, already scaled for 0..255 samples) come in as a uniform
// buffer, so the range/model come from the buffer's own colour space and nothing is driver-defined.
#version 450
layout(location = 0) in vec2 vByte;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uBytes;
layout(set = 0, binding = 1) uniform Frame {
  vec4 visibleChroma; // xy = visible size in pixels, z = first row of the chroma plane
  vec4 rowR;          // (cY, cCb, cCr, offset)
  vec4 rowG;
  vec4 rowB;
} uFrame;

void main() {
  const ivec2 pixel = ivec2(clamp(vByte, vec2(0.0), uFrame.visibleChroma.xy - 1.0));
  const float luma = texelFetch(uBytes, pixel, 0).r * 255.0;
  const ivec2 chromaPos = ivec2(pixel.x & ~1, int(uFrame.visibleChroma.z) + (pixel.y >> 1));
  const float cb = texelFetch(uBytes, chromaPos, 0).r * 255.0;
  const float cr = texelFetch(uBytes, ivec2(chromaPos.x + 1, chromaPos.y), 0).r * 255.0;
  const vec3 ycc = vec3(luma, cb, cr);
  const vec3 rgb = vec3(dot(uFrame.rowR.xyz, ycc) + uFrame.rowR.w,
                        dot(uFrame.rowG.xyz, ycc) + uFrame.rowG.w,
                        dot(uFrame.rowB.xyz, ycc) + uFrame.rowB.w);
  oColor = vec4(clamp(rgb / 255.0, 0.0, 1.0), 1.0);
}
