// HmRdp - FSR 1.0 RCAS pass (fragment stage): robust contrast-adaptive sharpening of
// the EASU output, written into the image the letterbox quad samples.
//
// Same vendored AMD FSR 1.0 header as fsr_easu.frag (MIT, native/third_party); the
// only callback it needs is the texel load, and the sharpness comes from the same
// uniform block. RCAS sharpness is in "stops": 0.0 is maximum sharpening and larger
// values halve it, which is why the presenter sends the setting already halved-down.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uInput;
layout(set = 0, binding = 1) uniform FsrParams {
  vec4 inputViewport;
  vec4 inputSize;
  vec4 outputSize;
  // x = RCAS sharpness in stops (0 = maximum).
  vec4 sharpness;
} fsr;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(uInput, ivec2(p), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"

void main() {
  AU4 con;
  FsrRcasCon(con, fsr.sharpness.x);
  AF1 r;
  AF1 g;
  AF1 b;
  FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), con);
  oColor = vec4(r, g, b, 1.0);
}
