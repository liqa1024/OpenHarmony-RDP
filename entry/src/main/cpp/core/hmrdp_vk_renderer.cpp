#include "hmrdp_vk_renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "present_quad.frag.h"
#include "present_quad.vert.h"


#if defined(HMRDP_HAVE_FSR)
#include "fsr_easu.frag.h"
#include "fsr_rcas.frag.h"
#endif

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string FormatName(VkFormat format) {
  switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
      return "B8G8R8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB:
      return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM:
      return "R8G8B8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB:
      return "R8G8B8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
      return "A2B10G10R10_UNORM_PACK32";
    default:
      break;
  }
  return "VkFormat(" + std::to_string(static_cast<int>(format)) + ")";
}

// Prefer BGRA8 (FreeRDP's byte order, no CPU swizzle), then RGBA8. A lone
// VK_FORMAT_UNDEFINED entry means "any" and resolves to BGRA8.
VkSurfaceFormatKHR PickSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
  if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
    VkSurfaceFormatKHR picked{};
    picked.format = VK_FORMAT_B8G8R8A8_UNORM;
    picked.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    return picked;
  }
  for (const VkSurfaceFormatKHR& format : formats) {
    if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
      return format;
    }
  }
  for (const VkSurfaceFormatKHR& format : formats) {
    if (format.format == VK_FORMAT_R8G8B8A8_UNORM) {
      return format;
    }
  }
  return formats[0];
}

VkCompositeAlphaFlagBitsKHR PickCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
  if ((supported & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  }
  if ((supported & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) != 0) {
    return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  }
  if ((supported & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0) {
    return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  }
  return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
}

// Rects one PresentBgra call may upload (matches the gdi caller's cap); longer
// input collapses into its bounding box, so the copy-region array cannot overflow.
constexpr int kMaxUploadRects = 256;

// 超分: sharpening handed to XEngine's spatial upscale, documented range [0.0, 1.0];
// a low value keeps text-heavy downscaled desktops from ringing.
constexpr float kSrSharpness = 0.2f;

// FSR: RCAS sharpening in "stops" (0.0 = max, N halves it N times). AMD's FSR 1.0
// sample value; maximum rings around glyph edges in text-heavy desktop content.
#if defined(HMRDP_HAVE_FSR)
constexpr float kFsrSharpnessStops = 0.2f;
#endif

int ClipUploadRects(const PresentRect* rects, int count, int desktopWidth, int desktopHeight,
                    PresentRect* out, int capacity) {
  auto clip = [desktopWidth, desktopHeight](PresentRect r) {
    if (r.x < 0) {
      r.width += r.x;
      r.x = 0;
    }
    if (r.y < 0) {
      r.height += r.y;
      r.y = 0;
    }
    if (r.x + r.width > desktopWidth) {
      r.width = desktopWidth - r.x;
    }
    if (r.y + r.height > desktopHeight) {
      r.height = desktopHeight - r.y;
    }
    if (r.width <= 0 || r.height <= 0) {
      r.width = 0;
      r.height = 0;
    }
    return r;
  };

  if (count > capacity) {
    int x0 = desktopWidth;
    int y0 = desktopHeight;
    int x1 = 0;
    int y1 = 0;
    for (int i = 0; i < count; ++i) {
      const PresentRect r = clip(rects[i]);
      if (r.width == 0 || r.height == 0) {
        continue;
      }
      if (r.x < x0) x0 = r.x;
      if (r.y < y0) y0 = r.y;
      if (r.x + r.width > x1) x1 = r.x + r.width;
      if (r.y + r.height > y1) y1 = r.y + r.height;
    }
    if (x1 <= x0 || y1 <= y0) {
      return 0;
    }
    out[0] = PresentRect{x0, y0, x1 - x0, y1 - y0};
    return 1;
  }

  int n = 0;
  for (int i = 0; i < count; ++i) {
    const PresentRect r = clip(rects[i]);
    if (r.width == 0 || r.height == 0) {
      continue;
    }
    out[n++] = r;
  }
  return n;
}

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

VkRenderer::VkRenderer() : videoImport_(vk_) {}

VkRenderer::~VkRenderer() {
  Reset();
}

void VkRenderer::SetSurface(void* nativeWindow, int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pendingWindow_ != nativeWindow) {
    DestroySwapchainLocked();
    vk_.DestroySurface(surface_);
    surface_ = VK_NULL_HANDLE;
  }
  pendingWindow_ = nativeWindow;
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  surfaceDirty_ = true;
}

void VkRenderer::ResizeSurface(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (width == surfaceWidth_ && height == surfaceHeight_) {
    return;
  }
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  surfaceDirty_ = true;
}

void VkRenderer::DestroySurface() {
  std::lock_guard<std::mutex> lock(mutex_);
  DestroySwapchainLocked();
  vk_.DestroySurface(surface_);
  surface_ = VK_NULL_HANDLE;
  pendingWindow_ = nullptr;
  surfaceDirty_ = true;
}

bool VkRenderer::Prepare() {
  std::lock_guard<std::mutex> lock(mutex_);
  return PresentSolidLocked(0, 0, 0);
}

bool VkRenderer::PresentSolidLocked(uint8_t r, uint8_t g, uint8_t b) {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureSwapchainLocked()) {
      return false;
    }
    const VkDevice device = context.device();
    if (device == VK_NULL_HANDLE) {
      error_ = "Vulkan device not available";
      return false;
    }
    const VkFence fence = inFlight_[frameIndex_];
    api.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result = api.AcquireNextImageKHR(device, swapchain_, UINT64_MAX,
                                              imageAvailable_[frameIndex_], VK_NULL_HANDLE,
                                              &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      surfaceDirty_ = true;
      continue;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      error_ = "vkAcquireNextImageKHR: " + VkResultName(result);
      HMRDP_LOGE("vulkan %{public}s", error_.c_str());
      return false;
    }

    api.ResetFences(device, 1, &fence);
    const VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    api.ResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api.BeginCommandBuffer(cmd, &beginInfo);

    VkClearValue clearValue{};
    clearValue.color.float32[0] = static_cast<float>(r) / 255.0f;
    clearValue.color.float32[1] = static_cast<float>(g) / 255.0f;
    clearValue.color.float32[2] = static_cast<float>(b) / 255.0f;
    clearValue.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo passBegin{};
    passBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passBegin.renderPass = renderPass_;
    passBegin.framebuffer = framebuffers_[imageIndex];
    passBegin.renderArea.offset = {0, 0};
    passBegin.renderArea.extent = extent_;
    passBegin.clearValueCount = 1;
    passBegin.pClearValues = &clearValue;
    api.CmdBeginRenderPass(cmd, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
    api.CmdEndRenderPass(cmd);

    const VkResult endResult = api.EndCommandBuffer(cmd);
    if (endResult != VK_SUCCESS) {
      error_ = "vkEndCommandBuffer: " + VkResultName(endResult);
      return false;
    }

    VkSemaphore waitSemaphores[] = {imageAvailable_[frameIndex_]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {renderFinished_[imageIndex]};
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;
    const VkResult submitResult = api.QueueSubmit(context.queue(), 1, &submitInfo, fence);
    if (submitResult != VK_SUCCESS) {
      error_ = "vkQueueSubmit: " + VkResultName(submitResult);
      HMRDP_LOGE("vulkan %{public}s", error_.c_str());
      return false;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult presentResult = api.QueuePresentKHR(context.queue(), &presentInfo);

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
      // The frame did reach the screen; rebuild before the next present.
      surfaceDirty_ = true;
      return true;
    }
    if (presentResult != VK_SUCCESS) {
      error_ = "vkQueuePresentKHR: " + VkResultName(presentResult);
      HMRDP_LOGE("vulkan %{public}s", error_.c_str());
      return false;
    }
    ++presentCount_;
    if (presentCount_ <= 3 || presentCount_ % 30 == 0) {
      HMRDP_LOGI("vulkan present #%{public}u image=%{public}u %{public}ux%{public}u",
                 static_cast<unsigned>(presentCount_), imageIndex, extent_.width,
                 extent_.height);
    }
    return true;
  }

  error_ = "swapchain out of date after re-creation";
  return false;
}

