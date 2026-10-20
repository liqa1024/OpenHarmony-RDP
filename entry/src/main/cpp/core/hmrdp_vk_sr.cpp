#include "hmrdp_vk_renderer.h"

#include <cstdint>
#include <cstring>

#if defined(HMRDP_HAVE_FSR)
#include "fsr_easu.frag.h"
#include "fsr_rcas.frag.h"
#endif

#include "hmrdp_log.h"
#include "present_quad.vert.h"

namespace hmrdp {
namespace {

// 超分: sharpening handed to XEngine's spatial upscale, documented range [0.0, 1.0];
// a low value keeps text-heavy downscaled desktops from ringing.
constexpr float kSrSharpness = 0.2f;

// FSR: RCAS sharpening in "stops" (0.0 = max, N halves it N times). AMD's FSR 1.0
// sample value; maximum rings around glyph edges in text-heavy desktop content.
#if defined(HMRDP_HAVE_FSR)
constexpr float kFsrSharpnessStops = 0.2f;

// FSR: the EASU target carries more precision than the picture it feeds. EASU's output is a weighted
// blend of 8-bit samples, so its low bits are real information that an 8-bit intermediate would round
// away before RCAS sharpens - and amplifies - the rounding. A wider intermediate costs bandwidth, not
// output precision: what the letterbox finally samples stays kPictureFormat.
constexpr VkFormat kFsrEasuFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
#endif

#if defined(HMRDP_HAVE_FSR)
bool CreateFsrPipeline(VkApi& api, VkDevice device, VkRenderPass pass, VkPipelineLayout layout,
                       const uint32_t* fragSpv, uint32_t fragWords, VkPipeline* out) {
  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  VkShaderModuleCreateInfo shaderInfo{};
  shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shaderInfo.codeSize = kPresentQuadVertSpvWords * sizeof(uint32_t);
  shaderInfo.pCode = kPresentQuadVertSpv;
  if (api.CreateShaderModule(device, &shaderInfo, nullptr, &vert) != VK_SUCCESS) {
    return false;
  }
  shaderInfo.codeSize = fragWords * sizeof(uint32_t);
  shaderInfo.pCode = fragSpv;
  if (api.CreateShaderModule(device, &shaderInfo, nullptr, &frag) != VK_SUCCESS) {
    api.DestroyShaderModule(device, vert, nullptr);
    return false;
  }
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;
  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;
  dynamicState.pDynamicStates = dynamicStates;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blendAttachment;

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertexInput;
  info.pInputAssemblyState = &inputAssembly;
  info.pViewportState = &viewportState;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamicState;
  info.layout = layout;
  info.renderPass = pass;
  info.subpass = 0;
  const VkResult result = api.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, out);
  api.DestroyShaderModule(device, vert, nullptr);
  api.DestroyShaderModule(device, frag, nullptr);
  return result == VK_SUCCESS;
}
#endif  // HMRDP_HAVE_FSR

}  // namespace

void VkRenderer::SetSuperResolution(bool enabled, SuperResolutionBackend backend, int outputWidth,
                                    int outputHeight) {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool changed = srRequested_ != enabled || srBackend_ != backend ||
                       srOutputWidth_ != outputWidth || srOutputHeight_ != outputHeight;
  srRequested_ = enabled;
  srBackend_ = backend;
  srOutputWidth_ = outputWidth > 0 ? outputWidth : 0;
  srOutputHeight_ = outputHeight > 0 ? outputHeight : 0;
  if (changed) {
    srFailed_ = false;
    srActiveFrame_ = false;
    DestroySuperResolutionLocked();
  }
  HMRDP_LOGI("vulkan presenter: super resolution %{public}s (%{public}s, output %{public}dx%{public}d)",
             enabled ? "on" : "off",
             backend == SuperResolutionBackend::kFsr ? "fsr" : "xengine", srOutputWidth_,
             srOutputHeight_);
}

