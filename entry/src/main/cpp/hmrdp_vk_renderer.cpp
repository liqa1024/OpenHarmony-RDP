/*
 * HmRdp - Vulkan presenter implementation. See hmrdp_vk_renderer.h and
 * doc_agent/gfx-engine.md §3.
 */
#include "hmrdp_vk_renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

// Prefer BGRA8 (FreeRDP's own byte order, so the engine needs no swizzle), then
// RGBA8 (the engine then swaps R/B on its CPU boundaries). Colour space is
// irrelevant here: the presenter never samples, it only blits. A single
// VK_FORMAT_UNDEFINED entry means "any" and is resolved to BGRA8.
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

}  // namespace

VkRenderer::VkRenderer() = default;

VkRenderer::~VkRenderer() {
  Reset();
}

void VkRenderer::SetSurface(void* nativeWindow, int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pendingWindow_ != nativeWindow) {
    // A new native window means the old VkSurfaceKHR is unusable.
    DestroySwapchainLocked();
    VkContext::Instance().DestroySurface(surface_);
    surface_ = VK_NULL_HANDLE;
  }
  pendingWindow_ = nativeWindow;
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  surfaceDirty_ = true;
}

void VkRenderer::ResizeSurface(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  surfaceDirty_ = true;
}

void VkRenderer::DestroySurface() {
  std::lock_guard<std::mutex> lock(mutex_);
  DestroySwapchainLocked();
  VkContext::Instance().DestroySurface(surface_);
  surface_ = VK_NULL_HANDLE;
  pendingWindow_ = nullptr;
  surfaceDirty_ = true;
}

bool VkRenderer::Prepare() {
  std::lock_guard<std::mutex> lock(mutex_);
  return PresentSolidLocked(0, 0, 0);
}

bool VkRenderer::PresentSolidLocked(uint8_t r, uint8_t g, uint8_t b) {
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();

  // Two attempts: a surface that went out of date between presents is rebuilt
  // once, then retried (the retry normally succeeds with the fresh swapchain).
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureSwapchainLocked()) {
      return false;
    }
    // The device is created by EnsureSwapchainLocked (a queue family can only be
    // chosen once the surface exists), so it must be read after it.
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

    // The bring-up frame draws nothing: the render pass attachment itself
    // carries the clear, so no pipeline or shader is needed to light up the
    // surface.
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
    // Periodic heartbeat: proves continuous acquire/present on a real-device run
    // without needing the dev panel or a screenshot.
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
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();
  *retry = false;
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }
  const VkFence fence = inFlight_[frameIndex_];
  api.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

  VkResult result = api.AcquireNextImageKHR(device, swapchain_, UINT64_MAX,
                                            imageAvailable_[frameIndex_], VK_NULL_HANDLE,
                                            imageIndex);
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

