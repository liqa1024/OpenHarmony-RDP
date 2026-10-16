#include "hmrdp_vk_video.h"

#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#include "hmrdp_log.h"
#include "video_convert.frag.h"
#include "video_convert.vert.h"

namespace hmrdp {

VkVideoImport::VkVideoImport(VkDeviceContext& context) : context_(context) {}

VkVideoImport::~VkVideoImport() {
  Destroy();
}

bool VkVideoImport::needsRebuild(int pictureWidth, int pictureHeight) const {
  return picture_.view == VK_NULL_HANDLE || picture_.width != pictureWidth ||
         picture_.height != pictureHeight;
}

bool VkVideoImport::Ensure(int pictureWidth, int pictureHeight) {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device == VK_NULL_HANDLE || pictureWidth <= 0 || pictureHeight <= 0) {
    return false;
  }
  if (!needsRebuild(pictureWidth, pictureHeight)) {
    return true;
  }
  if (api.GetNativeBufferPropertiesOHOS == nullptr) {
    error_ = "video import needs VK_OHOS_external_memory";
    return false;
  }
  Destroy();
  if (!CreateDeviceImage(api, context_, kPictureFormat, pictureWidth, pictureHeight,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         &picture_.image, &picture_.memory)) {
    error_ = "video picture allocation failed";
    Destroy();
    return false;
  }
  if (!CreateImageView2D(api, context_, kPictureFormat, picture_.image, &picture_.view)) {
    error_ = "video picture view failed";
    Destroy();
    return false;
  }
  picture_.width = pictureWidth;
  picture_.height = pictureHeight;
  return EnsureConvertPipeline();
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
    if (convertFramebuffer_ != VK_NULL_HANDLE && api.DestroyFramebuffer != nullptr) {
      api.DestroyFramebuffer(device, convertFramebuffer_, nullptr);
    }
    if (convertRenderPass_ != VK_NULL_HANDLE && api.DestroyRenderPass != nullptr) {
      api.DestroyRenderPass(device, convertRenderPass_, nullptr);
    }
    if (byteSampler_ != VK_NULL_HANDLE && api.DestroySampler != nullptr) {
      api.DestroySampler(device, byteSampler_, nullptr);
    }
    if (paramsMapped_ != nullptr && api.UnmapMemory != nullptr) {
      api.UnmapMemory(device, paramsMemory_);
      paramsMapped_ = nullptr;
    }
    if (paramsBuffer_ != VK_NULL_HANDLE && api.DestroyBuffer != nullptr) {
      api.DestroyBuffer(device, paramsBuffer_, nullptr);
    }
    if (paramsMemory_ != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, paramsMemory_, nullptr);
    }
  }
  byteSampler_ = VK_NULL_HANDLE;
  paramsBuffer_ = VK_NULL_HANDLE;
  paramsMemory_ = VK_NULL_HANDLE;
  paramsMapped_ = nullptr;
  convertPipeline_ = VK_NULL_HANDLE;
  convertPool_ = VK_NULL_HANDLE;
  convertSet_ = VK_NULL_HANDLE;
  convertPipelineLayout_ = VK_NULL_HANDLE;
  convertSetLayout_ = VK_NULL_HANDLE;
  convertFramebuffer_ = VK_NULL_HANDLE;
  convertRenderPass_ = VK_NULL_HANDLE;
}