bool VkRenderer::EnsureSuperResolutionLocked(int inputWidth, int inputHeight, uint32_t slot,
                                           VkImageView inputView, VkImageView otherInputView) {
  if (!srRequested_ || srFailed_) {
    return false;
  }
  const int outputWidth = srOutputWidth_ > 0 ? srOutputWidth_ : inputWidth;
  const int outputHeight = srOutputHeight_ > 0 ? srOutputHeight_ : inputHeight;
  if (outputWidth <= inputWidth || outputHeight <= inputHeight) {
    return false;
  }
  if (srBackend_ == SuperResolutionBackend::kFsr) {
    return EnsureFsrLocked(inputWidth, inputHeight, slot, inputView, otherInputView, outputWidth,
                           outputHeight);
  }
  return EnsureXengineLocked(inputWidth, inputHeight, outputWidth, outputHeight);
}

bool VkRenderer::EnsureXengineLocked(int inputWidth, int inputHeight, int outputWidth,
                                     int outputHeight) {
  const bool match = srImageView_[0] != VK_NULL_HANDLE && srImageView_[1] != VK_NULL_HANDLE &&
                     srUpscale_.valid() && srImageWidth_ == outputWidth &&
                     srImageHeight_ == outputHeight && srInputWidth_ == inputWidth &&
                     srInputHeight_ == inputHeight;
  if (match) {
    return true;
  }
  DestroySuperResolutionLocked();

  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    return false;
  }
  // Rare resize path: frames still reading the old output must drain first; safe
  // because this runs before the frame's swapchain image is acquired.
  DrainInFlightLocked();
  const auto fail = [this](const char* what) {
    error_ = what;
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
  };
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    if (!CreateDeviceImage(api, context, kPictureFormat, outputWidth, outputHeight,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           &srImage_[slot], &srImageMemory_[slot])) {
      return fail("super-resolution image allocation failed");
    }
    if (!CreateImageView2D(api, context, kPictureFormat, srImage_[slot],
                           &srImageView_[slot])) {
      return fail("super-resolution image view failed");
    }
  }

  VkExtent2D inputExtent{};
  inputExtent.width = static_cast<uint32_t>(inputWidth);
  inputExtent.height = static_cast<uint32_t>(inputHeight);
  VkExtent2D outputExtent{};
  outputExtent.width = static_cast<uint32_t>(outputWidth);
  outputExtent.height = static_cast<uint32_t>(outputHeight);
  if (!srUpscale_.Create(device, kPictureFormat, inputExtent, outputExtent, kSrSharpness)) {
    HMRDP_LOGW("vulkan presenter: super resolution unavailable, presenting the desktop as-is");
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
  }
  srImageWidth_ = outputWidth;
  srImageHeight_ = outputHeight;
  srInputWidth_ = inputWidth;
  srInputHeight_ = inputHeight;
  return true;
}

void VkRenderer::DestroySuperResolutionLocked() {
  DestroyFsrLocked();
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device != VK_NULL_HANDLE) {
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
      if (srImageView_[slot] != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
        api.DestroyImageView(device, srImageView_[slot], nullptr);
      }
      if (srImage_[slot] != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
        api.DestroyImage(device, srImage_[slot], nullptr);
      }
      if (srImageMemory_[slot] != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
        api.FreeMemory(device, srImageMemory_[slot], nullptr);
      }
      srImageView_[slot] = VK_NULL_HANDLE;
      srImage_[slot] = VK_NULL_HANDLE;
      srImageMemory_[slot] = VK_NULL_HANDLE;
      srInputView_[slot] = VK_NULL_HANDLE;
    }
  } else {
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
      srImageView_[slot] = VK_NULL_HANDLE;
      srImage_[slot] = VK_NULL_HANDLE;
      srImageMemory_[slot] = VK_NULL_HANDLE;
      srInputView_[slot] = VK_NULL_HANDLE;
    }
  }
  srUpscale_.Destroy();
  srImageWidth_ = 0;
  srImageHeight_ = 0;
  srInputWidth_ = 0;
  srInputHeight_ = 0;
  srActiveFrame_ = false;
}