void VkRenderer::RecordBlitLocked(VkCommandBuffer cmd, VkImage src, int srcWidth, int srcHeight,
                                 uint32_t imageIndex) {
  VkApi& api = GetVkApi();
  // Fit the source picture into the swapchain image preserving its aspect ratio,
  // centred (letterboxed). Equal extents take a plain copy; otherwise a scaled
  // blit. The former "never blit" rule came from a different (emulator)
  // implementation and does not apply here: on the real device the scaled blit
  // is correct (doc_agent/gfx-engine.md §1).
  const uint32_t srcW = static_cast<uint32_t>(srcWidth);
  const uint32_t srcH = static_cast<uint32_t>(srcHeight);
  const uint32_t dstW = extent_.width;
  const uint32_t dstH = extent_.height;
  const bool sameSize = srcW == dstW && srcH == dstH;
  int32_t offX = 0;
  int32_t offY = 0;
  int32_t fitW = static_cast<int32_t>(dstW);
  int32_t fitH = static_cast<int32_t>(dstH);
  if (!sameSize) {
    const double scaleX = static_cast<double>(dstW) / static_cast<double>(srcW);
    const double scaleY = static_cast<double>(dstH) / static_cast<double>(srcH);
    const double scale = scaleX < scaleY ? scaleX : scaleY;
    fitW = static_cast<int32_t>(static_cast<double>(srcW) * scale);
    fitH = static_cast<int32_t>(static_cast<double>(srcH) * scale);
    if (fitW < 1) fitW = 1;
    if (fitH < 1) fitH = 1;
    offX = (static_cast<int32_t>(dstW) - fitW) / 2;
    offY = (static_cast<int32_t>(dstH) - fitH) / 2;
  }

  const VkImage target = images_[imageIndex];
  VkImageMemoryBarrier toDst{};
  toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toDst.srcAccessMask = 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.image = target;
  toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toDst.subresourceRange.levelCount = 1;
  toDst.subresourceRange.layerCount = 1;
  api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

  const VkImageSubresourceLayers kColorLayers = [] {
    VkImageSubresourceLayers layers{};
    layers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    layers.layerCount = 1;
    return layers;
  }();

  if (sameSize) {
    VkImageCopy region{};
    region.srcSubresource = kColorLayers;
    region.dstSubresource = kColorLayers;
    region.extent.width = srcW;
    region.extent.height = srcH;
    region.extent.depth = 1;
    api.CmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, target,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  } else {
    // Clear first: the letterbox bars must not show stale swapchain content.
    VkClearColorValue black{};
    black.float32[3] = 1.0f;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    api.CmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

    VkImageBlit blit{};
    blit.srcSubresource = kColorLayers;
    blit.srcOffsets[1].x = static_cast<int32_t>(srcW);
    blit.srcOffsets[1].y = static_cast<int32_t>(srcH);
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = kColorLayers;
    blit.dstOffsets[0].x = offX;
    blit.dstOffsets[0].y = offY;
    blit.dstOffsets[1].x = offX + fitW;
    blit.dstOffsets[1].y = offY + fitH;
    blit.dstOffsets[1].z = 1;
    api.CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, target,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  }

  VkImageMemoryBarrier toPresent = toDst;
  toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toPresent.dstAccessMask = 0;
  toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toPresent);
}

bool VkRenderer::SubmitAndPresentLocked(VkCommandBuffer cmd, uint32_t imageIndex) {
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();

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
  // Periodic heartbeat: proves continuous acquire/present on a real-device run
  // without needing the dev panel or a screenshot.
  if (presentCount_ <= 3 || presentCount_ % 30 == 0) {
    HMRDP_LOGI("vulkan present #%{public}u image=%{public}u %{public}ux%{public}u",
               static_cast<unsigned>(presentCount_), imageIndex, extent_.width,
               extent_.height);
  }
  return true;
}

bool VkRenderer::PresentImage(VkImage image, VkFormat imageFormat, int width, int height) {
  if (image == VK_NULL_HANDLE || width <= 0 || height <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  VkApi& api = GetVkApi();

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureSwapchainLocked()) {
      return false;
    }
    // A copy cannot convert channel order, so the engine image must already use
    // the swapchain format (the caller creates the engine with format()).
    if (imageFormat != format_) {
      error_ = "present image format mismatch";
      HMRDP_LOGE("vulkan %{public}s: swapchain=%{public}s image=%{public}s", error_.c_str(),
                 FormatName(format_).c_str(), FormatName(imageFormat).c_str());
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
    const VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    api.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api.BeginCommandBuffer(cmd, &beginInfo);

    RecordBlitLocked(cmd, image, width, height, imageIndex);
    return SubmitAndPresentLocked(cmd, imageIndex);
  }

  error_ = "swapchain out of date after re-creation";
  return false;
}

bool VkRenderer::EnsureDesktopImageLocked(int width, int height) {
  const bool match = desktopImage_ != VK_NULL_HANDLE && desktopImageWidth_ == width &&
                     desktopImageHeight_ == height && desktopImageFormat_ == format_;
  if (match) {
    return true;
  }
  DestroyDesktopImageLocked();

  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }

  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format_;
  info.extent.width = static_cast<uint32_t>(width);
  info.extent.height = static_cast<uint32_t>(height);
  info.extent.depth = 1;
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
  // Device-local (the GPU only ever copies from it); host access goes through the
  // staging buffers, so the fast path is not needed here.
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

  desktopImageFormat_ = format_;
  desktopImageWidth_ = width;
  desktopImageHeight_ = height;
  desktopImageFullUpload_ = true;
  return true;
}

