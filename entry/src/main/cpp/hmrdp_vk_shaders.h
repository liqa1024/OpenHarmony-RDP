/*
 * HmRdp - access to the compiled-in SPIR-V shaders (VULKAN-TODO.md §5 V2).
 *
 * The words come from generated headers (cmake/EmbedSpirv.cmake); this is the
 * only translation unit that includes them, so callers get a plain pointer/size
 * pair and never depend on the generated symbol names.
 */
#ifndef HMRDP_VK_SHADERS_H
#define HMRDP_VK_SHADERS_H

#include <cstdint>

namespace hmrdp {

struct ShaderBlob {
  const uint32_t* words = nullptr;
  uint32_t wordCount = 0;
  bool valid() const { return words != nullptr && wordCount > 0; }
};

// Bring-up probe kernel (shaders/probe.comp): a deterministic two-SSBO compute
// dispatch used to validate the compute path on a device before the RFX decode
// kernels rely on it.
ShaderBlob ComputeProbeShader();

}  // namespace hmrdp

#endif  // HMRDP_VK_SHADERS_H
