#include "hmrdp_vk_video.h"

#include <cstddef>

#include <native_buffer/native_buffer.h>
#include <multimedia/player_framework/native_avformat.h>

#include "hmrdp_log.h"
#include "video_convert.frag.h"
#include "video_convert.vert.h"

namespace hmrdp {

VkVideoImport::VkVideoImport(VkDeviceContext& context) : context_(context) {}

VkVideoImport::~VkVideoImport() {
  Destroy();
}

bool VkVideoImport::needsRebuild(int pictureWidth, int pictureHeight) const {
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    if (pictures_[slot].view == VK_NULL_HANDLE ||
        pictures_[slot].width != pictureWidth || pictures_[slot].height != pictureHeight) {
      return true;
    }
  }
  return false;
}

bool VkVideoImport::needsDrainFor(const void* nativeBuffer) const {
  if (imports_.size() < kMaxImports) {
    return false;
  }
  if (nativeBuffer == nullptr) {
    return true;
  }
  const struct OH_NativeBuffer* buffer =
      static_cast<const struct OH_NativeBuffer*>(nativeBuffer);
  const uint32_t seq =
      OH_NativeBuffer_GetSeqNum(const_cast<struct OH_NativeBuffer*>(buffer));
  for (const Imported& imported : imports_) {
    if (imported.seq == seq) {
      return false;
    }
  }
  return true;
}

bool VkVideoImport::Ensure(int pictureWidth, int pictureHeight) {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device == VK_NULL_HANDLE || pictureWidth <= 0 || pictureHeight <= 0) {
    return false;
  }
  if (failed_ && failedWidth_ == pictureWidth && failedHeight_ == pictureHeight) {
    return false;
  }
  if (!needsRebuild(pictureWidth, pictureHeight)) {
    return true;
  }
  Destroy();
  failed_ = false;
  const auto fail = [this, pictureWidth, pictureHeight](const char* what) -> bool {
    error_ = what;
    Destroy();
    failed_ = true;
    failedWidth_ = pictureWidth;
    failedHeight_ = pictureHeight;
    return false;
  };
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    if (!CreateDeviceImage(api, context_, kPictureFormat, pictureWidth, pictureHeight,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           &pictures_[slot].image, &pictures_[slot].memory)) {
      return fail("video picture allocation failed");
    }
    if (!CreateImageView2D(api, context_, kPictureFormat, pictures_[slot].image,
                           &pictures_[slot].view)) {
      return fail("video picture view failed");
    }
    pictures_[slot].width = pictureWidth;
    pictures_[slot].height = pictureHeight;
  }
  if (!EnsureConvertPipeline()) {
    failed_ = true;
    failedWidth_ = pictureWidth;
    failedHeight_ = pictureHeight;
    return false;
  }
  return true;
}