bool VkRenderer::AcquireFrameLocked(uint32_t* imageIndex, bool* retry) {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  *retry = false;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }
  const VkFence fence = inFlight_[frameIndex_];
  const int64_t waitStartUs = NowUs();
  api.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

  VkResult result = api.AcquireNextImageKHR(device, swapchain_, UINT64_MAX,
                                            imageAvailable_[frameIndex_], VK_NULL_HANDLE,
                                            imageIndex);
  // Both wait on the display, not client work: the acquire blocks until the
  // compositor returns an image. Reported as the blocked `presentWait` sub-item.
  presentWaitUs_ += static_cast<uint64_t>(NowUs() - waitStartUs);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    surfaceDirty_ = true;
    *retry = true;
    return true;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    error_ = "vkAcquireNextImageKHR: " + VkResultName(result);
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    return false;
  }
  api.ResetFences(device, 1, &fence);
  return true;
}

bool VkRenderer::SubmitAndPresentLocked(VkCommandBuffer cmd, uint32_t imageIndex) {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;

  if (api.EndCommandBuffer(cmd) != VK_SUCCESS) {
    error_ = "vkEndCommandBuffer failed";
    return false;
  }

  VkSemaphore waitSemaphores[] = {imageAvailable_[frameIndex_]};
  VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT};
  VkSemaphore signalSemaphores[] = {renderFinished_[imageIndex]};
  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;
  if (api.QueueSubmit(context.queue(), 1, &submitInfo, inFlight_[frameIndex_]) != VK_SUCCESS) {
    error_ = "vkQueueSubmit failed";
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    return false;
  }

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = signalSemaphores;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &swapchain_;
  presentInfo.pImageIndices = &imageIndex;
  const VkResult presentResult = api.QueuePresentKHR(context.queue(), &presentInfo);

  frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;

  if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
    // The frame did reach the screen; rebuild before the next present.
    surfaceDirty_ = true;
    return true;
  }
  if (presentResult != VK_SUCCESS) {
    error_ = "vkQueuePresentKHR: " + VkResultName(presentResult);
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    return false;
  }
  ++presentCount_;
  if (presentCount_ <= 3 || presentCount_ % 30 == 0) {
    HMRDP_LOGI("vulkan present #%{public}u image=%{public}u %{public}ux%{public}u",
               static_cast<unsigned>(presentCount_), imageIndex, extent_.width,
               extent_.height);
  }
  return true;
}

bool VkRenderer::EnsurePresentPipelineLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }
  if (presentPipeline_ != VK_NULL_HANDLE && renderPass_ != VK_NULL_HANDLE) {
    return true;
  }
  if (api.CreateGraphicsPipelines == nullptr || api.CreateSampler == nullptr ||
      api.CmdSetViewport == nullptr || api.CmdDraw == nullptr) {
    error_ = "graphics entry points missing";
    return false;
  }

  if (sampler_ == VK_NULL_HANDLE) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0f;
    if (api.CreateSampler(device, &info, nullptr, &sampler_) != VK_SUCCESS) {
      error_ = "vkCreateSampler failed";
      return false;
    }
  }

  if (presentSetLayout_ == VK_NULL_HANDLE) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &sampler_;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (api.CreateDescriptorSetLayout(device, &info, nullptr, &presentSetLayout_) != VK_SUCCESS) {
      error_ = "vkCreateDescriptorSetLayout (present) failed";
      return false;
    }
  }

  if (presentPipelineLayout_ == VK_NULL_HANDLE) {
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &presentSetLayout_;
    if (api.CreatePipelineLayout(device, &info, nullptr, &presentPipelineLayout_) != VK_SUCCESS) {
      error_ = "vkCreatePipelineLayout (present) failed";
      return false;
    }
  }

  if (presentPool_ == VK_NULL_HANDLE) {
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = kFramesInFlight;
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = kFramesInFlight;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    if (api.CreateDescriptorPool(device, &info, nullptr, &presentPool_) != VK_SUCCESS) {
      error_ = "vkCreateDescriptorPool (present) failed";
      return false;
    }
    VkDescriptorSetLayout layouts[kFramesInFlight];
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
      layouts[i] = presentSetLayout_;
    }
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = presentPool_;
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts = layouts;
    if (api.AllocateDescriptorSets(device, &alloc, presentSets_) != VK_SUCCESS) {
      error_ = "vkAllocateDescriptorSets (present) failed";
      return false;
    }
  }

  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  VkShaderModuleCreateInfo shaderInfo{};
  shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shaderInfo.codeSize = kPresentQuadVertSpvWords * sizeof(uint32_t);
  shaderInfo.pCode = kPresentQuadVertSpv;
  if (api.CreateShaderModule(device, &shaderInfo, nullptr, &vert) != VK_SUCCESS) {
    error_ = "vkCreateShaderModule (present vert) failed";
    return false;
  }
  shaderInfo.codeSize = kPresentQuadFragSpvWords * sizeof(uint32_t);
  shaderInfo.pCode = kPresentQuadFragSpv;
  if (api.CreateShaderModule(device, &shaderInfo, nullptr, &frag) != VK_SUCCESS) {
    api.DestroyShaderModule(device, vert, nullptr);
    error_ = "vkCreateShaderModule (present frag) failed";
    return false;
  }

  // No R/B swap in the shader: the picture image is declared in FreeRDP's byte order
  // (kPictureFormat), so the sampled value is already the semantic colour.
  const VkBool32 swapRb = VK_FALSE;
  VkSpecializationMapEntry mapEntry{};
  mapEntry.constantID = 0;
  mapEntry.offset = 0;
  mapEntry.size = sizeof(VkBool32);
  VkSpecializationInfo spec{};
  spec.mapEntryCount = 1;
  spec.pMapEntries = &mapEntry;
  spec.dataSize = sizeof(VkBool32);
  spec.pData = &swapRb;

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";
  stages[1].pSpecializationInfo = &spec;

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
  info.layout = presentPipelineLayout_;
  info.renderPass = renderPass_;
  info.subpass = 0;

  if (presentPipeline_ != VK_NULL_HANDLE) {
    api.DestroyPipeline(device, presentPipeline_, nullptr);
    presentPipeline_ = VK_NULL_HANDLE;
  }
  const VkResult result =
      api.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &presentPipeline_);
  api.DestroyShaderModule(device, vert, nullptr);
  api.DestroyShaderModule(device, frag, nullptr);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateGraphicsPipelines (present): " + VkResultName(result);
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    return false;
  }
  presentPipelineFormat_ = format_;
  return true;
}

bool VkRenderer::EnsurePresentTimerLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (presentTimer_[0] != VK_NULL_HANDLE) {
    return true;
  }
  if (device == VK_NULL_HANDLE || api.CreateQueryPool == nullptr || api.GetQueryPoolResults == nullptr ||
      api.CmdResetQueryPool == nullptr || api.CmdWriteTimestamp == nullptr ||
      api.GetPhysicalDeviceProperties == nullptr) {
    return false;
  }
  VkPhysicalDeviceProperties props{};
  api.GetPhysicalDeviceProperties(context.physicalDevice(), &props);
  if (props.limits.timestampPeriod == 0.0f) {
    return false;
  }
  VkQueryPoolCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = kPresentTimestampsPerSlot;
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    if (api.CreateQueryPool(device, &info, nullptr, &presentTimer_[slot]) != VK_SUCCESS) {
      presentTimer_[slot] = VK_NULL_HANDLE;
      return false;
    }
  }
  presentNsPerTick_ = static_cast<double>(props.limits.timestampPeriod);
  HMRDP_LOGI("vulkan present probe: timestamps ready (period=%{public}.3f ns)",
             presentNsPerTick_);
  return true;
}