bool VkRenderer::EnsureFsrLocked(int inputWidth, int inputHeight, uint32_t slot,
                                 VkImageView inputView, VkImageView otherInputView,
                                 int outputWidth, int outputHeight) {
#if !defined(HMRDP_HAVE_FSR)
  (void)inputWidth;
  (void)inputHeight;
  (void)slot;
  (void)inputView;
  (void)otherInputView;
  (void)outputWidth;
  (void)outputHeight;
  srFailed_ = true;
  HMRDP_LOGW("vulkan presenter: this build has no FSR shaders"
             " (run native/scripts/fetch-sources.ps1)");
  return false;
#else
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE || slot >= kFramesInFlight) {
    return false;
  }
  const bool match = srImageView_[0] != VK_NULL_HANDLE && srImageView_[1] != VK_NULL_HANDLE &&
                     fsrRcasPipeline_ != VK_NULL_HANDLE &&
                     srImageWidth_ == outputWidth && srImageHeight_ == outputHeight &&
                     srInputWidth_ == inputWidth && srInputHeight_ == inputHeight &&
                     srInputView_[slot] == inputView &&
                     srInputView_[(slot + 1) % kFramesInFlight] == otherInputView;
  if (match) {
    return true;
  }
  DestroySuperResolutionLocked();
  // Rare path (resize or backend switch): frames still reading the old targets must
  // drain first; safe because this runs before the swapchain image is acquired.
  DrainInFlightLocked();
  const auto fail = [this](const char* what) {
    error_ = what;
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
  };

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (!CreateDeviceImage(api, context, kPictureFormat, outputWidth, outputHeight,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           &srImage_[i], &srImageMemory_[i])) {
      return fail("FSR: output image allocation failed");
    }
    if (!CreateImageView2D(api, context, kPictureFormat, srImage_[i], &srImageView_[i])) {
      return fail("FSR: output image view failed");
    }
    if (!CreateDeviceImage(api, context, kFsrEasuFormat, outputWidth, outputHeight,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           &fsrEasuImage_[i], &fsrEasuImageMemory_[i])) {
      return fail("FSR: EASU image allocation failed");
    }
    if (!CreateImageView2D(api, context, kFsrEasuFormat, fsrEasuImage_[i],
                           &fsrEasuImageView_[i])) {
      return fail("FSR: EASU image view failed");
    }
  }

  // One render pass per target format: a pass carries its attachment's format, so the two stages
  // cannot share one. Both rewrite their target whole every frame (loadOp DONT_CARE) and leave it in
  // GENERAL, because the next sampler - RCAS, then the letterbox - reads it from there.
  const auto make_render_pass = [&](VkFormat format, VkRenderPass* out) -> bool {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &colorAttachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    return api.CreateRenderPass(device, &info, nullptr, out) == VK_SUCCESS;
  };
  if (!make_render_pass(kFsrEasuFormat, &fsrEasuRenderPass_) ||
      !make_render_pass(kPictureFormat, &fsrRcasRenderPass_)) {
    return fail("FSR: vkCreateRenderPass failed");
  }
  const VkExtent2D extent{static_cast<uint32_t>(outputWidth),
                          static_cast<uint32_t>(outputHeight)};
  const auto make_framebuffer =
      [&](VkRenderPass pass, VkImageView view, VkFramebuffer* out) -> bool {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    return api.CreateFramebuffer(device, &info, nullptr, out) == VK_SUCCESS;
  };
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (!make_framebuffer(fsrEasuRenderPass_, fsrEasuImageView_[i], &fsrEasuFramebuffer_[i]) ||
        !make_framebuffer(fsrRcasRenderPass_, srImageView_[i], &fsrRcasFramebuffer_[i])) {
      return fail("FSR: vkCreateFramebuffer failed");
    }
  }

  // EASU gathers four texels per tap and RCAS texel-fetches, so the filter mode is
  // irrelevant - but addressing must be clamped (gather wraps otherwise).
  {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_NEAREST;
    info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0f;
    if (api.CreateSampler(device, &info, nullptr, &fsrSampler_) != VK_SUCCESS) {
      return fail("FSR: vkCreateSampler failed");
    }
  }
  {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[0].pImmutableSamplers = &fsrSampler_;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 2;
    info.pBindings = bindings;
    if (api.CreateDescriptorSetLayout(device, &info, nullptr, &fsrSetLayout_) != VK_SUCCESS) {
      return fail("FSR: vkCreateDescriptorSetLayout failed");
    }
  }
  {
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &fsrSetLayout_;
    if (api.CreatePipelineLayout(device, &info, nullptr, &fsrPipelineLayout_) != VK_SUCCESS) {
      return fail("FSR: vkCreatePipelineLayout failed");
    }
  }
  {
    constexpr uint32_t kSets = kFramesInFlight * 2;  // EASU input + RCAS input per frame slot
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = kSets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = kSets;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = kSets;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    if (api.CreateDescriptorPool(device, &info, nullptr, &fsrPool_) != VK_SUCCESS) {
      return fail("FSR: vkCreateDescriptorPool failed");
    }
    VkDescriptorSetLayout layouts[kSets] = {};
    for (uint32_t i = 0; i < kSets; ++i) {
      layouts[i] = fsrSetLayout_;
    }
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = fsrPool_;
    alloc.descriptorSetCount = kSets;
    alloc.pSetLayouts = layouts;
    VkDescriptorSet sets[kSets] = {};
    if (api.AllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS) {
      return fail("FSR: vkAllocateDescriptorSets failed");
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
      fsrEasuSet_[i] = sets[i * 2];
      fsrRcasSet_[i] = sets[i * 2 + 1];
    }
  }
  if (!CreateFsrPipeline(api, device, fsrEasuRenderPass_, fsrPipelineLayout_, kFsrEasuFragSpv,
                         kFsrEasuFragSpvWords, &fsrEasuPipeline_) ||
      !CreateFsrPipeline(api, device, fsrRcasRenderPass_, fsrPipelineLayout_, kFsrRcasFragSpv,
                         kFsrRcasFragSpvWords, &fsrRcasPipeline_)) {
    return fail("FSR: vkCreateGraphicsPipelines failed");
  }

  // The passes build their FSR constants from these floats; 4 x vec4 (std140).
  const auto make_params = [&](VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) -> bool {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kFsrParamsBytes;
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (api.CreateBuffer(device, &info, nullptr, buffer) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(device, *buffer, &req);
    uint32_t typeIndex = context.FindMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    fsrParamsCoherent_ = typeIndex != UINT32_MAX;
    if (!fsrParamsCoherent_) {
      typeIndex = context.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (typeIndex == UINT32_MAX) {
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = typeIndex;
    if (api.AllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS) {
      return false;
    }
    if (api.BindBufferMemory(device, *buffer, *memory, 0) != VK_SUCCESS) {
      return false;
    }
    if (api.MapMemory(device, *memory, 0, kFsrParamsBytes, 0, mapped) != VK_SUCCESS ||
        *mapped == nullptr) {
      *mapped = nullptr;
      return false;
    }
    return true;
  };
  if (!make_params(&fsrEasuParams_, &fsrEasuParamsMemory_, &fsrEasuParamsMapped_) ||
      !make_params(&fsrRcasParams_, &fsrRcasParamsMemory_, &fsrRcasParamsMapped_)) {
    return fail("FSR: parameter buffer allocation failed");
  }
  const size_t paramsFloats = kFsrParamsBytes / sizeof(float);
  float* easuParams = static_cast<float*>(fsrEasuParamsMapped_);
  float* rcasParams = static_cast<float*>(fsrRcasParamsMapped_);
  for (size_t i = 0; i < paramsFloats; ++i) {
    easuParams[i] = 0.0f;
    rcasParams[i] = 0.0f;
  }
  // x,y of each vec4: viewport (session desktop), image holding it, pass resolution.
  easuParams[0] = static_cast<float>(inputWidth);
  easuParams[1] = static_cast<float>(inputHeight);
  easuParams[4] = static_cast<float>(inputWidth);
  easuParams[5] = static_cast<float>(inputHeight);
  easuParams[8] = static_cast<float>(outputWidth);
  easuParams[9] = static_cast<float>(outputHeight);
  rcasParams[12] = kFsrSharpnessStops;
  if (!fsrParamsCoherent_) {
    for (VkDeviceMemory memory : {fsrEasuParamsMemory_, fsrRcasParamsMemory_}) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = memory;
      range.offset = 0;
      range.size = kFsrParamsBytes;
      api.FlushMappedMemoryRanges(device, 1, &range);
    }
  }

  VkDescriptorImageInfo easuInput[2] = {};
  // The EASU input is whatever the caller presents: RDP's desktop image, or the Moonlight video
  // picture for this frame slot (bound here rather than hardcoded, or FSR would sample a desktop
  // image that does not exist on the video path).
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    easuInput[i].sampler = fsrSampler_;
    easuInput[i].imageView = i == slot ? inputView : otherInputView;
    easuInput[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }
  VkDescriptorBufferInfo easuBlock{};
  easuBlock.buffer = fsrEasuParams_;
  easuBlock.range = kFsrParamsBytes;
  VkDescriptorImageInfo rcasInput[2] = {};
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    rcasInput[i].sampler = fsrSampler_;
    rcasInput[i].imageView = fsrEasuImageView_[i];
    rcasInput[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }
  VkDescriptorBufferInfo rcasBlock{};
  rcasBlock.buffer = fsrRcasParams_;
  rcasBlock.range = kFsrParamsBytes;
  VkWriteDescriptorSet writes[8] = {};
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkWriteDescriptorSet& easuImage = writes[i * 4];
    easuImage.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    easuImage.dstSet = fsrEasuSet_[i];
    easuImage.dstBinding = 0;
    easuImage.descriptorCount = 1;
    easuImage.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    easuImage.pImageInfo = &easuInput[i];
    VkWriteDescriptorSet& easuParams = writes[i * 4 + 1];
    easuParams.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    easuParams.dstSet = fsrEasuSet_[i];
    easuParams.dstBinding = 1;
    easuParams.descriptorCount = 1;
    easuParams.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    easuParams.pBufferInfo = &easuBlock;
    VkWriteDescriptorSet& rcasImage = writes[i * 4 + 2];
    rcasImage.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    rcasImage.dstSet = fsrRcasSet_[i];
    rcasImage.dstBinding = 0;
    rcasImage.descriptorCount = 1;
    rcasImage.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    rcasImage.pImageInfo = &rcasInput[i];
    VkWriteDescriptorSet& rcasParams = writes[i * 4 + 3];
    rcasParams.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    rcasParams.dstSet = fsrRcasSet_[i];
    rcasParams.dstBinding = 1;
    rcasParams.descriptorCount = 1;
    rcasParams.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    rcasParams.pBufferInfo = &rcasBlock;
  }
  api.UpdateDescriptorSets(device, 8, writes, 0, nullptr);

  srImageWidth_ = outputWidth;
  srImageHeight_ = outputHeight;
  srInputWidth_ = inputWidth;
  srInputHeight_ = inputHeight;
  srInputView_[slot] = inputView;
  srInputView_[(slot + 1) % kFramesInFlight] = otherInputView;
  return true;
#endif
}