void VkVideoImport::DestroyConvertPipeline() {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device != VK_NULL_HANDLE) {
    if (convertPipeline_ != VK_NULL_HANDLE && api.DestroyPipeline != nullptr) {
      api.DestroyPipeline(device, convertPipeline_, nullptr);
    }
    if (convertPool_ != VK_NULL_HANDLE && api.DestroyDescriptorPool != nullptr) {
      api.DestroyDescriptorPool(device, convertPool_, nullptr);
    }
    if (convertPipelineLayout_ != VK_NULL_HANDLE && api.DestroyPipelineLayout != nullptr) {
      api.DestroyPipelineLayout(device, convertPipelineLayout_, nullptr);
    }
    if (convertSetLayout_ != VK_NULL_HANDLE && api.DestroyDescriptorSetLayout != nullptr) {
      api.DestroyDescriptorSetLayout(device, convertSetLayout_, nullptr);
    }
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
      if (convertFramebuffer_[slot] != VK_NULL_HANDLE && api.DestroyFramebuffer != nullptr) {
        api.DestroyFramebuffer(device, convertFramebuffer_[slot], nullptr);
      }
    }
    if (convertRenderPass_ != VK_NULL_HANDLE && api.DestroyRenderPass != nullptr) {
      api.DestroyRenderPass(device, convertRenderPass_, nullptr);
    }
    if (byteSampler_ != VK_NULL_HANDLE && api.DestroySampler != nullptr) {
      api.DestroySampler(device, byteSampler_, nullptr);
    }
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
      if (paramsMapped_[slot] != nullptr && api.UnmapMemory != nullptr) {
        api.UnmapMemory(device, paramsMemory_[slot]);
      }
      if (paramsBuffer_[slot] != VK_NULL_HANDLE && api.DestroyBuffer != nullptr) {
        api.DestroyBuffer(device, paramsBuffer_[slot], nullptr);
      }
      if (paramsMemory_[slot] != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
        api.FreeMemory(device, paramsMemory_[slot], nullptr);
      }
    }
  }
  byteSampler_ = VK_NULL_HANDLE;
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    paramsBuffer_[slot] = VK_NULL_HANDLE;
    paramsMemory_[slot] = VK_NULL_HANDLE;
    paramsMapped_[slot] = nullptr;
    paramsCoherent_[slot] = true;
    convertFramebuffer_[slot] = VK_NULL_HANDLE;
  }
  convertPipeline_ = VK_NULL_HANDLE;
  convertPool_ = VK_NULL_HANDLE;
  convertPipelineLayout_ = VK_NULL_HANDLE;
  convertSetLayout_ = VK_NULL_HANDLE;
  convertRenderPass_ = VK_NULL_HANDLE;
}

void VkVideoImport::DestroyImported(Imported* imported) {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device != VK_NULL_HANDLE) {
    if (imported->view != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
      api.DestroyImageView(device, imported->view, nullptr);
    }
    if (imported->image != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
      api.DestroyImage(device, imported->image, nullptr);
    }
    if (imported->memory != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, imported->memory, nullptr);
    }
  }
  *imported = Imported();
}

void VkVideoImport::ClearImports() {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  for (Imported& imported : imports_) {
    DestroyImported(&imported);
  }
  imports_.clear();
  // The sets belong to the pool; resetting it hands them back. The caller has drained the frames
  // that may still have bound them.
  if (device != VK_NULL_HANDLE && convertPool_ != VK_NULL_HANDLE &&
      api.ResetDescriptorPool != nullptr) {
    api.ResetDescriptorPool(device, convertPool_, 0);
  }
}

void VkVideoImport::Destroy() {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  ClearImports();
  DestroyConvertPipeline();
  if (device != VK_NULL_HANDLE) {
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
      if (pictures_[slot].view != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
        api.DestroyImageView(device, pictures_[slot].view, nullptr);
      }
      if (pictures_[slot].image != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
        api.DestroyImage(device, pictures_[slot].image, nullptr);
      }
      if (pictures_[slot].memory != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
        api.FreeMemory(device, pictures_[slot].memory, nullptr);
      }
      pictures_[slot] = Picture();
    }
  } else {
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
      pictures_[slot] = Picture();
    }
  }
  failed_ = false;
  failedWidth_ = 0;
  failedHeight_ = 0;
  useCounter_ = 0;
  formatWarned_ = false;
  colorFallbackWarned_ = false;
  importFailed_ = false;
}

