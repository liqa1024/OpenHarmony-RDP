/*
 * HmRdp - XEngine (超分) bridge.
 *
 * The platform's GPU spatial upscale lives in libxengine.so, which is dlopen()ed
 * rather than linked - the same rule as libvulkan (see hmrdp_vk_context.h): a
 * device without it must fall back to plain presentation, not fail to load the
 * application. The XEngine headers stay inside hmrdp_xeg.cpp so the presentation
 * code never depends on them and a build without the HMS sysroot still works
 * (XegAvailable() is then simply false).
 *
 * Only the Vulkan spatial upscale is used, and only by the Vulkan presenter: the
 * GLES presenter is a compatibility fallback and deliberately has no 超分.
 */
#ifndef HMRDP_XEG_H
#define HMRDP_XEG_H

#include <string>

#include "hmrdp_vk_context.h"  // VK_NO_PROTOTYPES + the Vulkan handles

namespace hmrdp {

// Whether libxengine.so is loadable and exposes the spatial-upscale entry
// points. Independent of any device.
bool XegAvailable();

// Whether `device` advertises XEG_spatial_upscale, i.e. whether 超分 can be
// created on it. `reason` receives a stable token when it cannot:
// "no-xengine" (library unavailable) or "no-extension" (device without the
// feature), so the UI layer owns the wording.
bool XegSpatialUpscaleSupported(VkPhysicalDevice device, std::string* reason);

// One XEngine GPU spatial-upscale object (XEG_SpatialUpscale). Every method is
// inert when the library is missing or the configuration was rejected, so the
// presenter keeps a single code path and just checks valid().
class XegSpatialUpscale {
 public:
  XegSpatialUpscale() = default;
  ~XegSpatialUpscale();
  XegSpatialUpscale(const XegSpatialUpscale&) = delete;
  XegSpatialUpscale& operator=(const XegSpatialUpscale&) = delete;

  // Creates the object that upscales `input` into `output`. `format` is the
  // format of both images (they must match the views handed to Record).
  bool Create(VkDevice device, VkFormat format, VkExtent2D input, VkExtent2D output,
              float sharpness);
  // Records one upscale: `input` is sampled and `output` is rendered into. Both
  // views must have the extents and format the object was created with.
  void Record(VkCommandBuffer cmd, VkImageView input, VkImageView output) const;
  void Destroy();
  bool valid() const { return handle_ != nullptr; }

 private:
  void* handle_ = nullptr;
};

}  // namespace hmrdp

#endif  // HMRDP_XEG_H