void VkRenderer::DestroyFsrLocked() {
  VkApi& api = vk_.api();
  const VkDevice device = vk_.device();
  if (device != VK_NULL_HANDLE) {
    if (api.UnmapMemory != nullptr) {
      if (fsrEasuParamsMapped_ != nullptr) {
        api.UnmapMemory(device, fsrEasuParamsMemory_);
      }
      if (fsrRcasParamsMapped_ != nullptr) {
        api.UnmapMemory(device, fsrRcasParamsMemory_);
      }
    }
    if (api.DestroyFramebuffer != nullptr) {
      for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (fsrEasuFramebuffer_[i] != VK_NULL_HANDLE) {
          api.DestroyFramebuffer(device, fsrEasuFramebuffer_[i], nullptr);
        }
        if (fsrRcasFramebuffer_[i] != VK_NULL_HANDLE) {
          api.DestroyFramebuffer(device, fsrRcasFramebuffer_[i], nullptr);
        }
      }
    }
    if (api.DestroyPipeline != nullptr) {
      if (fsrEasuPipeline_ != VK_NULL_HANDLE) {
        api.DestroyPipeline(device, fsrEasuPipeline_, nullptr);
      }
      if (fsrRcasPipeline_ != VK_NULL_HANDLE) {
        api.DestroyPipeline(device, fsrRcasPipeline_, nullptr);
      }
    }
    if (api.DestroyDescriptorPool != nullptr && fsrPool_ != VK_NULL_HANDLE) {
      api.DestroyDescriptorPool(device, fsrPool_, nullptr);
    }
    if (api.DestroyPipelineLayout != nullptr && fsrPipelineLayout_ != VK_NULL_HANDLE) {
      api.DestroyPipelineLayout(device, fsrPipelineLayout_, nullptr);
    }
    if (api.DestroyDescriptorSetLayout != nullptr && fsrSetLayout_ != VK_NULL_HANDLE) {
      api.DestroyDescriptorSetLayout(device, fsrSetLayout_, nullptr);
    }
    if (api.DestroySampler != nullptr && fsrSampler_ != VK_NULL_HANDLE) {
      api.DestroySampler(device, fsrSampler_, nullptr);
    }
    if (api.DestroyRenderPass != nullptr) {
      if (fsrEasuRenderPass_ != VK_NULL_HANDLE) {
        api.DestroyRenderPass(device, fsrEasuRenderPass_, nullptr);
      }
      if (fsrRcasRenderPass_ != VK_NULL_HANDLE) {
        api.DestroyRenderPass(device, fsrRcasRenderPass_, nullptr);
      }
    }
    if (api.DestroyImageView != nullptr) {
      for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (fsrEasuImageView_[i] != VK_NULL_HANDLE) {
          api.DestroyImageView(device, fsrEasuImageView_[i], nullptr);
        }
      }
    }
    if (api.DestroyImage != nullptr) {
      for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (fsrEasuImage_[i] != VK_NULL_HANDLE) {
          api.DestroyImage(device, fsrEasuImage_[i], nullptr);
        }
      }
    }
    if (api.FreeMemory != nullptr) {
      for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (fsrEasuImageMemory_[i] != VK_NULL_HANDLE) {
          api.FreeMemory(device, fsrEasuImageMemory_[i], nullptr);
        }
      }
    }
    if (api.DestroyBuffer != nullptr) {
      if (fsrEasuParams_ != VK_NULL_HANDLE) {
        api.DestroyBuffer(device, fsrEasuParams_, nullptr);
      }
      if (fsrRcasParams_ != VK_NULL_HANDLE) {
        api.DestroyBuffer(device, fsrRcasParams_, nullptr);
      }
    }
    if (api.FreeMemory != nullptr) {
      if (fsrEasuParamsMemory_ != VK_NULL_HANDLE) {
        api.FreeMemory(device, fsrEasuParamsMemory_, nullptr);
      }
      if (fsrRcasParamsMemory_ != VK_NULL_HANDLE) {
        api.FreeMemory(device, fsrRcasParamsMemory_, nullptr);
      }
    }
  }
  fsrEasuParamsMapped_ = nullptr;
  fsrRcasParamsMapped_ = nullptr;
  fsrEasuRenderPass_ = VK_NULL_HANDLE;
  fsrRcasRenderPass_ = VK_NULL_HANDLE;
  fsrSampler_ = VK_NULL_HANDLE;
  fsrSetLayout_ = VK_NULL_HANDLE;
  fsrPipelineLayout_ = VK_NULL_HANDLE;
  fsrPool_ = VK_NULL_HANDLE;
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    fsrEasuSet_[slot] = VK_NULL_HANDLE;
    fsrRcasSet_[slot] = VK_NULL_HANDLE;
    fsrEasuImage_[slot] = VK_NULL_HANDLE;
    fsrEasuImageMemory_[slot] = VK_NULL_HANDLE;
    fsrEasuImageView_[slot] = VK_NULL_HANDLE;
    fsrEasuFramebuffer_[slot] = VK_NULL_HANDLE;
    fsrRcasFramebuffer_[slot] = VK_NULL_HANDLE;
  }
  fsrEasuPipeline_ = VK_NULL_HANDLE;
  fsrRcasPipeline_ = VK_NULL_HANDLE;
  fsrEasuParams_ = VK_NULL_HANDLE;
  fsrEasuParamsMemory_ = VK_NULL_HANDLE;
  fsrRcasParams_ = VK_NULL_HANDLE;
  fsrRcasParamsMemory_ = VK_NULL_HANDLE;
  fsrParamsCoherent_ = false;
}

