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
    DestroySuperResolutionLocked();
  }
  HMRDP_LOGI("vulkan presenter: super resolution %{public}s (%{public}s, output %{public}dx%{public}d)",
             enabled ? "on" : "off",
             backend == SuperResolutionBackend::kFsr ? "fsr" : "xengine", srOutputWidth_,
             srOutputHeight_);
}

bool VkRenderer::EnsureSuperResolutionLocked(int inputWidth, int inputHeight,
                                           VkImageView inputView) {
  if (!srRequested_ || srFailed_) {
    return false;
  }
  const int outputWidth = srOutputWidth_ > 0 ? srOutputWidth_ : inputWidth;
  const int outputHeight = srOutputHeight_ > 0 ? srOutputHeight_ : inputHeight;
  if (outputWidth <= inputWidth || outputHeight <= inputHeight) {
    return false;
  }
  if (srBackend_ == SuperResolutionBackend::kFsr) {
    return EnsureFsrLocked(inputWidth, inputHeight, inputView, outputWidth, outputHeight);
  }
  return EnsureXengineLocked(inputWidth, inputHeight, outputWidth, outputHeight);
}

bool VkRenderer::EnsureXengineLocked(int inputWidth, int inputHeight, int outputWidth,
                                     int outputHeight) {
  const bool match = srImageView_ != VK_NULL_HANDLE && srUpscale_.valid() &&
                     srImageWidth_ == outputWidth && srImageHeight_ == outputHeight &&
                     srInputWidth_ == inputWidth && srInputHeight_ == inputHeight;
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

  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = kPictureFormat;
  info.extent.width = static_cast<uint32_t>(outputWidth);
  info.extent.height = static_cast<uint32_t>(outputHeight);
  info.extent.depth = 1;
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult result = api.CreateImage(device, &info, nullptr, &srImage_);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateImage (super resolution): " + VkResultName(result);
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
  }
  VkMemoryRequirements req{};
  api.GetImageMemoryRequirements(device, srImage_, &req);
  const uint32_t typeIndex =
      context.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (typeIndex == UINT32_MAX) {
    error_ = "no device-local memory type for the super-resolution image";
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
  }
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = typeIndex;
  result = api.AllocateMemory(device, &alloc, nullptr, &srImageMemory_);
  if (result != VK_SUCCESS) {
    error_ = "vkAllocateMemory (super resolution): " + VkResultName(result);
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
  }
  result = api.BindImageMemory(device, srImage_, srImageMemory_, 0);
  if (result != VK_SUCCESS) {
    error_ = "vkBindImageMemory (super resolution): " + VkResultName(result);
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
  }

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = srImage_;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = kPictureFormat;
  viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  result = api.CreateImageView(device, &viewInfo, nullptr, &srImageView_);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateImageView (super resolution): " + VkResultName(result);
    srFailed_ = true;
    DestroySuperResolutionLocked();
    return false;
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
    if (srImageView_ != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
      api.DestroyImageView(device, srImageView_, nullptr);
    }
    if (srImage_ != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
      api.DestroyImage(device, srImage_, nullptr);
    }
    if (srImageMemory_ != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, srImageMemory_, nullptr);
    }
  }
  srUpscale_.Destroy();
  srImageView_ = VK_NULL_HANDLE;
  srImage_ = VK_NULL_HANDLE;
  srImageMemory_ = VK_NULL_HANDLE;
  srImageWidth_ = 0;
  srImageHeight_ = 0;
  srInputWidth_ = 0;
  srInputHeight_ = 0;
  srInputView_ = VK_NULL_HANDLE;
}

