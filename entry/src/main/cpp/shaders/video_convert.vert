// HmRdp - Moonlight video import (vertex stage).
//
// Outputs the *byte coordinates* of the visible picture inside the imported byte image (the buffer
// imported as R8: row pitch x plane rows, luma first, then the interleaved chroma plane). The
// fragment stage fetches luma and chroma from there and does the YUV -> RGB itself, so nothing here
// depends on the driver's external-format conversion path.
//
// The frame's parameters arrive in a uniform buffer (the presenter's FSR passes use the same route;
// push constants are not relied on).
#version 450
layout(location = 0) out vec2 vByte;
layout(set = 0, binding = 1) uniform Frame {
  vec4 frame;  // xy = visible size in pixels, zw = sampling origin inside the byte image (crop)
  vec4 plane; // x = first row of the chroma plane
  vec4 rowR;   // (cY, cCb, cCr, offset), already scaled for 0..255 samples
  vec4 rowG;
  vec4 rowB;
} uFrame;

void main() {
  const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  vByte = p * uFrame.frame.xy;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
