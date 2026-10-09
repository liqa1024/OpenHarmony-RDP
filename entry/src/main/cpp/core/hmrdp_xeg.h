#ifndef HMRDP_XEG_H
#define HMRDP_XEG_H

#include <string>

#include "hmrdp_vk_context.h"  // VK_NO_PROTOTYPES + the Vulkan handles

namespace hmrdp {

// libxengine.so is dlopen()ed rather than linked, like libvulkan: a device
// without it must fall back to plain presentation, not fail to load the app. The
// XEngine headers stay inside hmrdp_xeg.cpp, so a build without the HMS sysroot
// still works.
bool XegAvailable();

// Whether `device` advertises XEG_spatial_upscale. `reason` receives a stable
// token when it cannot: "no-xengine" or "no-extension".
bool XegSpatialUpscaleSupported(VkPhysicalDevice device, std::string* reason);

// Every method is inert when the library is missing or the configuration was
// rejected, so the presenter keeps a single code path and just checks valid().
class XegSpatialUpscale {
 public:
  XegSpatialUpscale() = default;
  ~XegSpatialUpscale();
  XegSpatialUpscale(const XegSpatialUpscale&) = delete;
  XegSpatialUpscale& operator=(const XegSpatialUpscale&) = delete;

  // `format` is the format of both images (they must match the views handed to
  // Record).
  bool Create(VkDevice device, VkFormat format, VkExtent2D input, VkExtent2D output,
              float sharpness);
  // Both views must have the extents and format the object was created with.
  void Record(VkCommandBuffer cmd, VkImageView input, VkImageView output) const;
  void Destroy();
  bool valid() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

}  // namespace hmrdp

#endif  // HMRDP_XEG_H