void VkVideoImport::Destroy() {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  DestroyConvertPipeline();
  if (device != VK_NULL_HANDLE) {
    if (picture_.view != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
      api.DestroyImageView(device, picture_.view, nullptr);
    }
    if (picture_.image != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
      api.DestroyImage(device, picture_.image, nullptr);
    }
    if (picture_.memory != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, picture_.memory, nullptr);
    }
  }
  picture_ = Picture{};
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
  {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = convertRenderPass_;
    info.attachmentCount = 1;
    info.pAttachments = &picture_.view;
    info.width = static_cast<uint32_t>(picture_.width);
    info.height = static_cast<uint32_t>(picture_.height);
    info.layers = 1;
    if (api.CreateFramebuffer(device, &info, nullptr, &convertFramebuffer_) != VK_SUCCESS) {
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
    VkDescriptorPoolSize sizes[2] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 1;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    if (api.CreateDescriptorPool(device, &info, nullptr, &convertPool_) != VK_SUCCESS) {
      error_ = "video convert pool failed";
      Destroy();
      return false;
    }
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = convertPool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &convertSetLayout_;
    if (api.AllocateDescriptorSets(device, &alloc, &convertSet_) != VK_SUCCESS) {
      error_ = "video convert set allocation failed";
      Destroy();
      return false;
    }
  }
  {
    // Host-visible coherent: rewritten every frame, like the FSR parameter buffers.
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kFrameParamsBytes;
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (api.CreateBuffer(device, &info, nullptr, &paramsBuffer_) != VK_SUCCESS) {
      paramsBuffer_ = VK_NULL_HANDLE;
      error_ = "video convert params buffer failed";
      Destroy();
      return false;
    }
    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(device, paramsBuffer_, &req);
    const uint32_t typeIndex = context_.FindMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (typeIndex == UINT32_MAX) {
      error_ = "video convert params memory type failed";
      Destroy();
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = typeIndex;
    if (api.AllocateMemory(device, &alloc, nullptr, &paramsMemory_) != VK_SUCCESS) {
      error_ = "video convert params allocation failed";
      Destroy();
      return false;
    }
    api.BindBufferMemory(device, paramsBuffer_, paramsMemory_, 0);
    if (api.MapMemory(device, paramsMemory_, 0, VK_WHOLE_SIZE, 0, &paramsMapped_) != VK_SUCCESS) {
      paramsMapped_ = nullptr;
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
void VkVideoImport::UpdateColorRows(int colorSpace, int suggestedModel, int suggestedRange) {
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
      // 0 = RGB identity (no conversion), 1 = BT.601, 2 = BT.709; 0/1 = full/limited.
      bt601 = suggestedModel == 1;
      full = suggestedRange == 0;
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
  colorRows_[0] = cY;
  colorRows_[1] = rCb;
  colorRows_[2] = rCr;
  colorRows_[3] = yTerm - chromaOffset * (rCb + rCr);
  colorRows_[4] = cY;
  colorRows_[5] = gCb;
  colorRows_[6] = gCr;
  colorRows_[7] = yTerm - chromaOffset * (gCb + gCr);
  colorRows_[8] = cY;
  colorRows_[9] = bCb;
  colorRows_[10] = bCr;
  colorRows_[11] = yTerm - chromaOffset * (bCb + bCr);
}

bool VkVideoImport::Import(const void* nativeBuffer, int visibleWidth, int visibleHeight,
                           Frame* out) {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device == VK_NULL_HANDLE || nativeBuffer == nullptr || out == nullptr) {
    return false;
  }
  const struct OH_NativeBuffer* buffer =
      static_cast<const struct OH_NativeBuffer*>(nativeBuffer);
  VkNativeBufferFormatPropertiesOHOS formatProperties{};
  formatProperties.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS;
  VkNativeBufferPropertiesOHOS properties{};
  properties.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS;
  properties.pNext = &formatProperties;
  if (api.GetNativeBufferPropertiesOHOS(device, buffer, &properties) != VK_SUCCESS) {
    error_ = "vkGetNativeBufferPropertiesOHOS failed";
    if (!importWarned_) {
      importWarned_ = true;
      HMRDP_LOGE("vulkan video import: %{public}s; 超分 cannot present", error_.c_str());
    }
    return false;
  }
  // Geometry: the buffer is imported as a byte image, so its extent is the allocation's own shape.
  // A multi-plane buffer's planes are placed by the allocator, not by the visible picture size: the
  // row pitch is generally wider than the visible width and the luma plane's height is aligned up,
  // with the interleaved chroma plane immediately after it. The image is therefore
  // (pitch x lumaRows + chromaRows) bytes and the convert pass addresses luma and chroma in exactly
  // those terms — see doc_agent/rdp-present.md §5 for why the declared geometry must be this one.
  OH_NativeBuffer_Config nativeConfig{};
  OH_NativeBuffer_GetConfig(const_cast<struct OH_NativeBuffer*>(buffer), &nativeConfig);
  const int pitch = nativeConfig.stride > nativeConfig.width ? nativeConfig.stride
                                                             : nativeConfig.width;
  const int configHeight = nativeConfig.height > 0 ? nativeConfig.height : visibleHeight;
  const int lumaRows = (configHeight + 63) / 64 * 64;
  const int chromaRows = lumaRows / 2;
  const uint32_t imageWidth = static_cast<uint32_t>(pitch);
  const uint32_t imageHeight = static_cast<uint32_t>(lumaRows + chromaRows);
  if (imageWidth <= 0 || imageHeight <= 0) {
    error_ = "video buffer geometry is unusable";
    return false;
  }
  OH_NativeBuffer_ColorSpace colorSpace = OH_COLORSPACE_NONE;
  OH_NativeBuffer_GetColorSpace(const_cast<struct OH_NativeBuffer*>(buffer), &colorSpace);
  UpdateColorRows(static_cast<int>(colorSpace),
                  static_cast<int>(formatProperties.suggestedYcbcrModel),
                  static_cast<int>(formatProperties.suggestedYcbcrRange));
  if (!importLogged_) {
    importLogged_ = true;
    HMRDP_LOGI("vulkan video import: %{public}llu bytes, media %{public}dx%{public}d "
               "stride=%{public}d fmt=%{public}d cs=%{public}d, byte image %{public}ux%{public}u "
               "(luma %{public}d rows), visible %{public}dx%{public}d",
               static_cast<unsigned long long>(properties.allocationSize), nativeConfig.width,
               nativeConfig.height, nativeConfig.stride, static_cast<int>(nativeConfig.format),
               static_cast<int>(colorSpace), imageWidth, imageHeight, lumaRows, visibleWidth,
               visibleHeight);
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
    error_ = "vkCreateImage (video byte image) failed";
    if (!importWarned_) {
      importWarned_ = true;
      HMRDP_LOGE("vulkan video import: %{public}s (R8/LINEAR is not accepted for this buffer)",
                 error_.c_str());
    }
    return false;
  }

  // The imported buffer *is* the image's memory, so the allocation must be dedicated to it; the
  // import info hangs off the dedicated-allocation info (the official import recipe).
  uint32_t memoryType = context_.FindMemoryType(properties.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memoryType == UINT32_MAX) {
    memoryType = context_.FindMemoryType(properties.memoryTypeBits, 0);
  }
  if (memoryType == UINT32_MAX) {
    error_ = "no memory type for the imported video buffer";
    Release(out);
    return false;
  }
  VkImportNativeBufferInfoOHOS import{};
  import.sType = VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS;
  import.buffer = const_cast<struct OH_NativeBuffer*>(buffer);
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
    Release(out);
    return false;
  }
  if (api.BindImageMemory(device, out->image, out->memory, 0) != VK_SUCCESS) {
    error_ = "vkBindImageMemory (video import) failed";
    Release(out);
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
    Release(out);
    return false;
  }
  out->bufferWidth = static_cast<int>(imageWidth);
  out->bufferHeight = static_cast<int>(imageHeight);
  out->chromaRow = lumaRows;
  return true;
}

void VkVideoImport::Release(Frame* frame) {
  if (frame == nullptr) {
    return;
  }
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
  if (device != VK_NULL_HANDLE) {
    if (frame->view != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
      api.DestroyImageView(device, frame->view, nullptr);
    }
    if (frame->image != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
      api.DestroyImage(device, frame->image, nullptr);
    }
    if (frame->memory != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, frame->memory, nullptr);
    }
  }
  frame->view = VK_NULL_HANDLE;
  frame->image = VK_NULL_HANDLE;
  frame->memory = VK_NULL_HANDLE;
}


bool VkVideoImport::WriteFrameParams(const Frame& frame, int visibleWidth, int visibleHeight) {
  if (paramsMapped_ == nullptr) {
    return false;
  }
  float* values = static_cast<float*>(paramsMapped_);
  values[0] = static_cast<float>(visibleWidth);
  values[1] = static_cast<float>(visibleHeight);
  values[2] = static_cast<float>(frame.chromaRow);
  values[3] = 0.0f;
  for (int i = 0; i < 12; ++i) {
    values[4 + i] = colorRows_[i];
  }
  return true;
}

void VkVideoImport::RecordConvert(VkCommandBuffer cmd, const Frame& frame, int visibleWidth,
                                  int visibleHeight) {
  VkApi& api = context_.api();
  const VkDevice device = context_.device();
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
  WriteFrameParams(frame, visibleWidth, visibleHeight);
  {
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = byteSampler_;
    imageInfo.imageView = frame.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo paramsInfo{};
    paramsInfo.buffer = paramsBuffer_;
    paramsInfo.offset = 0;
    paramsInfo.range = kFrameParamsBytes;
    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = convertSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = convertSet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &paramsInfo;
    api.UpdateDescriptorSets(device, 2, writes, 0, nullptr);
  }
  {
    VkViewport viewport{};
    viewport.width = static_cast<float>(picture_.width);
    viewport.height = static_cast<float>(picture_.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent.width = static_cast<uint32_t>(picture_.width);
    scissor.extent.height = static_cast<uint32_t>(picture_.height);
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = convertRenderPass_;
    begin.framebuffer = convertFramebuffer_;
    begin.renderArea = scissor;
    api.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    api.CmdSetViewport(cmd, 0, 1, &viewport);
    api.CmdSetScissor(cmd, 0, 1, &scissor);
    api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convertPipeline_);
    api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, convertPipelineLayout_, 0, 1,
                              &convertSet_, 0, nullptr);
    api.CmdDraw(cmd, 3, 1, 0, 0);
    api.CmdEndRenderPass(cmd);
  }
}

}