bool VkVideoImport::EnsureConvertPipeline() {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device == VK_NULL_HANDLE) {
    return false;
  }
  if (convertPipeline_ != VK_NULL_HANDLE) {
    return true;
  }
  // Rewritten every frame (DONT_CARE) and left in GENERAL so the 超分 / letterbox tail samples it
  // exactly like the desktop picture.
  {
    VkAttachmentDescription attachment{};
    attachment.format = kPictureFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
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
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    if (api.CreateRenderPass(device, &info, nullptr, &convertRenderPass_) != VK_SUCCESS) {
      error_ = "video convert render pass failed";
      Destroy();
      return false;
    }
  }
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = convertRenderPass_;
    info.attachmentCount = 1;
    info.pAttachments = &pictures_[slot].view;
    info.width = static_cast<uint32_t>(pictures_[slot].width);
    info.height = static_cast<uint32_t>(pictures_[slot].height);
    info.layers = 1;
    if (api.CreateFramebuffer(device, &info, nullptr, &convertFramebuffer_[slot]) != VK_SUCCESS) {
      error_ = "video convert framebuffer failed";
      Destroy();
      return false;
    }
  }
  {
    // A plain sampler: the shader fetches the buffer's bytes with texelFetch, so filtering and
    // address modes are irrelevant - and no YCbCr conversion is involved anywhere.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (api.CreateSampler(device, &samplerInfo, nullptr, &byteSampler_) != VK_SUCCESS) {
      byteSampler_ = VK_NULL_HANDLE;
      error_ = "video convert sampler failed";
      Destroy();
      return false;
    }
  }
  {
    // Two bindings: the byte image, and the frame parameters in a uniform buffer (the same route the
    // presenter's FSR passes use; the fragment stage cannot rely on push constants here).
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 2;
    info.pBindings = bindings;
    if (api.CreateDescriptorSetLayout(device, &info, nullptr, &convertSetLayout_) != VK_SUCCESS) {
      error_ = "video convert set layout failed";
      Destroy();
      return false;
    }
  }
  {
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &convertSetLayout_;
    if (api.CreatePipelineLayout(device, &info, nullptr, &convertPipelineLayout_) != VK_SUCCESS) {
      error_ = "video convert pipeline layout failed";
      Destroy();
      return false;
    }
  }
  {
    // One set per imported buffer: a set is written when its buffer is first imported and then never
    // touched again, so the per-frame path has no descriptor write.
    VkDescriptorPoolSize sizes[2] = {};
    const uint32_t totalSets = static_cast<uint32_t>(kMaxImports * kSlots);
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = totalSets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = totalSets;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = totalSets;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    if (api.CreateDescriptorPool(device, &info, nullptr, &convertPool_) != VK_SUCCESS) {
      error_ = "video convert pool failed";
      Destroy();
      return false;
    }
  }
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    // Host-visible: rewritten every frame, like the FSR parameter buffers.
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kFrameParamsBytes;
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (api.CreateBuffer(device, &info, nullptr, &paramsBuffer_[slot]) != VK_SUCCESS) {
      paramsBuffer_[slot] = VK_NULL_HANDLE;
      error_ = "video convert params buffer failed";
      Destroy();
      return false;
    }
    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(device, paramsBuffer_[slot], &req);
    uint32_t typeIndex = context_.FindMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    paramsCoherent_[slot] = typeIndex != UINT32_MAX;
    if (!paramsCoherent_[slot]) {
      // Same fallback as the desktop staging path: visible but not coherent still works, with an
      // explicit flush after each frame's write.
      typeIndex = context_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (typeIndex == UINT32_MAX) {
      error_ = "video convert params memory type failed";
      Destroy();
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = typeIndex;
    if (api.AllocateMemory(device, &alloc, nullptr, &paramsMemory_[slot]) != VK_SUCCESS) {
      error_ = "video convert params allocation failed";
      Destroy();
      return false;
    }
    api.BindBufferMemory(device, paramsBuffer_[slot], paramsMemory_[slot], 0);
    if (api.MapMemory(device, paramsMemory_[slot], 0, VK_WHOLE_SIZE, 0,
                      &paramsMapped_[slot]) != VK_SUCCESS) {
      paramsMapped_[slot] = nullptr;
      error_ = "video convert params map failed";
      Destroy();
      return false;
    }
  }

  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  VkShaderModuleCreateInfo shaderInfo{};
  shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shaderInfo.codeSize = kVideoConvertVertSpvWords * sizeof(uint32_t);
  shaderInfo.pCode = kVideoConvertVertSpv;
  if (api.CreateShaderModule(device, &shaderInfo, nullptr, &vert) != VK_SUCCESS) {
    error_ = "video convert vert module failed";
    Destroy();
    return false;
  }
  shaderInfo.codeSize = kVideoConvertFragSpvWords * sizeof(uint32_t);
  shaderInfo.pCode = kVideoConvertFragSpv;
  if (api.CreateShaderModule(device, &shaderInfo, nullptr, &frag) != VK_SUCCESS) {
    api.DestroyShaderModule(device, vert, nullptr);
    error_ = "video convert frag module failed";
    Destroy();
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
  info.layout = convertPipelineLayout_;
  info.renderPass = convertRenderPass_;
  info.subpass = 0;
  const VkResult result =
      api.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &convertPipeline_);
  api.DestroyShaderModule(device, vert, nullptr);
  api.DestroyShaderModule(device, frag, nullptr);
  if (result != VK_SUCCESS) {
    error_ = "video convert pipeline failed";
    Destroy();
    return false;
  }
  return true;
}

