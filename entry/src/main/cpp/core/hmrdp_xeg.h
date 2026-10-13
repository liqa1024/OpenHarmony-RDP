#ifndef HMRDP_XEG_H
#define HMRDP_XEG_H

#include <string>

#include "hmrdp_vk_context.h"  // VK_NO_PROTOTYPES + the Vulkan handles

namespace hmrdp {

// libxengine.so is dlopen()ed (like libvulkan) so a device without it falls back to plain presentation
// instead of failing to load; the XEngine headers stay in hmrdp_xeg.cpp, so a build without the HMS sysroot works.
bool XegAvailable();

// Whether `device` advertises XEG_spatial_upscale; `reason` gets a stable token ("no-xengine" /
// "no-extension") when not.
bool XegSpatialUpscaleSupported(VkPhysicalDevice device, std::string* reason);

// Inert when the library is missing or the config was rejected, so the presenter checks only valid().
class XegSpatialUpscale {
 public:
  XegSpatialUpscale() = default;
  ~XegSpatialUpscale();
  XegSpatialUpscale(const XegSpatialUpscale&) = delete;
  XegSpatialUpscale& operator=(const XegSpatialUpscale&) = delete;

  // `format` applies to both images and must match the views handed to Record().
  bool Create(VkDevice device, VkFormat format, VkExtent2D input, VkExtent2D output,
              float sharpness);
  // Both views must have the extents and format the object was created with.
  void Record(VkCommandBuffer cmd, VkImageView input, VkImageView output) const;
  void Destroy();
  bool valid() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

}

#endif