void VkRenderer::CollectPresentTimerLocked(uint32_t slot) {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (presentTimer_[slot] == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
    return;
  }
  uint64_t ticks[kPresentTimestampsPerSlot] = {0, 0, 0, 0};
  // The slot's fence was waited just before this, so the submission that wrote
  // these queries has completed.
  const VkResult result =
      api.GetQueryPoolResults(device, presentTimer_[slot], 0, kPresentTimestampsPerSlot,
                              sizeof(ticks), ticks, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
  if (result != VK_SUCCESS) {
    return;
  }
  auto nsBetween = [this](uint64_t from, uint64_t to) -> uint64_t {
    return to > from ? static_cast<uint64_t>(static_cast<double>(to - from) * presentNsPerTick_)
                     : 0;
  };
  const uint64_t copyNs = nsBetween(ticks[0], ticks[1]);
  const uint64_t srNs = nsBetween(ticks[1], ticks[2]);
  const uint64_t blitNs = nsBetween(ticks[2], ticks[3]);
  presentTimerCopyNs_ += copyNs;
  presentTimerSrNs_ += srNs;
  presentTimerBlitNs_ += blitNs;
  gpuWindowCopyNs_ += copyNs;
  gpuWindowSrNs_ += srNs;
  gpuWindowBlitNs_ += blitNs;
  ++presentTimerFrames_;
  if ((presentTimerFrames_ % 30) == 0) {
    HMRDP_LOGI("vulkan present probe: gpu copy=%{public}.2fms sr=%{public}.2fms"
               " blit=%{public}.2fms total=%{public}.2fms (n=%{public}llu)",
               static_cast<double>(presentTimerCopyNs_) / 30.0 / 1e6,
               static_cast<double>(presentTimerSrNs_) / 30.0 / 1e6,
               static_cast<double>(presentTimerBlitNs_) / 30.0 / 1e6,
               static_cast<double>(presentTimerCopyNs_ + presentTimerSrNs_ + presentTimerBlitNs_) /
                   30.0 / 1e6,
               static_cast<unsigned long long>(presentTimerFrames_));
    presentTimerCopyNs_ = 0;
    presentTimerSrNs_ = 0;
    presentTimerBlitNs_ = 0;
  }
}

void VkRenderer::TakeGpuTimings(uint64_t* copyUs, uint64_t* srUs, uint64_t* blitUs) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (copyUs != nullptr) {
    *copyUs = gpuWindowCopyNs_ / 1000;
  }
  if (srUs != nullptr) {
    *srUs = gpuWindowSrNs_ / 1000;
  }
  if (blitUs != nullptr) {
    *blitUs = gpuWindowBlitNs_ / 1000;
  }
  gpuWindowCopyNs_ = 0;
  gpuWindowSrNs_ = 0;
  gpuWindowBlitNs_ = 0;
}

void VkRenderer::DestroyPresentPipelineLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device != VK_NULL_HANDLE) {
    if (presentPipeline_ != VK_NULL_HANDLE && api.DestroyPipeline != nullptr) {
      api.DestroyPipeline(device, presentPipeline_, nullptr);
    }
    if (presentPool_ != VK_NULL_HANDLE && api.DestroyDescriptorPool != nullptr) {
      api.DestroyDescriptorPool(device, presentPool_, nullptr);
    }
    if (presentPipelineLayout_ != VK_NULL_HANDLE && api.DestroyPipelineLayout != nullptr) {
      api.DestroyPipelineLayout(device, presentPipelineLayout_, nullptr);
    }
    if (presentSetLayout_ != VK_NULL_HANDLE && api.DestroyDescriptorSetLayout != nullptr) {
      api.DestroyDescriptorSetLayout(device, presentSetLayout_, nullptr);
    }
    if (sampler_ != VK_NULL_HANDLE && api.DestroySampler != nullptr) {
      api.DestroySampler(device, sampler_, nullptr);
    }
  }
  presentPipeline_ = VK_NULL_HANDLE;
  presentPool_ = VK_NULL_HANDLE;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    presentSets_[i] = VK_NULL_HANDLE;
  }
  presentPipelineLayout_ = VK_NULL_HANDLE;
  presentSetLayout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
  presentPipelineFormat_ = VK_FORMAT_UNDEFINED;
}

void VkRenderer::UpdatePresentDescriptorLocked(uint32_t slot, VkImageView view) {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE || slot >= kFramesInFlight || presentSets_[slot] == VK_NULL_HANDLE ||
      view == VK_NULL_HANDLE || sampler_ == VK_NULL_HANDLE) {
    return;
  }
  VkDescriptorImageInfo imageInfo{};
  imageInfo.sampler = sampler_;
  imageInfo.imageView = view;
  // The desktop image lives in GENERAL for its whole life (see the class comment).
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = presentSets_[slot];
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &imageInfo;
  api.UpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void VkRenderer::RecordPresentQuadLocked(VkCommandBuffer cmd, int srcWidth, int srcHeight,
                                        uint32_t imageIndex) {
  VkApi& api = vk_.api();
  if (presentPipeline_ == VK_NULL_HANDLE || presentSets_[frameIndex_] == VK_NULL_HANDLE ||
      renderPass_ == VK_NULL_HANDLE || imageIndex >= framebuffers_.size()) {
    return;
  }
  const uint32_t dstW = extent_.width;
  const uint32_t dstH = extent_.height;
  const uint32_t srcW = static_cast<uint32_t>(srcWidth);
  const uint32_t srcH = static_cast<uint32_t>(srcHeight);
  int32_t offX = 0;
  int32_t offY = 0;
  uint32_t fitW = dstW;
  uint32_t fitH = dstH;
  if (srcW != dstW || srcH != dstH) {
    const double scaleX = static_cast<double>(dstW) / static_cast<double>(srcW);
    const double scaleY = static_cast<double>(dstH) / static_cast<double>(srcH);
    const double scale = scaleX < scaleY ? scaleX : scaleY;
    fitW = static_cast<uint32_t>(static_cast<double>(srcW) * scale);
    fitH = static_cast<uint32_t>(static_cast<double>(srcH) * scale);
    if (fitW < 1) fitW = 1;
    if (fitH < 1) fitH = 1;
    offX = (static_cast<int32_t>(dstW) - static_cast<int32_t>(fitW)) / 2;
    offY = (static_cast<int32_t>(dstH) - static_cast<int32_t>(fitH)) / 2;
  }

  VkClearValue clear{};
  clear.color.float32[0] = 0.0f;
  clear.color.float32[1] = 0.0f;
  clear.color.float32[2] = 0.0f;
  clear.color.float32[3] = 1.0f;
  VkRenderPassBeginInfo passBegin{};
  passBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  passBegin.renderPass = renderPass_;
  passBegin.framebuffer = framebuffers_[imageIndex];
  passBegin.renderArea.offset = {0, 0};
  passBegin.renderArea.extent = extent_;
  passBegin.clearValueCount = 1;
  passBegin.pClearValues = &clear;
  api.CmdBeginRenderPass(cmd, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.x = static_cast<float>(offX);
  viewport.y = static_cast<float>(offY);
  viewport.width = static_cast<float>(fitW);
  viewport.height = static_cast<float>(fitH);
  viewport.maxDepth = 1.0f;
  api.CmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.offset = {offX, offY};
  scissor.extent = {fitW, fitH};
  api.CmdSetScissor(cmd, 0, 1, &scissor);

  api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipeline_);
  api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipelineLayout_, 0, 1,
                           &presentSets_[frameIndex_], 0, nullptr);
  api.CmdDraw(cmd, 3, 1, 0, 0);
  api.CmdEndRenderPass(cmd);
}