// The YUV -> RGB rows for 8-bit samples, as `value = cY*Y + cCb*Cb + cCr*Cr + offset`. Narrow range
// expects 16..235 luma / 16..240 chroma, full range 0..255.
void VkVideoImport::UpdateColorRows(int colorSpace, int suggestedModel, int suggestedRange,
                                    float* out) {
  // OH_COLORSPACE_* (native_buffer.h): BT.601 EBU/SMPTE-C and BT.709, each with a FULL/LIMIT pair.
  // Anything else falls back to what the driver suggested for this buffer (2 = BT.709).
  bool full = false;
  bool bt601 = false;
  switch (colorSpace) {
    case OH_COLORSPACE_BT601_EBU_FULL:
    case OH_COLORSPACE_BT601_SMPTE_C_FULL:
      full = true;
      bt601 = true;
      break;
    case OH_COLORSPACE_BT601_EBU_LIMIT:
    case OH_COLORSPACE_BT601_SMPTE_C_LIMIT:
      bt601 = true;
      break;
    case OH_COLORSPACE_BT709_FULL:
      full = true;
      break;
    case OH_COLORSPACE_BT709_LIMIT:
      break;
    default:
      if (colorSpace == OH_COLORSPACE_NONE) {
        // The SDR stream contract is Rec.709 limited. An unreported buffer must not fall back to the
        // driver's RGB identity / full-range suggestion: for YUV that is the wrong matrix and range.
        if (!colorFallbackWarned_) {
          colorFallbackWarned_ = true;
          HMRDP_LOGW("vulkan video import: buffer reports no colour space; assuming BT.709 limited");
        }
        full = false;
        bt601 = false;
      } else {
        // 0 = RGB identity (no conversion), 1 = BT.601, 2 = BT.709; 0/1 = full/limited.
        bt601 = suggestedModel == 1;
        full = suggestedRange == 0;
      }
      break;
  }
  const float kb = bt601 ? 0.114f : 0.0722f;  // blue-difference weight
  const float kr = bt601 ? 0.299f : 0.2126f;  // red-difference weight
  const float cCb = 2.0f * (1.0f - kb);
  const float cCr = 2.0f * (1.0f - kr);
  const float cY = full ? 1.0f : 255.0f / 219.0f;
  const float yOffset = full ? 0.0f : 16.0f;
  const float chromaOffset = 128.0f;
  const float scale = full ? 1.0f : 255.0f / 224.0f;
  // R = cY*(Y - 16) + cCr*scale*(Cr - 128); G = cY*(Y - 16) - (kb*cCb*scale)/(1-kr-kb)*(Cb - 128)
  //   - (kr*cCr*scale)/(1-kr-kb)*(Cr - 128); B = cY*(Y - 16) + cCb*scale*(Cb - 128).
  const float rCb = 0.0f;
  const float rCr = cCr * scale;
  const float gCb = -(kb * cCb * scale) / (1.0f - kr - kb);
  const float gCr = -(kr * cCr * scale) / (1.0f - kr - kb);
  const float bCb = cCb * scale;
  const float bCr = 0.0f;
  const float yTerm = -cY * yOffset;
  out[0] = cY;
  out[1] = rCb;
  out[2] = rCr;
  out[3] = yTerm - chromaOffset * (rCb + rCr);
  out[4] = cY;
  out[5] = gCb;
  out[6] = gCr;
  out[7] = yTerm - chromaOffset * (gCb + gCr);
  out[8] = cY;
  out[9] = bCb;
  out[10] = bCr;
  out[11] = yTerm - chromaOffset * (bCb + bCr);
}