bool VkRenderer::EnsureFsrLocked(int inputWidth, int inputHeight, VkImageView inputView,
                                 int outputWidth, int outputHeight) {
#if !defined(HMRDP_HAVE_FSR)
  (void)inputWidth;
  (void)inputHeight;
  (void)inputView;
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
  if (device == VK_NULL_HANDLE) {
    return false;
  }
  const bool match = srImageView_ != VK_NULL_HANDLE && fsrRcasPipeline_ != VK_NULL_HANDLE &&
                     srImageWidth_ == outputWidth && srImageHeight_ == outputHeight &&
                     srInputWidth_ == inputWidth && srInputHeight_ == inputHeight &&
                     srInputView_ == inputView;
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

  if (!CreateDeviceImage(api, context, kPictureFormat, outputWidth, outputHeight,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         &srImage_, &srImageMemory_)) {
    return fail("FSR: output image allocation failed");
  }
  if (!CreateImageView2D(api, context, kPictureFormat, srImage_, &srImageView_)) {
    return fail("FSR: output image view failed");
  }
  if (!CreateDeviceImage(api, context, kPictureFormat, outputWidth, outputHeight,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         &fsrEasuImage_, &fsrEasuImageMemory_)) {
    return fail("FSR: EASU image allocation failed");
  }
  if (!CreateImageView2D(api, context, kPictureFormat, fsrEasuImage_, &fsrEasuImageView_)) {
    return fail("FSR: EASU image view failed");
  }

  {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = kPictureFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The next stage samples it (RCAS, then the letterbox), so leave it in GENERAL.
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
    if (api.CreateRenderPass(device, &info, nullptr, &fsrRenderPass_) != VK_SUCCESS) {
      return fail("FSR: vkCreateRenderPass failed");
    }
  }
  const VkExtent2D extent{static_cast<uint32_t>(outputWidth),
                          static_cast<uint32_t>(outputHeight)};
  const auto make_framebuffer = [&](VkImageView view, VkFramebuffer* out) -> bool {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = fsrRenderPass_;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    return api.CreateFramebuffer(device, &info, nullptr, out) == VK_SUCCESS;
  };
  if (!make_framebuffer(fsrEasuImageView_, &fsrEasuFramebuffer_) ||
      !make_framebuffer(srImageView_, &fsrRcasFramebuffer_)) {
    return fail("FSR: vkCreateFramebuffer failed");
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
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 2;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 2;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 2;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    if (api.CreateDescriptorPool(device, &info, nullptr, &fsrPool_) != VK_SUCCESS) {
      return fail("FSR: vkCreateDescriptorPool failed");
    }
    VkDescriptorSetLayout layouts[2] = {fsrSetLayout_, fsrSetLayout_};
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = fsrPool_;
    alloc.descriptorSetCount = 2;
    alloc.pSetLayouts = layouts;
    VkDescriptorSet sets[2] = {};
    if (api.AllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS) {
      return fail("FSR: vkAllocateDescriptorSets failed");
    }
    fsrEasuSet_ = sets[0];
    fsrRcasSet_ = sets[1];
  }
  if (!CreateFsrPipeline(api, device, fsrRenderPass_, fsrPipelineLayout_, kFsrEasuFragSpv,
                         kFsrEasuFragSpvWords, &fsrEasuPipeline_) ||
      !CreateFsrPipeline(api, device, fsrRenderPass_, fsrPipelineLayout_, kFsrRcasFragSpv,
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

  VkDescriptorImageInfo easuInput{};
  easuInput.sampler = fsrSampler_;
  // The EASU input is whatever the caller presents: RDP's desktop image, or the Moonlight video
  // input image (bound here rather than hardcoded, or FSR would sample a desktop image that does
  // not exist on the video path).
  easuInput.imageView = inputView;
  easuInput.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorBufferInfo easuBlock{};
  easuBlock.buffer = fsrEasuParams_;
  easuBlock.range = kFsrParamsBytes;
  VkDescriptorImageInfo rcasInput{};
  rcasInput.sampler = fsrSampler_;
  rcasInput.imageView = fsrEasuImageView_;
  rcasInput.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorBufferInfo rcasBlock{};
  rcasBlock.buffer = fsrRcasParams_;
  rcasBlock.range = kFsrParamsBytes;
  VkWriteDescriptorSet writes[4] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = fsrEasuSet_;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &easuInput;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = fsrEasuSet_;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[1].pBufferInfo = &easuBlock;
  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = fsrRcasSet_;
  writes[2].dstBinding = 0;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[2].pImageInfo = &rcasInput;
  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = fsrRcasSet_;
  writes[3].dstBinding = 1;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[3].pBufferInfo = &rcasBlock;
  api.UpdateDescriptorSets(device, 4, writes, 0, nullptr);

  srImageWidth_ = outputWidth;
  srImageHeight_ = outputHeight;
  srInputWidth_ = inputWidth;
  srInputHeight_ = inputHeight;
  srInputView_ = inputView;
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
      if (fsrEasuFramebuffer_ != VK_NULL_HANDLE) {
        api.DestroyFramebuffer(device, fsrEasuFramebuffer_, nullptr);
      }
      if (fsrRcasFramebuffer_ != VK_NULL_HANDLE) {
        api.DestroyFramebuffer(device, fsrRcasFramebuffer_, nullptr);
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
    if (api.DestroyRenderPass != nullptr && fsrRenderPass_ != VK_NULL_HANDLE) {
      api.DestroyRenderPass(device, fsrRenderPass_, nullptr);
    }
    if (api.DestroyImageView != nullptr && fsrEasuImageView_ != VK_NULL_HANDLE) {
      api.DestroyImageView(device, fsrEasuImageView_, nullptr);
    }
    if (api.DestroyImage != nullptr && fsrEasuImage_ != VK_NULL_HANDLE) {
      api.DestroyImage(device, fsrEasuImage_, nullptr);
    }
    if (api.FreeMemory != nullptr && fsrEasuImageMemory_ != VK_NULL_HANDLE) {
      api.FreeMemory(device, fsrEasuImageMemory_, nullptr);
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
  fsrRenderPass_ = VK_NULL_HANDLE;
  fsrSampler_ = VK_NULL_HANDLE;
  fsrSetLayout_ = VK_NULL_HANDLE;
  fsrPipelineLayout_ = VK_NULL_HANDLE;
  fsrPool_ = VK_NULL_HANDLE;
  fsrEasuSet_ = VK_NULL_HANDLE;
  fsrRcasSet_ = VK_NULL_HANDLE;
  fsrEasuPipeline_ = VK_NULL_HANDLE;
  fsrRcasPipeline_ = VK_NULL_HANDLE;
  fsrEasuImage_ = VK_NULL_HANDLE;
  fsrEasuImageMemory_ = VK_NULL_HANDLE;
  fsrEasuImageView_ = VK_NULL_HANDLE;
  fsrEasuFramebuffer_ = VK_NULL_HANDLE;
  fsrRcasFramebuffer_ = VK_NULL_HANDLE;
  fsrEasuParams_ = VK_NULL_HANDLE;
  fsrEasuParamsMemory_ = VK_NULL_HANDLE;
  fsrRcasParams_ = VK_NULL_HANDLE;
  fsrRcasParamsMemory_ = VK_NULL_HANDLE;
  fsrParamsCoherent_ = false;
}

bool VkRenderer::RecordSuperResolutionLocked(VkCommandBuffer cmd, VkImageView inputView) {
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
    toAttachment.image = srImage_;
    toAttachment.subresourceRange = subresource;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &toAttachment);
    srUpscale_.Record(cmd, inputView, srImageView_);
    targetToSampled(srImage_);
    return true;
  }

#if defined(HMRDP_HAVE_FSR)
  if (fsrEasuPipeline_ == VK_NULL_HANDLE || fsrRcasPipeline_ == VK_NULL_HANDLE ||
      fsrEasuFramebuffer_ == VK_NULL_HANDLE || fsrRcasFramebuffer_ == VK_NULL_HANDLE) {
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
  const auto drawPass = [&](VkFramebuffer framebuffer, VkPipeline pipeline, VkDescriptorSet set) {
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = fsrRenderPass_;
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

  drawPass(fsrEasuFramebuffer_, fsrEasuPipeline_, fsrEasuSet_);
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fsrEasuImage_;
    barrier.subresourceRange = subresource;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
  }
  drawPass(fsrRcasFramebuffer_, fsrRcasPipeline_, fsrRcasSet_);
  targetToSampled(srImage_);
  return true;
#else
  return false;
#endif
}


}  // namespace hmrdp
