/*
 * HmRdp - compiled-in SPIR-V shaders (VULKAN-TODO.md §5 V2).
 *
 * The `*.comp.h` headers are generated into the CMake binary dir by
 * cmake/EmbedSpirv.cmake (glslang_validator from the OHOS SDK toolchains).
 */
#include "hmrdp_vk_shaders.h"

#include "probe.comp.h"

namespace hmrdp {

ShaderBlob ComputeProbeShader() {
  ShaderBlob blob;
  blob.words = kProbeSpv;
  blob.wordCount = kProbeSpvWords;
  return blob;
}

}  // namespace hmrdp