bool VkVideoImport::AllocateImportSet(VkImageView view, uint32_t slot, VkDescriptorSet* out) {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device == VK_NULL_HANDLE || out == nullptr || slot >= kSlots) {
    return false;
  }
  VkDescriptorSetAllocateInfo setInfo{};
  setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  setInfo.descriptorPool = convertPool_;
  setInfo.descriptorSetCount = 1;
  setInfo.pSetLayouts = &convertSetLayout_;
  if (api.AllocateDescriptorSets(device, &setInfo, out) != VK_SUCCESS) {
    error_ = "video convert set allocation failed";
    return false;
  }
  VkDescriptorImageInfo imageDescriptor{};
  imageDescriptor.sampler = byteSampler_;
  imageDescriptor.imageView = view;
  imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorBufferInfo paramsDescriptor{};
  paramsDescriptor.buffer = paramsBuffer_[slot];
  paramsDescriptor.offset = 0;
  paramsDescriptor.range = kFrameParamsBytes;
  VkWriteDescriptorSet writes[2] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = *out;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[0].pImageInfo = &imageDescriptor;
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = *out;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[1].pBufferInfo = &paramsDescriptor;
  api.UpdateDescriptorSets(device, 2, writes, 0, nullptr);
  return true;
}

void VkVideoImport::RebuildSets() {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device == VK_NULL_HANDLE || convertPool_ == VK_NULL_HANDLE) {
    return;
  }
  if (api.ResetDescriptorPool != nullptr) {
    api.ResetDescriptorPool(device, convertPool_, 0);
  }
  for (Imported& imported : imports_) {
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
      imported.sets[slot] = VK_NULL_HANDLE;
      if (!AllocateImportSet(imported.view, slot, &imported.sets[slot])) {
        error_ = "video convert set rebuild failed";
      }
    }
  }
}

