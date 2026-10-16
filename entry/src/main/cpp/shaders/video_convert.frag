// HmRdp - Moonlight video import (fragment stage): samples one decoded frame imported from its
// ohos native buffer and writes it into the presenter's picture image at the session resolution.
//
// The imported image's sampler carries a VkSamplerYcbcrConversion, so the sampled value is already
// RGB (the hardware does the NV12 -> RGB conversion); no channel swap is needed, and the shared
// 超分 / letterbox tail then treats the picture image exactly like the RDP desktop picture.
#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uVideo;

void main() {
  oColor = vec4(texture(uVideo, vUv).rgb, 1.0);
}
