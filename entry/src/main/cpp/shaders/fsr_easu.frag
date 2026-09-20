// HmRdp - FSR 1.0 EASU pass (fragment stage): edge-adaptive spatial upsampling of
// the session-resolution desktop into the output-resolution intermediate image.
//
// The algorithm is AMD's FidelityFX Super Resolution 1.0 (MIT), vendored under
// native/third_party/ and fetched by native/scripts/fetch-sources.ps1. `ffx_fsr1.h`
// is written to be #included by the calling shader and asks it for the gather4
// callbacks below; the constants it needs are built here from the plain uniforms
// (FsrEasuCon works on the GPU too), so the presenter only uploads a few floats.
//
// The desktop image is BGRA8 like the rest of the presenter: EASU's channel maths
// is symmetric in R and B, so the ordering does not change the result, and the
// present quad still does the swap at the end (see present_quad.frag).
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uInput;
layout(set = 0, binding = 1) uniform FsrParams {
  // xy = rendered viewport (the session desktop size), zw unused.
  vec4 inputViewport;
  // xy = the size of the image holding the input, zw unused.
  vec4 inputSize;
  // xy = the output resolution this pass renders at, zw unused.
  vec4 outputSize;
  // x = RCAS sharpness, unused by EASU.
  vec4 sharpness;
} fsr;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(uInput, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(uInput, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(uInput, p, 2); }
#include "ffx_fsr1.h"

void main() {
  AU4 con0;
  AU4 con1;
  AU4 con2;
  AU4 con3;
  FsrEasuCon(con0, con1, con2, con3, fsr.inputViewport.x, fsr.inputViewport.y,
             fsr.inputSize.x, fsr.inputSize.y, fsr.outputSize.x, fsr.outputSize.y);
  AF3 pix;
  FsrEasuF(pix, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
  oColor = vec4(pix, 1.0);
}