bool VkVideoImport::CreateImported(const void* nativeBuffer, const VideoFrameGeometry& geometry,
                                   Imported* out) {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  const struct OH_NativeBuffer* buffer = static_cast<const struct OH_NativeBuffer*>(nativeBuffer);
  struct OH_NativeBuffer* mutableBuffer = const_cast<struct OH_NativeBuffer*>(buffer);
  VkNativeBufferFormatPropertiesOHOS formatProperties{};
  formatProperties.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS;
  VkNativeBufferPropertiesOHOS properties{};
  properties.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS;
  properties.pNext = &formatProperties;
  if (api.GetNativeBufferPropertiesOHOS(device, buffer, &properties) != VK_SUCCESS) {
    error_ = "vkGetNativeBufferPropertiesOHOS failed";
    return false;
  }
  OH_NativeBuffer_Config nativeConfig{};
  OH_NativeBuffer_GetConfig(mutableBuffer, &nativeConfig);
  const int pixelFormat = geometry.pixelFormat;
  if (pixelFormat > 0 && pixelFormat != AV_PIXEL_FORMAT_NV12 &&
      pixelFormat != AV_PIXEL_FORMAT_NV21) {
    // The configured NV12 request was not honoured. Addressing another layout as interleaved 420
    // would produce wrong colours, so refuse rather than present garbage.
    error_ = "unsupported decoder pixel format " + std::to_string(pixelFormat);
    return false;
  }
  if (pixelFormat == 0 && !formatWarned_) {
    formatWarned_ = true;
    HMRDP_LOGW("vulkan video import: the codec reported no pixel format; assuming NV12");
  }
  const int visibleWidth = geometry.visibleWidth > 0 ? geometry.visibleWidth : nativeConfig.width;
  const int visibleHeight =
      geometry.visibleHeight > 0 ? geometry.visibleHeight : nativeConfig.height;
  // The codec's own description of the frame comes first (`stride` in bytes, `sliceHeight` rows per
  // plane: the chroma plane starts at their product). Where it states nothing - an output surface
  // hides the layout behind the allocator - the allocation's config is the only reading left, and its
  // plane height has to be derived rather than read.
  const int bufferStride =
      nativeConfig.stride > nativeConfig.width ? nativeConfig.stride : nativeConfig.width;
  const int stride = geometry.stride > 0 ? geometry.stride : bufferStride;
  int sliceHeight = geometry.sliceHeight > 0 ? geometry.sliceHeight : nativeConfig.height;
  if (geometry.sliceHeight <= 0) {
    sliceHeight = sliceHeight > 0 ? (sliceHeight + 63) / 64 * 64 : visibleHeight;
    if (!derivedLayoutLogged_) {
      derivedLayoutLogged_ = true;
      HMRDP_LOGW("vulkan video import: the codec reported no plane height; deriving it from the "
                 "allocation (%{public}d rows)", sliceHeight);
    }
  }
  if (visibleWidth <= 0 || visibleHeight <= 0 || stride <= 0) {
    error_ = "video buffer geometry is unusable";
    return false;
  }
  if (sliceHeight < visibleHeight) {
    // The luma plane cannot hold the picture: every byte after it is chroma, not luma.
    error_ = "video plane height " + std::to_string(sliceHeight) + " is below the picture " +
             std::to_string(visibleHeight);
    return false;
  }
  const uint32_t imageWidth = static_cast<uint32_t>(stride > visibleWidth ? stride : visibleWidth);
  const uint32_t imageHeight = static_cast<uint32_t>(sliceHeight + sliceHeight / 2);
  // The byte image covers the whole allocation; if the allocation cannot hold that layout, the
  // description and the buffer disagree and reading it would sample garbage.
  if (properties.allocationSize <
      static_cast<VkDeviceSize>(imageWidth) * static_cast<VkDeviceSize>(imageHeight)) {
    error_ = "video buffer is smaller than its described layout (" +
             std::to_string(properties.allocationSize) + " < " + std::to_string(imageWidth) + "x" +
             std::to_string(imageHeight) + ")";
    return false;
  }
  OH_NativeBuffer_ColorSpace colorSpace = OH_COLORSPACE_NONE;
  OH_NativeBuffer_GetColorSpace(mutableBuffer, &colorSpace);
  float rows[12] = {};
  UpdateColorRows(static_cast<int>(colorSpace),
                  static_cast<int>(formatProperties.suggestedYcbcrModel),
                  static_cast<int>(formatProperties.suggestedYcbcrRange), rows);
  for (int i = 0; i < 12; ++i) {
    out->colorRows[i] = rows[i];
  }
  out->cbFirst = geometry.cbFirst;
  if (!importLogged_) {
    importLogged_ = true;
    HMRDP_LOGI("vulkan video import: %{public}llu bytes, fmt=%{public}d cs=%{public}d, byte image "
               "%{public}ux%{public}u (chroma row %{public}d), visible %{public}dx%{public}d at "
               "%{public}d,%{public}d",
               static_cast<unsigned long long>(properties.allocationSize),
               static_cast<int>(nativeConfig.format), static_cast<int>(colorSpace), imageWidth,
               imageHeight, sliceHeight, visibleWidth, visibleHeight, geometry.originX,
               geometry.originY);
  }

  // A byte image over the whole allocation: plain R8, linear (the buffer is linear memory), declared
  // externally backed so the driver binds the imported memory rather than allocating its own.
  VkExternalMemoryImageCreateInfo externalMemory{};
  externalMemory.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  externalMemory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.pNext = &externalMemory;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = VK_FORMAT_R8_UNORM;
  imageInfo.extent.width = imageWidth;
  imageInfo.extent.height = imageHeight;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
  imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (api.CreateImage(device, &imageInfo, nullptr, &out->image) != VK_SUCCESS) {
    error_ = "vkCreateImage (video byte image) failed: R8/LINEAR not accepted for this buffer";
    return false;
  }

  // The imported buffer *is* the image's memory, so the allocation must be dedicated to it; the
  // import info hangs off the dedicated-allocation info (the official import recipe).
  uint32_t memoryType =
      context_.FindMemoryType(properties.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memoryType == UINT32_MAX) {
    memoryType = context_.FindMemoryType(properties.memoryTypeBits, 0);
  }
  if (memoryType == UINT32_MAX) {
    error_ = "no memory type for the imported video buffer";
    DestroyImported(out);
    return false;
  }
  VkImportNativeBufferInfoOHOS import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS;
  import.buffer = mutableBuffer;
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.pNext = &import;
  dedicated.image = out->image;
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &dedicated;
  alloc.allocationSize = properties.allocationSize;
  alloc.memoryTypeIndex = memoryType;
  if (api.AllocateMemory(device, &alloc, nullptr, &out->memory) != VK_SUCCESS) {
    error_ = "vkAllocateMemory (video import) failed";
    DestroyImported(out);
    return false;
  }
  if (api.BindImageMemory(device, out->image, out->memory, 0) != VK_SUCCESS) {
    error_ = "vkBindImageMemory (video import) failed";
    DestroyImported(out);
    return false;
  }

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = out->image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R8_UNORM;
  viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  if (api.CreateImageView(device, &viewInfo, nullptr, &out->view) != VK_SUCCESS) {
    error_ = "vkCreateImageView (video import) failed";
    DestroyImported(out);
    return false;
  }

  // Each slot's set is written once, here, and bound every frame afterwards.
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    if (!AllocateImportSet(out->view, slot, &out->sets[slot])) {
      DestroyImported(out);
      return false;
    }
  }

  out->chromaRow = sliceHeight;
  return true;
}