bool VkRenderer::EnsureDesktopImageLocked(int width, int height) {
  const bool match = desktopImage_ != VK_NULL_HANDLE && desktopImageWidth_ == width &&
                     desktopImageHeight_ == height && desktopImageFormat_ == kPictureFormat;
  if (match) {
    return true;
  }
  DestroyDesktopImageLocked();

  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }

  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = kPictureFormat;
  info.extent.width = static_cast<uint32_t>(width);
  info.extent.height = static_cast<uint32_t>(height);
  info.extent.depth = 1;
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult result = api.CreateImage(device, &info, nullptr, &desktopImage_);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateImage (desktop): " + VkResultName(result);
    HMRDP_LOGE("vulkan %{public}s", error_.c_str());
    return false;
  }

  VkMemoryRequirements req{};
  api.GetImageMemoryRequirements(device, desktopImage_, &req);
  // Device-local (the GPU only ever copies from it); host access goes via staging.
  const uint32_t typeIndex =
      context.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (typeIndex == UINT32_MAX) {
    error_ = "no device-local memory type for the desktop image";
    DestroyDesktopImageLocked();
    return false;
  }
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = typeIndex;
  result = api.AllocateMemory(device, &alloc, nullptr, &desktopImageMemory_);
  if (result != VK_SUCCESS) {
    error_ = "vkAllocateMemory (desktop): " + VkResultName(result);
    DestroyDesktopImageLocked();
    return false;
  }
  result = api.BindImageMemory(device, desktopImage_, desktopImageMemory_, 0);
  if (result != VK_SUCCESS) {
    error_ = "vkBindImageMemory (desktop): " + VkResultName(result);
    DestroyDesktopImageLocked();
    return false;
  }

  desktopImageFormat_ = kPictureFormat;
  desktopImageWidth_ = width;
  desktopImageHeight_ = height;
  desktopImageFullUpload_ = true;

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = desktopImage_;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = kPictureFormat;
  viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  result = api.CreateImageView(device, &viewInfo, nullptr, &desktopImageView_);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateImageView (desktop): " + VkResultName(result);
    DestroyDesktopImageLocked();
    return false;
  }
  // Every slot's descriptor points at the new view and frames still referencing the old
  // image must drain first (fences are created signaled, so waiting here is safe).
  DrainInFlightLocked();
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    UpdatePresentDescriptorLocked(i, desktopImageView_);
  }
  return true;
}

void VkRenderer::DestroyDesktopImageLocked() {
  // The 超分 output derives from the desktop (its input size is the desktop size).
  DestroySuperResolutionLocked();
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device != VK_NULL_HANDLE) {
    if (desktopImageView_ != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
      api.DestroyImageView(device, desktopImageView_, nullptr);
    }
    if (desktopImage_ != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
      api.DestroyImage(device, desktopImage_, nullptr);
    }
    if (desktopImageMemory_ != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, desktopImageMemory_, nullptr);
    }
  }
  desktopImageView_ = VK_NULL_HANDLE;
  desktopImage_ = VK_NULL_HANDLE;
  desktopImageMemory_ = VK_NULL_HANDLE;
  desktopImageFormat_ = VK_FORMAT_UNDEFINED;
  desktopImageWidth_ = 0;
  desktopImageHeight_ = 0;
  desktopImageFullUpload_ = true;
}

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

bool VkRenderer::RecordPresentTailLocked(VkCommandBuffer cmd, uint32_t slot, uint32_t imageIndex,
                                         const PresentInput& input, bool srActive, bool timers) {
  VkApi& api = vk_.api();
  // The input picture was written by the producer (a buffer->image transfer, or the video convert
  // render pass); 超分 and the letterbox both sample it next. The tail owns this dependency so every
  // producer is covered, not just the ones that remember to barrier it themselves.
  {
    VkImageMemoryBarrier ready{};
    ready.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ready.srcAccessMask = input.producerAccess;
    ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ready.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ready.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.image = input.image;
    ready.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ready.subresourceRange.levelCount = 1;
    ready.subresourceRange.layerCount = 1;
    api.CmdPipelineBarrier(cmd, input.producerStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &ready);
  }

  if (srActive) {
    // Either backend renders into `srImage_`, which replaces the input picture as the letterbox
    // source (its extent is the output resolution). Includes its own barriers.
    RecordSuperResolutionLocked(cmd, input.view);
  }

  if (timers) {
    api.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, presentTimer_[slot], 2);
  }

  const VkImageView source = srActive ? srImageView_ : input.view;
  const int sourceWidth = srActive ? srImageWidth_ : input.width;
  const int sourceHeight = srActive ? srImageHeight_ : input.height;
  UpdatePresentDescriptorLocked(slot, source);
  RecordPresentQuadLocked(cmd, sourceWidth, sourceHeight, imageIndex);
  if (timers) {
    // Outside the render pass: the blit is not portably measurable from inside one.
    api.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, presentTimer_[slot], 3);
  }
  return SubmitAndPresentLocked(cmd, imageIndex);
}

bool VkRenderer::EnsureStageLocked(size_t bytes) {
  const uint32_t slot = frameIndex_;
  if (stageBuffers_[slot] != VK_NULL_HANDLE && stageCapacities_[slot] >= bytes) {
    return true;
  }
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }
  // The mapped pointer belongs to this slot and is unmapped before replacement; the
  // slot's fence was already waited, so nothing in flight references it.
  if (stageMapped_[slot] != nullptr && api.UnmapMemory != nullptr) {
    api.UnmapMemory(device, stageMemories_[slot]);
    stageMapped_[slot] = nullptr;
  }
  if (stageBuffers_[slot] != VK_NULL_HANDLE && api.DestroyBuffer != nullptr) {
    api.DestroyBuffer(device, stageBuffers_[slot], nullptr);
    stageBuffers_[slot] = VK_NULL_HANDLE;
  }
  if (stageMemories_[slot] != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
    api.FreeMemory(device, stageMemories_[slot], nullptr);
    stageMemories_[slot] = VK_NULL_HANDLE;
  }
  stageCapacities_[slot] = 0;

  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = api.CreateBuffer(device, &info, nullptr, &stageBuffers_[slot]);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateBuffer (staging): " + VkResultName(result);
    return false;
  }

  VkMemoryRequirements req{};
  api.GetBufferMemoryRequirements(device, stageBuffers_[slot], &req);
  // Host-visible + coherent when offered (UMA); a non-coherent type still works -
  // the range is flushed explicitly below.
  uint32_t typeIndex = context.FindMemoryType(
      req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (typeIndex == UINT32_MAX) {
    typeIndex = context.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  }
  if (typeIndex == UINT32_MAX) {
    error_ = "no host-visible memory type for the staging buffer";
    return false;
  }
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = typeIndex;
  result = api.AllocateMemory(device, &alloc, nullptr, &stageMemories_[slot]);
  if (result != VK_SUCCESS) {
    error_ = "vkAllocateMemory (staging): " + VkResultName(result);
    return false;
  }
  result = api.BindBufferMemory(device, stageBuffers_[slot], stageMemories_[slot], 0);
  if (result != VK_SUCCESS) {
    error_ = "vkBindBufferMemory (staging): " + VkResultName(result);
    return false;
  }
  result = api.MapMemory(device, stageMemories_[slot], 0, VK_WHOLE_SIZE, 0,
                         &stageMapped_[slot]);
  if (result != VK_SUCCESS || stageMapped_[slot] == nullptr) {
    error_ = "vkMapMemory (staging): " + VkResultName(result);
    stageMapped_[slot] = nullptr;
    return false;
  }
  stageCapacities_[slot] = bytes;
  return true;
}

uint8_t* VkRenderer::AcquireDesktopBuffer(int width, int height, int* stride) {
  if (stride == nullptr || width <= 0 || height <= 0) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // The device is created with the swapchain, so before the first surface there is
  // nothing to allocate from; the caller keeps gdi's buffer and retries next frame.
  if (!vk_.ready()) {
    return nullptr;
  }
  if (desktopBufferMapped_ == nullptr || desktopBufferWidth_ != width ||
      desktopBufferHeight_ != height) {
    if (!CreateDesktopBufferLocked(width, height)) {
      return nullptr;
    }
  }
  *stride = desktopBufferStride_;
  return static_cast<uint8_t*>(desktopBufferMapped_);
}