void VkRenderer::DestroyDesktopImageLocked() {
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();
  const VkDevice device = context.device();
  if (device != VK_NULL_HANDLE) {
    if (desktopImage_ != VK_NULL_HANDLE && api.DestroyImage != nullptr) {
      api.DestroyImage(device, desktopImage_, nullptr);
    }
    if (desktopImageMemory_ != VK_NULL_HANDLE && api.FreeMemory != nullptr) {
      api.FreeMemory(device, desktopImageMemory_, nullptr);
    }
  }
  desktopImage_ = VK_NULL_HANDLE;
  desktopImageMemory_ = VK_NULL_HANDLE;
  desktopImageFormat_ = VK_FORMAT_UNDEFINED;
  desktopImageWidth_ = 0;
  desktopImageHeight_ = 0;
  desktopImageFullUpload_ = true;
}

bool VkRenderer::EnsureStageLocked(size_t bytes) {
  const uint32_t slot = frameIndex_;
  if (stageBuffers_[slot] != VK_NULL_HANDLE && stageCapacities_[slot] >= bytes) {
    return true;
  }
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();
  const VkDevice device = context.device();
  if (device == VK_NULL_HANDLE) {
    error_ = "Vulkan device not available";
    return false;
  }
  // The mapped pointer is owned by this slot, so it is unmapped before the
  // buffer is replaced. The slot's fence was waited before calling this, so no
  // in-flight command buffer still references it.
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
  // Host-visible and coherent when the device offers it (UMA); a non-coherent
  // type still works - the range is flushed explicitly below.
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

void VkRenderer::DestroyStageLocked() {
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();
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

bool VkRenderer::PresentBgraFrame(const uint8_t* data, int srcStride, int desktopWidth,
                                  int desktopHeight, int x, int y, int width, int height) {
  if (data == nullptr || srcStride <= 0 || desktopWidth <= 0 || desktopHeight <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!EnsureSwapchainLocked()) {
      return false;
    }
    if (!EnsureDesktopImageLocked(desktopWidth, desktopHeight)) {
      return false;
    }
    // The image was just (re)created, so it must be painted completely.
    int rx = desktopImageFullUpload_ ? 0 : x;
    int ry = desktopImageFullUpload_ ? 0 : y;
    int rw = desktopImageFullUpload_ ? desktopWidth : width;
    int rh = desktopImageFullUpload_ ? desktopHeight : height;
    if (rw <= 0 || rh <= 0) {
      return false;
    }
    // Clip the dirty region to the desktop (the caller's rect comes from FreeRDP
    // and can exceed it, e.g. after a resize).
    if (rx < 0) {
      rw += rx;
      rx = 0;
    }
    if (ry < 0) {
      rh += ry;
      ry = 0;
    }
    if (rx + rw > desktopWidth) {
      rw = desktopWidth - rx;
    }
    if (ry + rh > desktopHeight) {
      rh = desktopHeight - ry;
    }
    if (rw <= 0 || rh <= 0) {
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

    const VkDevice device = context.device();
    // This slot's fence was just waited by AcquireFrameLocked and the slot has
    // not been re-submitted since, so its staging buffer is free to refill.
    const size_t rowBytes = static_cast<size_t>(rw) * 4u;
    if (!EnsureStageLocked(rowBytes * static_cast<size_t>(rh))) {
      return false;
    }
    // The swapchain may be RGBA8 while FreeRDP hands us BGRA; the channel order
    // cannot be changed by a copy/blit, so it is swapped while filling the
    // staging buffer (same rule as GfxVkDesktop's CPU boundaries).
    const bool swapRb =
        format_ == VK_FORMAT_R8G8B8A8_UNORM || format_ == VK_FORMAT_R8G8B8A8_SRGB;
    uint8_t* stage = static_cast<uint8_t*>(stageMapped_[frameIndex_]);
    for (int row = 0; row < rh; ++row) {
      const uint8_t* srcRow = data + static_cast<size_t>(ry + row) * static_cast<size_t>(srcStride) +
                              static_cast<size_t>(rx) * 4u;
      uint8_t* dstRow = stage + static_cast<size_t>(row) * rowBytes;
      if (swapRb) {
        for (int col = 0; col < rw; ++col) {
          dstRow[col * 4 + 0] = srcRow[col * 4 + 2];
          dstRow[col * 4 + 1] = srcRow[col * 4 + 1];
          dstRow[col * 4 + 2] = srcRow[col * 4 + 0];
          dstRow[col * 4 + 3] = srcRow[col * 4 + 3];
        }
      } else {
        std::memcpy(dstRow, srcRow, rowBytes);
      }
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = stageMemories_[frameIndex_];
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    // A no-op on coherent memory; required when the type is not coherent.
    if (api.FlushMappedMemoryRanges != nullptr) {
      api.FlushMappedMemoryRanges(device, 1, &range);
    }

    const VkCommandBuffer cmd = commandBuffers_[frameIndex_];
    api.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    api.BeginCommandBuffer(cmd, &beginInfo);

    // Host writes -> transfer read (the staging buffer is not read through the
    // host, so a plain memory barrier is enough).
    VkMemoryBarrier hostToTransfer{};
    hostToTransfer.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hostToTransfer.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                           &hostToTransfer, 0, nullptr, 0, nullptr);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    // The staging buffer holds only the dirty rows, tightly packed.
    region.bufferRowLength = static_cast<uint32_t>(rw);
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = rx;
    region.imageOffset.y = ry;
    region.imageExtent.width = static_cast<uint32_t>(rw);
    region.imageExtent.height = static_cast<uint32_t>(rh);
    region.imageExtent.depth = 1;
    api.CmdCopyBufferToImage(cmd, stageBuffers_[frameIndex_], desktopImage_,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &region);

    // The upload is read by the blit below, in the same command buffer.
    VkMemoryBarrier transferToTransfer{};
    transferToTransfer.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    transferToTransfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    transferToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           1, &transferToTransfer, 0, nullptr, 0, nullptr);

    RecordBlitLocked(cmd, desktopImage_, desktopWidth, desktopHeight, imageIndex);
    const bool presented = SubmitAndPresentLocked(cmd, imageIndex);
    if (presented) {
      desktopImageFullUpload_ = false;
    }
    return presented;
  }

  error_ = "swapchain out of date after re-creation";
  return false;
}

VkFormat VkRenderer::format() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return format_;
}