bool VkVideoImport::Import(const void* nativeBuffer, const VideoFrameGeometry& geometry,
                           uint32_t slot, Frame* out) {
  const VkDevice device = context_.device();
  VkApi& api = context_.api();
  if (device == VK_NULL_HANDLE || nativeBuffer == nullptr || out == nullptr || slot >= kSlots) {
    return false;
  }
  if (importFailed_) {
    // The descriptor set of a failed import may already be partially built; retrying at the stream
    // rate would keep taking sets from the pool (and resetting the pool would invalidate the sets
    // the other frame slot may still use). Latch until the picture is rebuilt or the session ends.
    return false;
  }
  if (api.GetNativeBufferPropertiesOHOS == nullptr) {
    error_ = "video import needs VK_OHOS_external_memory";
    if (!importWarned_) {
      importWarned_ = true;
      HMRDP_LOGE("vulkan video import: %{public}s; 超分 cannot present", error_.c_str());
    }
    return false;
  }
  struct OH_NativeBuffer* buffer =
      const_cast<struct OH_NativeBuffer*>(static_cast<const struct OH_NativeBuffer*>(nativeBuffer));
  const uint32_t seq = OH_NativeBuffer_GetSeqNum(buffer);
  for (Imported& imported : imports_) {
    if (imported.seq == seq) {
      imported.lastUsed = ++useCounter_;
      WriteFrameParams(geometry, imported.chromaRow, imported.colorRows, slot);
      out->image = imported.image;
      out->set = imported.sets[slot];
      return true;
    }
  }
  if (imports_.size() >= kMaxImports) {
    // The caller has drained the frames that may still read the cached imports. Evict the least
    // recently used one; resetting the pool invalidates every set, so the survivors are re-written.
    size_t oldest = 0;
    for (size_t i = 1; i < imports_.size(); ++i) {
      if (imports_[i].lastUsed < imports_[oldest].lastUsed) {
        oldest = i;
      }
    }
    DestroyImported(&imports_[oldest]);
    imports_.erase(imports_.begin() + static_cast<std::ptrdiff_t>(oldest));
    RebuildSets();
  }
  Imported created;
  created.seq = seq;
  created.lastUsed = ++useCounter_;
  if (!CreateImported(nativeBuffer, geometry, &created)) {
    // One-shot, in the one place every import failure passes through: a refused frame would
    // otherwise repeat at the stream rate, and the reason is what matters.
    if (!importWarned_) {
      importWarned_ = true;
      HMRDP_LOGE("vulkan video import: %{public}s; 超分 cannot present", error_.c_str());
    }
    importFailed_ = true;
    return false;
  }
  imports_.push_back(created);
  WriteFrameParams(geometry, created.chromaRow, created.colorRows, slot);
  out->image = created.image;
  out->set = created.sets[slot];
  return true;
}