void VkRenderer::BeginDesktopBufferWrite() {
  std::lock_guard<std::mutex> lock(mutex_);
  VkApi& api = vk_.api();
  const VkDevice device = vk_.device();
  if (desktopBufferFencePending_ && device != VK_NULL_HANDLE && api.WaitForFences != nullptr &&
      desktopBufferFence_ != VK_NULL_HANDLE) {
    api.WaitForFences(device, 1, &desktopBufferFence_, VK_TRUE, UINT64_MAX);
  }
  desktopBufferFencePending_ = false;
}

void VkRenderer::ReleaseDesktopBuffer() {
  std::lock_guard<std::mutex> lock(mutex_);
  DestroyDesktopBufferLocked();
}

bool VkRenderer::usesDesktopBuffer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return desktopBufferMapped_ != nullptr;
}

uint64_t VkRenderer::TakePresentWaitUs() {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t us = presentWaitUs_;
  presentWaitUs_ = 0;
  return us;
}

bool VkRenderer::CreateDesktopBufferLocked(int width, int height) {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE || api.CreateBuffer == nullptr) {
    error_ = "Vulkan device not available";
    return false;
  }
  // The previous buffer may still be read by an in-flight copy; this only runs on a
  // resize, where one wait is cheaper than tracking the range.
  if (desktopBufferFencePending_ && api.WaitForFences != nullptr &&
      desktopBufferFence_ != VK_NULL_HANDLE) {
    api.WaitForFences(device, 1, &desktopBufferFence_, VK_TRUE, UINT64_MAX);
  }
  desktopBufferFencePending_ = false;
  DestroyDesktopBufferLocked();

  const int stride = width * 4;
  const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(height);

  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkResult result = api.CreateBuffer(device, &info, nullptr, &desktopBuffer_);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateBuffer (desktop): " + VkResultName(result);
    return false;
  }
  VkMemoryRequirements req{};
  api.GetBufferMemoryRequirements(device, desktopBuffer_, &req);
  // Cached first (the CPU writes 20+ MB/frame and reads it back), then coherent -
  // see doc_agent/rdp-present.md §2. A non-coherent type is flushed per rect.
  uint32_t typeIndex = context.FindMemoryType(
      req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  if (typeIndex == UINT32_MAX) {
    typeIndex = context.FindMemoryType(
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  if (typeIndex == UINT32_MAX) {
    typeIndex = context.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
  }
  if (typeIndex == UINT32_MAX) {
    error_ = "no host-visible memory type for the desktop buffer";
    DestroyDesktopBufferLocked();
    return false;
  }
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = typeIndex;
  result = api.AllocateMemory(device, &alloc, nullptr, &desktopBufferMemory_);
  if (result != VK_SUCCESS) {
    error_ = "vkAllocateMemory (desktop buffer): " + VkResultName(result);
    DestroyDesktopBufferLocked();
    return false;
  }
  result = api.BindBufferMemory(device, desktopBuffer_, desktopBufferMemory_, 0);
  if (result != VK_SUCCESS) {
    error_ = "vkBindBufferMemory (desktop buffer): " + VkResultName(result);
    DestroyDesktopBufferLocked();
    return false;
  }
  if (api.MapMemory == nullptr) {
    error_ = "vkMapMemory unavailable";
    DestroyDesktopBufferLocked();
    return false;
  }
  result = api.MapMemory(device, desktopBufferMemory_, 0, VK_WHOLE_SIZE, 0,
                         &desktopBufferMapped_);
  if (result != VK_SUCCESS || desktopBufferMapped_ == nullptr) {
    error_ = "vkMapMemory (desktop buffer): " + VkResultName(result);
    desktopBufferMapped_ = nullptr;
    DestroyDesktopBufferLocked();
    return false;
  }
  // gdi does *not* initialise a caller-provided buffer and un-drawn desktop pixels
  // must read 0xFF (gdi_CreateCompatibleBitmap is what fills 0xFF).
  std::memset(desktopBufferMapped_, 0xFF, bytes);

  VkPhysicalDeviceMemoryProperties props{};
  context.api().GetPhysicalDeviceMemoryProperties(context.physicalDevice(), &props);
  desktopBufferCoherent_ =
      (props.memoryTypes[typeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
  desktopBufferBytes_ = bytes;
  desktopBufferWidth_ = width;
  desktopBufferHeight_ = height;
  desktopBufferStride_ = stride;
  HMRDP_LOGI("vulkan presenter: desktop buffer %{public}dx%{public}d stride=%{public}d %{public}s"
             " (gdi composes into it)",
             width, height, stride,
             desktopBufferCoherent_ ? "coherent" : "non-coherent (flushed per frame)");
  return true;
}

void VkRenderer::DestroyDesktopBufferLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device != VK_NULL_HANDLE) {
    if (desktopBufferMapped_ != nullptr && api.UnmapMemory != nullptr) {
      api.UnmapMemory(device, desktopBufferMemory_);
    }
    if (desktopBuffer_ != VK_NULL_HANDLE && api.DestroyBuffer != nullptr) {
      api.DestroyBuffer(device, desktopBuffer_, nullptr);
    }
    if (desktopBufferMemory_ != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, desktopBufferMemory_, nullptr);
    }
  }
  desktopBufferMapped_ = nullptr;
  desktopBuffer_ = VK_NULL_HANDLE;
  desktopBufferMemory_ = VK_NULL_HANDLE;
  desktopBufferBytes_ = 0;
  desktopBufferWidth_ = 0;
  desktopBufferHeight_ = 0;
  desktopBufferStride_ = 0;
  desktopBufferCoherent_ = false;
  desktopBufferFencePending_ = false;
}

void VkRenderer::DestroyStageLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  if (device != VK_NULL_HANDLE) {
    if (api.DeviceWaitIdle != nullptr) {
      api.DeviceWaitIdle(device);
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
      if (stageMapped_[i] != nullptr && api.UnmapMemory != nullptr) {
        api.UnmapMemory(device, stageMemories_[i]);
      }
      stageMapped_[i] = nullptr;
      if (stageBuffers_[i] != VK_NULL_HANDLE && api.DestroyBuffer != nullptr) {
        api.DestroyBuffer(device, stageBuffers_[i], nullptr);
      }
      stageBuffers_[i] = VK_NULL_HANDLE;
      if (stageMemories_[i] != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
        api.FreeMemory(device, stageMemories_[i], nullptr);
      }
      stageMemories_[i] = VK_NULL_HANDLE;
      stageCapacities_[i] = 0;
    }
  }
}