bool VkRenderer::RecordSuperResolutionLocked(VkCommandBuffer cmd, VkImageView inputView,
                                             uint32_t slot) {
  if (slot >= kFramesInFlight) {
    return false;
  }
  VkApi& api = vk_.api();
  VkImageSubresourceRange subresource{};
  subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  subresource.levelCount = 1;
  subresource.layerCount = 1;
  const auto targetToSampled = [&](VkImage image) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = subresource;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
  };

  if (srBackend_ != SuperResolutionBackend::kFsr) {
    // XEngine: the tail already barriered the input picture for the fragment stage, and the
    // upscale renders straight into the target.
    VkImageMemoryBarrier toAttachment{};
    toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttachment.srcAccessMask = 0;
    toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = srImage_[slot];
    toAttachment.subresourceRange = subresource;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &toAttachment);
    srUpscale_.Record(cmd, inputView, srImageView_[slot]);
    targetToSampled(srImage_[slot]);
    return true;
  }

#if defined(HMRDP_HAVE_FSR)
  if (fsrEasuPipeline_ == VK_NULL_HANDLE || fsrRcasPipeline_ == VK_NULL_HANDLE ||
      fsrEasuFramebuffer_[slot] == VK_NULL_HANDLE ||
      fsrRcasFramebuffer_[slot] == VK_NULL_HANDLE) {
    return false;
  }
  const VkExtent2D extent{static_cast<uint32_t>(srImageWidth_),
                          static_cast<uint32_t>(srImageHeight_)};
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = extent;
  const auto drawPass = [&](VkRenderPass pass, VkFramebuffer framebuffer, VkPipeline pipeline,
                            VkDescriptorSet set) {
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.offset = {0, 0};
    begin.renderArea.extent = extent;
    api.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    api.CmdSetViewport(cmd, 0, 1, &viewport);
    api.CmdSetScissor(cmd, 0, 1, &scissor);
    api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsrPipelineLayout_, 0, 1, &set,
                              0, nullptr);
    api.CmdDraw(cmd, 3, 1, 0, 0);
    api.CmdEndRenderPass(cmd);
  };

  drawPass(fsrEasuRenderPass_, fsrEasuFramebuffer_[slot], fsrEasuPipeline_, fsrEasuSet_[slot]);
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fsrEasuImage_[slot];
    barrier.subresourceRange = subresource;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
  }
  drawPass(fsrRcasRenderPass_, fsrRcasFramebuffer_[slot], fsrRcasPipeline_, fsrRcasSet_[slot]);
  targetToSampled(srImage_[slot]);
  return true;
#else
  return false;
#endif
}


}  // namespace hmrdp