void VkRenderer::Reset() {
  const int64_t t0 = NowUs();
  std::lock_guard<std::mutex> lock(mutex_);
  DestroySwapchainLocked();
  DestroyDesktopImageLocked();
  DestroyFrameResourcesLocked();
  VkContext::Instance().DestroySurface(surface_);
  surface_ = VK_NULL_HANDLE;
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
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();

  if (surface_ == VK_NULL_HANDLE) {
    if (!context.CreateSurface(pendingWindow_, &surface_)) {
      error_ = context.lastError();
      return false;
    }
  }
  // A device is only usable for a surface it can present to, so it is created
  // lazily here (doc_agent/gfx-engine.md §1 keeps it process-wide from then on). Its
  // entry points are only resolved by this call, so they are checked after it.
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
  // FIFO is the only mode the spec guarantees; it is also the one that matches
  // the "present only when the picture changed" policy (doc_agent/gfx-engine.md §1).
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
  // PresentImage writes the swapchain image with transfer commands (copy when
  // the sizes match, scaled blit otherwise), so TRANSFER_DST is required in
  // addition to the render-pass path's colour attachment.
  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
    error_ = "swapchain images cannot be transfer destinations";
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
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

  // Everything below replaces the previous swapchain, so tear it down only after
  // the new one exists (oldSwapchain already handed its images back).
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
  std::snprintf(buf, sizeof(buf), "swapchain %ux%u %s images=%zu fifo=%d", extent_.width,
                extent_.height, FormatName(format_).c_str(), images_.size(),
                presentMode == VK_PRESENT_MODE_FIFO_KHR ? 1 : 0);
  info_ = buf;
  HMRDP_LOGI("vulkan %{public}s", buf);
  return true;
}

void VkRenderer::DestroySwapchainLocked() {
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();
  const VkDevice device = context.device();

  if (swapchain_ != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
    // Re-creation is rare, so a full idle wait is the simple correct answer to
    // "nothing may still reference these objects" (doc_agent/gfx-engine.md §3).
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
    if (renderPass_ != VK_NULL_HANDLE && api.DestroyRenderPass != nullptr) {
      api.DestroyRenderPass(device, renderPass_, nullptr);
      renderPass_ = VK_NULL_HANDLE;
    }
  }
  renderFinished_.clear();
  framebuffers_.clear();
  views_.clear();
  images_.clear();
  format_ = VK_FORMAT_UNDEFINED;
  extent_ = {0, 0};
}

void VkRenderer::DestroyFrameResourcesLocked() {
  VkApi& api = GetVkApi();
  VkContext& context = VkContext::Instance();
  const VkDevice device = context.device();
  DestroyStageLocked();
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
    // Frees the command buffers allocated from it as well.
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

bool VkRenderer::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return swapchain_ != VK_NULL_HANDLE;
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
