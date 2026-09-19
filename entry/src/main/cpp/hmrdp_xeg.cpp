/*
 * HmRdp - XEngine (超分) bridge, see hmrdp_xeg.h.
 */
#include "hmrdp_xeg.h"

#include <dlfcn.h>

#include <mutex>
#include <string>
#include <vector>

#include "hmrdp_log.h"

#if defined(HMRDP_HAVE_XENGINE)
// The prototypes in the XEngine headers would have to be linked; the entry
// points are resolved with dlsym instead.
#ifndef XEG_NO_PROTOTYPES
#define XEG_NO_PROTOTYPES 1
#endif
#include <xengine/xeg_extension_defs.h>
#include <xengine/xeg_vulkan_extension.h>
#include <xengine/xeg_vulkan_spatial_upscale.h>
#endif

namespace hmrdp {

namespace {

#if defined(HMRDP_HAVE_XENGINE)
constexpr const char* kXegLibrary = "libxengine.so";

struct XegApi {
  bool loaded = false;
  std::string loadError;
  PFN_HMS_XEG_EnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties = nullptr;
  PFN_HMS_XEG_CreateSpatialUpscale createSpatialUpscale = nullptr;
  PFN_HMS_XEG_CmdRenderSpatialUpscale cmdRenderSpatialUpscale = nullptr;
  PFN_HMS_XEG_DestroySpatialUpscale destroySpatialUpscale = nullptr;
};

XegApi g_api;
std::once_flag g_loadOnce;

template <typename T>
T ResolveXeg(void* library, const char* name) {
  return reinterpret_cast<T>(dlsym(library, name));
}

void LoadXeg() {
  void* library = dlopen(kXegLibrary, RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    const char* reason = dlerror();
    g_api.loadError = reason != nullptr ? reason : "dlopen failed";
    HMRDP_LOGW("xengine: %{public}s unavailable: %{public}s", kXegLibrary,
               g_api.loadError.c_str());
    return;
  }
  g_api.enumerateDeviceExtensionProperties =
      ResolveXeg<PFN_HMS_XEG_EnumerateDeviceExtensionProperties>(
          library, "HMS_XEG_EnumerateDeviceExtensionProperties");
  g_api.createSpatialUpscale =
      ResolveXeg<PFN_HMS_XEG_CreateSpatialUpscale>(library, "HMS_XEG_CreateSpatialUpscale");
  g_api.cmdRenderSpatialUpscale = ResolveXeg<PFN_HMS_XEG_CmdRenderSpatialUpscale>(
      library, "HMS_XEG_CmdRenderSpatialUpscale");
  g_api.destroySpatialUpscale = ResolveXeg<PFN_HMS_XEG_DestroySpatialUpscale>(
      library, "HMS_XEG_DestroySpatialUpscale");
  g_api.loaded = g_api.enumerateDeviceExtensionProperties != nullptr &&
                 g_api.createSpatialUpscale != nullptr &&
                 g_api.cmdRenderSpatialUpscale != nullptr &&
                 g_api.destroySpatialUpscale != nullptr;
  if (!g_api.loaded) {
    g_api.loadError = "entry points missing";
    HMRDP_LOGW("xengine: %{public}s loaded but incomplete", kXegLibrary);
    return;
  }
  HMRDP_LOGI("xengine: %{public}s loaded", kXegLibrary);
}
#endif  // HMRDP_HAVE_XENGINE

}  // namespace

bool XegAvailable() {
#if defined(HMRDP_HAVE_XENGINE)
  std::call_once(g_loadOnce, LoadXeg);
  return g_api.loaded;
#else
  return false;
#endif
}

bool XegSpatialUpscaleSupported(VkPhysicalDevice device, std::string* reason) {
  if (reason != nullptr) {
    reason->clear();
  }
#if defined(HMRDP_HAVE_XENGINE)
  if (device == VK_NULL_HANDLE) {
    if (reason != nullptr) {
      *reason = "no-xengine";
    }
    return false;
  }
  if (!XegAvailable()) {
    if (reason != nullptr) {
      *reason = "no-xengine";
    }
    return false;
  }
  uint32_t count = 0;
  if (g_api.enumerateDeviceExtensionProperties(device, &count, nullptr) != VK_SUCCESS ||
      count == 0) {
    if (reason != nullptr) {
      *reason = "no-extension";
    }
    return false;
  }
  std::vector<XEG_ExtensionProperties> props(count);
  if (g_api.enumerateDeviceExtensionProperties(device, &count, props.data()) != VK_SUCCESS &&
      count == 0) {
    if (reason != nullptr) {
      *reason = "no-extension";
    }
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (std::string(props[i].extensionName) == XEG_SPATIAL_UPSCALE_EXTENSION_NAME) {
      return true;
    }
  }
  if (reason != nullptr) {
    *reason = "no-extension";
  }
  return false;
#else
  (void)device;
  if (reason != nullptr) {
    *reason = "no-xengine";
  }
  return false;
#endif
}

XegSpatialUpscale::~XegSpatialUpscale() {
  Destroy();
}

bool XegSpatialUpscale::Create(VkDevice device, VkFormat format, VkExtent2D input,
                               VkExtent2D output, float sharpness) {
#if defined(HMRDP_HAVE_XENGINE)
  if (!XegAvailable() || handle_ != nullptr) {
    return false;
  }
  XEG_SpatialUpscaleCreateInfo info{};
  info.inputSize = input;
  info.inputRegion.offset = {0, 0};
  info.inputRegion.extent = input;
  info.outputSize = output;
  info.outputRegion.offset = {0, 0};
  info.outputRegion.extent = output;
  info.format = format;
  info.sharpness = sharpness;
  XEG_SpatialUpscale upscale = nullptr;
  const VkResult result = g_api.createSpatialUpscale(device, &info, &upscale);
  if (result != VK_SUCCESS || upscale == nullptr) {
    HMRDP_LOGE("xengine: create spatial upscale failed (%{public}d), %ux%u -> %ux%u",
               static_cast<int>(result), input.width, input.height, output.width,
               output.height);
    return false;
  }
  handle_ = reinterpret_cast<void*>(upscale);
  HMRDP_LOGI("xengine: spatial upscale %{public}ux%{public}u -> %{public}ux%{public}u",
             input.width, input.height, output.width, output.height);
  return true;
#else
  (void)device;
  (void)format;
  (void)input;
  (void)output;
  (void)sharpness;
  return false;
#endif
}

void XegSpatialUpscale::Record(VkCommandBuffer cmd, VkImageView input,
                               VkImageView output) const {
#if defined(HMRDP_HAVE_XENGINE)
  if (handle_ == nullptr) {
    return;
  }
  XEG_SpatialUpscaleDescription description{};
  description.inputImage = input;
  description.outputImage = output;
  g_api.cmdRenderSpatialUpscale(cmd, reinterpret_cast<XEG_SpatialUpscale>(handle_),
                                &description);
#else
  (void)cmd;
  (void)input;
  (void)output;
#endif
}

void XegSpatialUpscale::Destroy() {
#if defined(HMRDP_HAVE_XENGINE)
  if (handle_ != nullptr) {
    g_api.destroySpatialUpscale(reinterpret_cast<XEG_SpatialUpscale>(handle_));
    handle_ = nullptr;
  }
#endif
}

}  // namespace hmrdp