bool VkRenderer::PresentBgra(const uint8_t* data, int srcStride, int desktopWidth,
                                  int desktopHeight, const PresentRect* rects, int rectCount) {
  if (data == nullptr || srcStride <= 0 || desktopWidth <= 0 || desktopHeight <= 0 ||
      rects == nullptr || rectCount <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureSwapchainLocked()) {
      return false;
    }
    if (!EnsureDesktopImageLocked(desktopWidth, desktopHeight)) {
      return false;
    }
    // 超分: build the upscale target before acquiring the swapchain image, so the
    // rare (re)creation can drain frames still reading the old one.
    const bool srActive = EnsureSuperResolutionLocked(desktopWidth, desktopHeight, desktopImageView_);
    PresentRect upload[kMaxUploadRects];
    int uploadCount = 0;
    if (desktopImageFullUpload_) {
      upload[0] = PresentRect{0, 0, desktopWidth, desktopHeight};
      uploadCount = 1;
    } else {
      uploadCount = ClipUploadRects(rects, rectCount, desktopWidth, desktopHeight, upload,
                                   kMaxUploadRects);
    }
    if (uploadCount <= 0) {
      return false;
    }

    uint32_t imageIndex = 0;
    bool retry = false;
    if (!AcquireFrameLocked(&imageIndex, &retry)) {
      return false;
    }
    if (retry) {
      continue;
    }

    if (!EnsurePresentPipelineLocked()) {
      return false;
    }

    const VkDevice device = context.device();
    // SubmitAndPresentLocked advances frameIndex_, so capture the active slot first.
    const uint32_t slot = frameIndex_;
    // The slot's fence was just waited, so the previous timestamps are readable.
    const bool timers = EnsurePresentTimerLocked();
    if (timers) {
      CollectPresentTimerLocked(slot);
    }
    // When gdi composed into our desktop buffer the rects are read in place at the desktop
    // row pitch (no CPU copy); otherwise they are staged first.
    const bool direct =
        data == static_cast<const uint8_t*>(desktopBufferMapped_) && srcStride == desktopBufferStride_ &&
        desktopBuffer_ != VK_NULL_HANDLE && desktopWidth == desktopBufferWidth_ &&
        desktopHeight == desktopBufferHeight_;
    if (direct) {
      // gdi's host writes must be visible to the transfer read; non-coherent types
      // need this flush (a no-op when coherent).
      if (!desktopBufferCoherent_ && api.FlushMappedMemoryRanges != nullptr) {
        // One range spanning the uploaded rects: per-rect ranges overlap heavily and
        // cost a driver call each, while a flush only writes back dirty lines.
        VkDeviceSize first = desktopBufferBytes_;
        VkDeviceSize last = 0;
        for (int i = 0; i < uploadCount; ++i) {
          const PresentRect& r = upload[i];
          const VkDeviceSize begin =
              static_cast<VkDeviceSize>(r.y) * static_cast<VkDeviceSize>(desktopBufferStride_) +
              static_cast<VkDeviceSize>(r.x) * 4u;
          VkDeviceSize end = begin + static_cast<VkDeviceSize>(r.height - 1) *
                                         static_cast<VkDeviceSize>(desktopBufferStride_) +
                             static_cast<VkDeviceSize>(r.width) * 4u;
          if (end > desktopBufferBytes_) {
            end = desktopBufferBytes_;
          }
          if (begin < first) {
            first = begin;
          }
          if (end > last) {
            last = end;
          }
        }
        if (last > first) {
          VkMappedMemoryRange range{};
          range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          range.memory = desktopBufferMemory_;
          range.offset = first;
          range.size = last - first;
          api.FlushMappedMemoryRanges(device, 1, &range);
        }
      }
    } else {
      // This slot's fence was just waited and it has not been re-submitted, so its staging
      // buffer is free; uploads are packed back to back, each keeping its own row pitch.
      size_t stageBytes = 0;
      for (int i = 0; i < uploadCount; ++i) {
        stageBytes += static_cast<size_t>(upload[i].width) * 4u *
                      static_cast<size_t>(upload[i].height);
      }
      if (!EnsureStageLocked(stageBytes)) {
        return false;
      }
      // Rows are copied verbatim; the channel order is fixed on the GPU by the
      // shader (a per-byte CPU swap cost more than the whole rest of the present).
      uint8_t* stage = static_cast<uint8_t*>(stageMapped_[slot]);
      size_t stageOffset = 0;
      for (int i = 0; i < uploadCount; ++i) {
        const PresentRect& r = upload[i];
        const size_t rowBytes = static_cast<size_t>(r.width) * 4u;
        if (static_cast<size_t>(srcStride) == rowBytes) {
          std::memcpy(stage + stageOffset,
                      data + static_cast<size_t>(r.y) * static_cast<size_t>(srcStride) +
                          static_cast<size_t>(r.x) * 4u,
                      rowBytes * static_cast<size_t>(r.height));
        } else {
          for (int row = 0; row < r.height; ++row) {
            const uint8_t* srcRow =
                data + static_cast<size_t>(r.y + row) * static_cast<size_t>(srcStride) +
                static_cast<size_t>(r.x) * 4u;
            std::memcpy(stage + stageOffset + static_cast<size_t>(row) * rowBytes, srcRow, rowBytes);
          }
        }
        stageOffset += rowBytes * static_cast<size_t>(r.height);
      }
      // Flush exactly what was written (the buffer grows to the largest dirty rect,
      // so VK_WHOLE_SIZE would flush far more). No-op on coherent memory.
      if (api.FlushMappedMemoryRanges != nullptr) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = stageMemories_[slot];
        range.offset = 0;
        range.size = stageBytes;
        api.FlushMappedMemoryRanges(device, 1, &range);
      }
    }

    const VkCommandBuffer cmd = commandBuffers_[slot];
    api.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api.BeginCommandBuffer(cmd, &beginInfo);
    if (timers) {
      api.CmdResetQueryPool(cmd, presentTimer_[slot], 0, kPresentTimestampsPerSlot);
      api.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, presentTimer_[slot], 0);
    }

    // Host writes -> transfer read (the staging buffer is not host-read, so a plain
    // memory barrier suffices).
    VkMemoryBarrier hostToTransfer{};
    hostToTransfer.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostToTransfer.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                           &hostToTransfer, 0, nullptr, 0, nullptr);

    VkBufferImageCopy regions[kMaxUploadRects];
    size_t regionOffset = 0;
    for (int i = 0; i < uploadCount; ++i) {
      const PresentRect& r = upload[i];
      VkBufferImageCopy& region = regions[i];
      region = VkBufferImageCopy{};
      if (direct) {
        // The rect is read where gdi left it: at the desktop row pitch, rows
        // contiguous inside the rect (stride == width * 4).
        region.bufferOffset =
            static_cast<VkDeviceSize>(r.y) * static_cast<VkDeviceSize>(desktopBufferStride_) +
            static_cast<VkDeviceSize>(r.x) * 4u;
        region.bufferRowLength = static_cast<uint32_t>(desktopBufferStride_ / 4);
      } else {
        region.bufferOffset = regionOffset;
        region.bufferRowLength = static_cast<uint32_t>(r.width);
        regionOffset += static_cast<size_t>(r.width) * 4u * static_cast<size_t>(r.height);
      }
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.layerCount = 1;
      region.imageOffset.x = r.x;
      region.imageOffset.y = r.y;
      region.imageExtent.width = static_cast<uint32_t>(r.width);
      region.imageExtent.height = static_cast<uint32_t>(r.height);
      region.imageExtent.depth = 1;
    }
    api.CmdCopyBufferToImage(cmd, direct ? desktopBuffer_ : stageBuffers_[slot], desktopImage_,
                             VK_IMAGE_LAYOUT_GENERAL, static_cast<uint32_t>(uploadCount),
                             regions);
    if (timers) {
      api.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, presentTimer_[slot], 1);
    }

    // The present tail reads the upload in the same command buffer; it declares that dependency
    // itself (see RecordPresentTailLocked), so the desktop is described by how it was written here.
    PresentInput input;
    input.image = desktopImage_;
    input.view = desktopImageView_;
    input.width = desktopWidth;
    input.height = desktopHeight;
    input.producerStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    input.producerAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    const bool presented = RecordPresentTailLocked(cmd, slot, imageIndex, input, srActive, timers);
    if (presented) {
      desktopImageFullUpload_ = false;
      if (direct) {
        // This submission reads gdi's desktop buffer; the next frame must not
        // overwrite it before this fence signals (BeginDesktopBufferWrite).
        desktopBufferFence_ = inFlight_[slot];
        desktopBufferFencePending_ = true;
      }
    }
    return presented;
  }

  error_ = "swapchain out of date after re-creation";
  return false;
}

void VkRenderer::DrainInFlightLocked() {
  VkApi& api = vk_.api();
  const VkDevice device = vk_.device();
  if (device != VK_NULL_HANDLE && api.WaitForFences != nullptr && inFlight_[0] != VK_NULL_HANDLE) {
    api.WaitForFences(device, kFramesInFlight, inFlight_, VK_TRUE, UINT64_MAX);
  }
}

