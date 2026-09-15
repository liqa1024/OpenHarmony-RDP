// HmRdp - presenter quad (fragment stage): samples the desktop picture and writes
// the swapchain image.
//
// The channel swap lives here (not in a CPU pass over the frame): FreeRDP hands us
// BGRA and the screen surface is usually RGBA-ordered, so the texture read has to
// be swizzled. `kSwapRb` is a specialization constant because it depends on the
// swapchain format: when the swapchain is BGRA-ordered the uploaded bytes are
// already in the right order and the swap must not happen. This mirrors what the
// GLES presenter's fragment shader does.
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uPicture;
layout(constant_id = 0) const bool kSwapRb = true;

void main() {
  const vec4 c = texture(uPicture, vUv);
  oColor = kSwapRb ? vec4(c.bgr, 1.0) : vec4(c.rgb, 1.0);
}