void VkVideoImport::WriteFrameParams(const VideoFrameGeometry& geometry, int chromaRow,
                                     const float* colorRows, uint32_t slot) {
  if (slot >= kSlots || paramsMapped_[slot] == nullptr || colorRows == nullptr) {
    return;
  }
  float* values = static_cast<float*>(paramsMapped_[slot]);
  values[0] = static_cast<float>(geometry.visibleWidth);
  values[1] = static_cast<float>(geometry.visibleHeight);
  values[2] = static_cast<float>(geometry.originX);
  values[3] = static_cast<float>(geometry.originY);
  values[4] = static_cast<float>(chromaRow);
  values[5] = geometry.cbFirst ? 1.0f : 0.0f;
  values[6] = 0.0f;
  values[7] = 0.0f;
  for (int i = 0; i < 12; ++i) {
    values[8 + i] = colorRows[i];
  }
  if (!paramsCoherent_[slot]) {
    VkApi& api = context_.api();
    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE && api.FlushMappedMemoryRanges != nullptr) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = paramsMemory_[slot];
      range.offset = 0;
      range.size = kFrameParamsBytes;
      api.FlushMappedMemoryRanges(device, 1, &range);
    }
  }
}

void VkVideoImport::RecordConvert(VkCommandBuffer cmd, const Frame& frame, uint32_t slot) {
  VkApi& api = context_.api();
  if (slot >= kSlots || pictures_[slot].view == VK_NULL_HANDLE) {
    return;
  }
  // Imported byte image: written by the external producer, only ever fetched (read-only layout).
  {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = frame.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &barrier);
  }
  {
    const Picture& picture = pictures_[slot];
    VkViewport viewport{};
    viewport.width = static_cast<float>(picture.width);
    viewport.height = static_cast<float>(picture.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent.width = static_cast<uint32_t>(picture.width);
    scissor.extent.height = static_cast<uint32_t>(picture.height);
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = convertRenderPass_;
    begin.framebuffer = convertFramebuffer_[slot];
    begin.renderArea = scissor;
    api.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    api.CmdSetViewport(cmd, 0, 1, &viewport);
    api.CmdSetScissor(cmd, 0, 1, &scissor);
    api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convertPipeline_);
    api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convertPipelineLayout_, 0, 1,
                              &frame.set, 0, nullptr);
    api.CmdDraw(cmd, 3, 1, 0, 0);
    api.CmdEndRenderPass(cmd);
  }
}

}