bool VkRenderer::PresentVideo(const void* nativeBuffer, const VideoFrameGeometry& geometry) {
  const int visibleWidth = geometry.visibleWidth;
  const int visibleHeight = geometry.visibleHeight;
  if (nativeBuffer == nullptr || visibleWidth <= 0 || visibleHeight <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureSwapchainLocked()) {
      return false;
    }
    const VkDevice device = context.device();
    if (device == VK_NULL_HANDLE) {
      return false;
    }
    // The importer's picture is the session resolution and only changes on a resolution switch;
    // drain the frames still reading the old one before it is rebuilt (which drops the imports too).
    // The same holds when the import cache is at its cap and a new buffer has to be imported.
    if (videoImport_.needsRebuild(visibleWidth, visibleHeight) || videoImport_.importCacheFull()) {
      DrainInFlightLocked();
    }
    if (!videoImport_.Ensure(visibleWidth, visibleHeight)) {
      error_ = videoImport_.lastError();
      return false;
    }
    VkVideoImport::Frame imported{};
    if (!videoImport_.Import(nativeBuffer, geometry, &imported)) {
      error_ = videoImport_.lastError();
      return false;
    }
    // 超分: build the upscale target before acquiring, so a rare (re)creation can drain.
    const bool srActive =
        EnsureSuperResolutionLocked(visibleWidth, visibleHeight, videoImport_.pictureView());

    uint32_t imageIndex = 0;
    bool retry = false;
    if (!AcquireFrameLocked(&imageIndex, &retry)) {
      return false;
    }
    if (retry) {
      continue;
    }
    if (!EnsurePresentPipelineLocked()) {
      return false;
    }

    const uint32_t slot = frameIndex_;
    const bool timers = EnsurePresentTimerLocked();
    if (timers) {
      CollectPresentTimerLocked(slot);
    }
    const VkCommandBuffer cmd = commandBuffers_[slot];
    api.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api.BeginCommandBuffer(cmd, &beginInfo);
    if (timers) {
      api.CmdResetQueryPool(cmd, presentTimer_[slot], 0, kPresentTimestampsPerSlot);
      api.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, presentTimer_[slot], 0);
    }
    videoImport_.RecordConvert(cmd, imported);
    if (timers) {
      api.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, presentTimer_[slot],
                            1);
    }

    // The convert pass just wrote the picture image; the shared tail declares that dependency
    // itself, so the video picture is described by how this pass wrote it.
    PresentInput input;
    input.image = videoImport_.pictureImage();
    input.view = videoImport_.pictureView();
    input.width = visibleWidth;
    input.height = visibleHeight;
    input.producerStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    input.producerAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const bool presented = RecordPresentTailLocked(cmd, slot, imageIndex, input, srActive, timers);
    // The caller returns the decoded buffer to the codec as soon as this returns, so the GPU work
    // reading it has to be finished here; the video path is present-paced anyway.
    if (api.WaitForFences != nullptr) {
      api.WaitForFences(device, 1, &inFlight_[slot], VK_TRUE, UINT64_MAX);
    }
    return presented;
  }
  return false;
}

void VkRenderer::Reset() {
  const int64_t t0 = NowUs();
  std::lock_guard<std::mutex> lock(mutex_);
  DestroySwapchainLocked();
  DestroyDesktopBufferLocked();
  DestroyDesktopImageLocked();
  videoImport_.Destroy();
  DestroyFrameResourcesLocked();
  vk_.DestroySurface(surface_);
  surface_ = VK_NULL_HANDLE;
  vk_.Destroy();
  pendingWindow_ = nullptr;
  surfaceDirty_ = true;
  error_.clear();
  info_.clear();
  HMRDP_LOGI("vulkan presenter: reset %{public}llu ms",
             static_cast<unsigned long long>((NowUs() - t0) / 1000));
}

bool VkRenderer::EnsureSwapchainLocked() {
  if (pendingWindow_ == nullptr) {
    error_ = "XComponent surface not bound";
    return false;
  }
  if (swapchain_ != VK_NULL_HANDLE && !surfaceDirty_) {
    return true;
  }
  if (!CreateSwapchainLocked()) {
    return false;
  }
  surfaceDirty_ = false;
  return true;
}

bool VkRenderer::CreateSwapchainLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;

  if (surface_ == VK_NULL_HANDLE) {
    if (!context.CreateSurface(pendingWindow_, &surface_)) {
      error_ = context.lastError();
      return false;
    }
  }
  // A device is only usable for a surface it can present to, so it is created lazily
  // here; its entry points are only resolved by this call, hence the checks after.
  if (!context.EnsureDevice(surface_)) {
    error_ = context.lastError();
    context.DestroySurface(surface_);
    surface_ = VK_NULL_HANDLE;
    return false;
  }
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }
  if (api.CreateSwapchainKHR == nullptr || api.GetPhysicalDeviceSurfaceCapabilitiesKHR == nullptr) {
    error_ = "VK_KHR_swapchain entry points missing";
    return false;
  }

  VkSurfaceCapabilitiesKHR caps{};
  VkResult result = api.GetPhysicalDeviceSurfaceCapabilitiesKHR(context.physicalDevice(), surface_, &caps);
  if (result != VK_SUCCESS) {
    error_ = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR: " + VkResultName(result);
    return false;
  }

  uint32_t formatCount = 0;
  api.GetPhysicalDeviceSurfaceFormatsKHR(context.physicalDevice(), surface_, &formatCount, nullptr);
  if (formatCount == 0) {
    error_ = "surface reports no format";
    return false;
  }
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  api.GetPhysicalDeviceSurfaceFormatsKHR(context.physicalDevice(), surface_, &formatCount, formats.data());
  const VkSurfaceFormatKHR surfaceFormat = PickSurfaceFormat(formats);

  uint32_t presentModeCount = 0;
  api.GetPhysicalDeviceSurfacePresentModesKHR(context.physicalDevice(), surface_, &presentModeCount, nullptr);
  std::vector<VkPresentModeKHR> presentModes(presentModeCount);
  if (presentModeCount > 0) {
    api.GetPhysicalDeviceSurfacePresentModesKHR(context.physicalDevice(), surface_, &presentModeCount, presentModes.data());
  }
  // FIFO is the only mode the spec guarantees and matches the "present only when
  // the picture changed" policy.
  VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
  bool fifoAvailable = false;
  for (VkPresentModeKHR mode : presentModes) {
    if (mode == VK_PRESENT_MODE_FIFO_KHR) {
      fifoAvailable = true;
    }
  }
  if (!fifoAvailable && presentModeCount > 0) {
    presentMode = presentModes[0];
  }

  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
    error_ = "swapchain images cannot be colour attachments";
    return false;
  }

  VkExtent2D extent{};
  if (caps.currentExtent.width != UINT32_MAX) {
    extent = caps.currentExtent;
  } else {
    extent.width = std::clamp(static_cast<uint32_t>(surfaceWidth_ < 1 ? 1 : surfaceWidth_),
                              caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(static_cast<uint32_t>(surfaceHeight_ < 1 ? 1 : surfaceHeight_),
                               caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
    imageCount = caps.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = surface_;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  createInfo.preTransform = caps.currentTransform;
  createInfo.compositeAlpha = PickCompositeAlpha(caps.supportedCompositeAlpha);
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;
  createInfo.oldSwapchain = swapchain_;

  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  result = api.CreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateSwapchainKHR: " + VkResultName(result);
    return false;
  }

  // Tear the old swapchain down only after the new one exists (oldSwapchain already
  // handed its images back).
  DestroySwapchainLocked();
  swapchain_ = swapchain;
  format_ = surfaceFormat.format;
  extent_ = extent;

  uint32_t swapImageCount = 0;
  api.GetSwapchainImagesKHR(device, swapchain_, &swapImageCount, nullptr);
  if (swapImageCount == 0) {
    error_ = "swapchain reports no image";
    return false;
  }
  images_.resize(swapImageCount);
  api.GetSwapchainImagesKHR(device, swapchain_, &swapImageCount, images_.data());
  if (images_.empty()) {
    error_ = "swapchain images unavailable";
    return false;
  }

  const bool renderPassReused = renderPass_ != VK_NULL_HANDLE;
  // Recreate the render pass only when the format changed: an out-of-date swapchain
  // is normally just a resize, where rebuilding it is pure churn.
  if (renderPass_ != VK_NULL_HANDLE && renderPassFormat_ != format_) {
    if (api.DestroyRenderPass != nullptr) {
      api.DestroyRenderPass(device, renderPass_, nullptr);
    }
    renderPass_ = VK_NULL_HANDLE;
  }
  if (renderPass_ == VK_NULL_HANDLE) {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = format_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    result = api.CreateRenderPass(device, &renderPassInfo, nullptr, &renderPass_);
    renderPassFormat_ = result == VK_SUCCESS ? format_ : VK_FORMAT_UNDEFINED;
    if (result != VK_SUCCESS) {
      error_ = "vkCreateRenderPass: " + VkResultName(result);
      return false;
    }
  }

  views_.resize(images_.size(), VK_NULL_HANDLE);
  framebuffers_.resize(images_.size(), VK_NULL_HANDLE);
  for (size_t i = 0; i < images_.size(); ++i) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = images_[i];
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format_;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    result = api.CreateImageView(device, &viewInfo, nullptr, &views_[i]);
    if (result != VK_SUCCESS) {
      error_ = "vkCreateImageView: " + VkResultName(result);
      return false;
    }

    VkImageView attachments[] = {views_[i]};
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass_;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = extent_.width;
    framebufferInfo.height = extent_.height;
    framebufferInfo.layers = 1;
    result = api.CreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers_[i]);
    if (result != VK_SUCCESS) {
      error_ = "vkCreateFramebuffer: " + VkResultName(result);
      return false;
    }
  }

  renderFinished_.resize(images_.size(), VK_NULL_HANDLE);
  for (size_t i = 0; i < renderFinished_.size(); ++i) {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    result = api.CreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished_[i]);
    if (result != VK_SUCCESS) {
      error_ = "vkCreateSemaphore: " + VkResultName(result);
      return false;
    }
  }

  if (commandPool_ == VK_NULL_HANDLE) {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context.queueFamily();
    result = api.CreateCommandPool(device, &poolInfo, nullptr, &commandPool_);
    if (result != VK_SUCCESS) {
      error_ = "vkCreateCommandPool: " + VkResultName(result);
      return false;
    }

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = kFramesInFlight;
    result = api.AllocateCommandBuffers(device, &allocateInfo, commandBuffers_);
    if (result != VK_SUCCESS) {
      error_ = "vkAllocateCommandBuffers: " + VkResultName(result);
      return false;
    }
  }

  if (imageAvailable_[0] == VK_NULL_HANDLE) {
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
      VkSemaphoreCreateInfo semaphoreInfo{};
      semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      result = api.CreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailable_[i]);
      if (result != VK_SUCCESS) {
        error_ = "vkCreateSemaphore: " + VkResultName(result);
        return false;
      }

      VkFenceCreateInfo fenceInfo{};
      fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      result = api.CreateFence(device, &fenceInfo, nullptr, &inFlight_[i]);
      if (result != VK_SUCCESS) {
        error_ = "vkCreateFence: " + VkResultName(result);
        return false;
      }
    }
  }
  frameIndex_ = 0;

  char buf[192];
  std::snprintf(buf, sizeof(buf), "swapchain %ux%u %s images=%zu fifo=%d renderPass=%s",
                extent_.width, extent_.height, FormatName(format_).c_str(), images_.size(),
                presentMode == VK_PRESENT_MODE_FIFO_KHR ? 1 : 0,
                renderPassReused ? "reused" : "created");
  info_ = buf;
  HMRDP_LOGI("vulkan %{public}s", buf);
  return true;
}

