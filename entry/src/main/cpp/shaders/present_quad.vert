// HmRdp - presenter quad (vertex stage).
//
// Draws one fullscreen triangle; the letterbox is done by the *viewport* (set per
// frame to the picture rectangle), exactly like the GLES presenter's glViewport.
// uv = 0 is the top edge of the picture (Vulkan NDC y = -1 is the top), matching
// the top-down desktop image.
#version 450
layout(location = 0) out vec2 vUv;

void main() {
  const vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
