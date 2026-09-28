// HmRdp - presenter quad (fragment stage): samples the desktop picture and writes
// the swapchain image.
//
// `kSwapRb` is a specialization constant and the pipeline sets it false: the
// picture image is declared in FreeRDP's byte order (`kPictureFormat`), so the
// sampled value is already the semantic colour and writing it to the attachment
// converts to the swapchain's format. No CPU pass over the frame does the swap.
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uPicture;
layout(constant_id = 0) const bool kSwapRb = true;

void main() {
  const vec4 c = texture(uPicture, vUv);
  oColor = kSwapRb ? vec4(c.bgr, 1.0) : vec4(c.rgb, 1.0);
}