void VkRenderer::DestroySwapchainLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();

  if (swapchain_ != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
    if (api.DeviceWaitIdle != nullptr) {
      api.DeviceWaitIdle(device);
    }
    if (api.DestroySwapchainKHR != nullptr) {
      api.DestroySwapchainKHR(device, swapchain_, nullptr);
    }
  }
  swapchain_ = VK_NULL_HANDLE;

  if (device != VK_NULL_HANDLE) {
    for (VkSemaphore semaphore : renderFinished_) {
      if (semaphore != VK_NULL_HANDLE && api.DestroySemaphore != nullptr) {
        api.DestroySemaphore(device, semaphore, nullptr);
      }
    }
    for (VkFramebuffer framebuffer : framebuffers_) {
      if (framebuffer != VK_NULL_HANDLE && api.DestroyFramebuffer != nullptr) {
        api.DestroyFramebuffer(device, framebuffer, nullptr);
      }
    }
    for (VkImageView view : views_) {
      if (view != VK_NULL_HANDLE && api.DestroyImageView != nullptr) {
        api.DestroyImageView(device, view, nullptr);
      }
    }
  }
  // renderPass_ deliberately survives: it is rebuilt only on a format change and
  // released with the rest of the frame resources.
  renderFinished_.clear();
  framebuffers_.clear();
  views_.clear();
  images_.clear();
  format_ = VK_FORMAT_UNDEFINED;
  extent_ = {0, 0};
}

void VkRenderer::DestroyFrameResourcesLocked() {
  VkApi& api = vk_.api();
  VkDeviceContext& context = vk_;
  const VkDevice device = context.device();
  DestroyStageLocked();
  DestroyPresentPipelineLocked();
  // The render pass outlives individual swapchains, so it is released here (full
  // teardown) rather than in DestroySwapchainLocked().
  if (device != VK_NULL_HANDLE && renderPass_ != VK_NULL_HANDLE &&
      api.DestroyRenderPass != nullptr) {
    api.DestroyRenderPass(device, renderPass_, nullptr);
  }
  renderPass_ = VK_NULL_HANDLE;
  renderPassFormat_ = VK_FORMAT_UNDEFINED;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (presentTimer_[i] != VK_NULL_HANDLE && device != VK_NULL_HANDLE &&
        api.DestroyQueryPool != nullptr) {
      api.DestroyQueryPool(device, presentTimer_[i], nullptr);
    }
    presentTimer_[i] = VK_NULL_HANDLE;
  }
  presentNsPerTick_ = 0.0;
  presentTimerCopyNs_ = 0;
  presentTimerSrNs_ = 0;
  presentTimerBlitNs_ = 0;
  presentTimerFrames_ = 0;
  gpuWindowCopyNs_ = 0;
  gpuWindowSrNs_ = 0;
  gpuWindowBlitNs_ = 0;

  if (device == VK_NULL_HANDLE) {
    commandPool_ = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
      commandBuffers_[i] = VK_NULL_HANDLE;
      imageAvailable_[i] = VK_NULL_HANDLE;
      inFlight_[i] = VK_NULL_HANDLE;
    }
    return;
  }
  if (api.DeviceWaitIdle != nullptr) {
    api.DeviceWaitIdle(device);
  }
  if (commandPool_ != VK_NULL_HANDLE) {
    if (api.DestroyCommandPool != nullptr) {
      api.DestroyCommandPool(device, commandPool_, nullptr);
    }
    commandPool_ = VK_NULL_HANDLE;
  }
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    commandBuffers_[i] = VK_NULL_HANDLE;
    if (imageAvailable_[i] != VK_NULL_HANDLE && api.DestroySemaphore != nullptr) {
      api.DestroySemaphore(device, imageAvailable_[i], nullptr);
    }
    imageAvailable_[i] = VK_NULL_HANDLE;
    if (inFlight_[i] != VK_NULL_HANDLE && api.DestroyFence != nullptr) {
      api.DestroyFence(device, inFlight_[i], nullptr);
    }
    inFlight_[i] = VK_NULL_HANDLE;
  }
  frameIndex_ = 0;
}

std::string VkRenderer::lastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

std::string VkRenderer::Describe() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (swapchain_ == VK_NULL_HANDLE) {
    return error_.empty() ? "vulkan: no swapchain" : "vulkan: " + error_;
  }
  return info_;
}

}  // namespace hmrdp
