#include "hmrdp_vk_context.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "hmrdp_log.h"
#include "hmrdp_xeg.h"

namespace hmrdp {
namespace {

constexpr const char* kLibVulkan = "libvulkan.so";

std::once_flag g_apiOnce;
VkApi g_api;

template <typename Fn>
Fn Resolve(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* name) {
  if (gipa == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<Fn>(gipa(instance, name));
}

// Device commands go through vkGetDeviceProcAddr first (cheaper dispatch), falling back to
// vkGetInstanceProcAddr for entries the driver only exposes at instance level.
template <typename Fn>
Fn ResolveDevice(PFN_vkGetDeviceProcAddr gdpa, PFN_vkGetInstanceProcAddr gipa,
                 VkDevice device, VkInstance instance, const char* name) {
  if (gdpa != nullptr) {
    const Fn fn = reinterpret_cast<Fn>(gdpa(device, name));
    if (fn != nullptr) {
      return fn;
    }
  }
  return Resolve<Fn>(gipa, instance, name);
}

VkApi LoadVulkan() {
  VkApi api;
  // Loaded once and intentionally never closed: the process keeps using it for
  // the whole lifetime.
  void* handle = dlopen(kLibVulkan, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* err = dlerror();
    api.loadError = err != nullptr ? std::string(err) : std::string("dlopen failed");
    return api;
  }
  auto gipa =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
  if (gipa == nullptr) {
    api.loadError = "vkGetInstanceProcAddr not found";
    return api;
  }
  api.GetInstanceProcAddr = gipa;
  api.GetDeviceProcAddr =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(handle, "vkGetDeviceProcAddr"));

  // Only the global commands are guaranteed to be reachable without an instance.
  api.EnumerateInstanceVersion = Resolve<PFN_vkEnumerateInstanceVersion>(
      gipa, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  api.EnumerateInstanceExtensionProperties =
      Resolve<PFN_vkEnumerateInstanceExtensionProperties>(
          gipa, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  api.EnumerateInstanceLayerProperties = Resolve<PFN_vkEnumerateInstanceLayerProperties>(
      gipa, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
  api.CreateInstance = Resolve<PFN_vkCreateInstance>(gipa, VK_NULL_HANDLE, "vkCreateInstance");
  if (api.CreateInstance == nullptr) {
    api.loadError = "vkCreateInstance not found";
    return api;
  }
  api.loaded = true;
  return api;
}

bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const VkExtensionProperties& ext : exts) {
    if (std::strcmp(ext.extensionName, name) == 0) {
      return true;
    }
  }
  return false;
}

std::string VersionString(uint32_t version) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_API_VERSION_MAJOR(version),
                VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
  return std::string(buf);
}

std::string DeviceTypeName(uint32_t type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return "INTEGRATED_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return "DISCRETE_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return "VIRTUAL_GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
      return "CPU";
    default:
      return "OTHER";
  }
}

// Only the colour spaces this pipeline can act on are named; anything else prints as its raw value,
// which is still enough to tell what a device advertised.
std::string ColorSpaceName(VkColorSpaceKHR colorSpace) {
  switch (colorSpace) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
      return "srgb";
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
      return "hdr10";
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
      return "hlg";
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT:
      return "p3";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
      return "ext-srgb-linear";
    default:
      break;
  }
  return std::to_string(static_cast<int>(colorSpace));
}

std::string SurfaceFormatName(VkFormat format) {
  switch (format) {
    case VK_FORMAT_UNDEFINED:
      return "any";
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

VulkanCapabilities ProbeVulkan() {
  VulkanCapabilities caps;
  VkApi& api = GetVkApi();
  caps.loaderPresent = api.loaded;
  caps.loadError = api.loadError;
  if (!api.loaded) {
    caps.probeError = "libvulkan.so unavailable";
    return caps;
  }

  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (api.EnumerateInstanceVersion != nullptr) {
    api.EnumerateInstanceVersion(&loaderVersion);
  }
  caps.loaderApiVersion = loaderVersion;

  // Instance extensions can be enumerated before the instance exists.
  if (api.EnumerateInstanceExtensionProperties != nullptr) {
    uint32_t count = 0;
    api.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    if (count > 0) {
      api.EnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    }
    caps.instanceExtensionCount = count;
    caps.extKhrSurface = HasExtension(exts, VK_KHR_SURFACE_EXTENSION_NAME);
    caps.extOhosSurface = HasExtension(exts, VK_OHOS_SURFACE_EXTENSION_NAME);
    caps.extSwapchainColorspace = HasExtension(exts, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    for (size_t i = 0; i < exts.size(); ++i) {
      if (i != 0) {
        caps.instanceExtensions += ", ";
      }
      caps.instanceExtensions += exts[i].extensionName;
    }
  }

  if (!VkInstanceContext::Instance().EnsureInstance()) {
    caps.probeError = VkInstanceContext::Instance().lastError();
    return caps;
  }
  caps.instanceOk = true;
  const VkInstance instance = VkInstanceContext::Instance().instance();

  uint32_t deviceCount = 0;
  if (api.EnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS ||
      deviceCount == 0) {
    caps.probeError = "no physical device (VkEnumeratePhysicalDevices returned 0)";
    return caps;
  }
  std::vector<VkPhysicalDevice> devices(deviceCount);
  if (api.EnumeratePhysicalDevices(instance, &deviceCount, devices.data()) != VK_SUCCESS) {
    caps.probeError = "vkEnumeratePhysicalDevices failed";
    return caps;
  }

  VkPhysicalDevice best = VK_NULL_HANDLE;
  uint32_t bestScore = 0;
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties props{};
    api.GetPhysicalDeviceProperties(device, &props);
    uint32_t score = 1;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score = 4;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      score = 3;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
      score = 2;
    }
    if (score > bestScore) {
      bestScore = score;
      best = device;
    }
  }
  if (best == VK_NULL_HANDLE) {
    caps.probeError = "no usable physical device";
    return caps;
  }

  VkPhysicalDeviceProperties props{};
  api.GetPhysicalDeviceProperties(best, &props);
  caps.deviceFound = true;
  caps.deviceApiVersion = props.apiVersion;
  caps.driverVersion = props.driverVersion;
  caps.vendorId = props.vendorID;
  caps.deviceId = props.deviceID;
  caps.deviceType = props.deviceType;
  std::snprintf(caps.deviceName, sizeof(caps.deviceName), "%s", props.deviceName);

  uint32_t extCount = 0;
  api.EnumerateDeviceExtensionProperties(best, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> devExts(extCount);
  if (extCount > 0) {
    api.EnumerateDeviceExtensionProperties(best, nullptr, &extCount, devExts.data());
  }
  caps.deviceExtensionCount = extCount;
  caps.extKhrSwapchain = HasExtension(devExts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  caps.extTimelineSemaphore = HasExtension(devExts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
  caps.extExternalMemory = HasExtension(devExts, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
  caps.extExternalMemoryFd = HasExtension(devExts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
  caps.extOhosExternalMemory = HasExtension(devExts, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME);

  // The importer addresses the buffer as an R8 linear image; verify the device accepts that shape
  // before advertising the video path (the external-memory import itself still has its own checks).
  if (api.GetPhysicalDeviceImageFormatProperties != nullptr) {
    VkImageFormatProperties formatProps{};
    caps.videoImportFormatOk = api.GetPhysicalDeviceImageFormatProperties(
                                   best, VK_FORMAT_R8_UNORM, VK_IMAGE_TYPE_2D,
                                   VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, 0,
                                   &formatProps) == VK_SUCCESS;
    // The 10-bit target an HDR10 swapchain writes into. A surface could still refuse the format
    // itself, which is why the surface side is observed separately (ObserveSurfaceHdrSupport).
    caps.hdr10AttachmentOk = api.GetPhysicalDeviceImageFormatProperties(
                                 best, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_TYPE_2D,
                                 VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
                                 &formatProps) == VK_SUCCESS;
  }

  // XEngine (超分) is queried through libxengine.so's own enumeration, not the
  // Vulkan loader's, so it is probed separately (see hmrdp_xeg.h).
  caps.xegLibrary = XegAvailable();
  if (caps.xegLibrary) {
    std::string reason;
    caps.xegSpatialUpscale = XegSpatialUpscaleSupported(best, &reason);
  }

  VkPhysicalDeviceMemoryProperties memProps{};
  api.GetPhysicalDeviceMemoryProperties(best, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = memProps.memoryTypes[i].propertyFlags;
    const bool hostVisible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool hostCoherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    const bool deviceLocal = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    caps.memHostVisible = caps.memHostVisible || hostVisible;
    caps.memHostCoherent = caps.memHostCoherent || hostCoherent;
    caps.memHostVisibleDeviceLocal =
        caps.memHostVisibleDeviceLocal || (hostVisible && hostCoherent && deviceLocal);
    if (!caps.memoryTypes.empty()) {
      caps.memoryTypes += "; ";
    }
    std::string type;
    if (deviceLocal) {
      type += "DEVICE_LOCAL";
    }
    if (hostVisible) {
      type += type.empty() ? "" : "|";
      type += "HOST_VISIBLE";
    }
    if (hostCoherent) {
      type += type.empty() ? "" : "|";
      type += "HOST_COHERENT";
    }
    if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
      type += type.empty() ? "" : "|";
      type += "HOST_CACHED";
    }
    caps.memoryTypes += type.empty() ? "NONE" : type;
  }

  uint32_t familyCount = 0;
  api.GetPhysicalDeviceQueueFamilyProperties(best, &familyCount, nullptr);
  caps.queueFamilyCount = familyCount;
  std::vector<VkQueueFamilyProperties> families(familyCount);
  if (familyCount > 0) {
    api.GetPhysicalDeviceQueueFamilyProperties(best, &familyCount, families.data());
  }
  for (uint32_t i = 0; i < familyCount; ++i) {
    const VkQueueFlags flags = families[i].queueFlags;
    if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      caps.graphicsQueueFamilies++;
    }
    if ((flags & VK_QUEUE_TRANSFER_BIT) != 0) {
      caps.transferQueueFamilies++;
    }
  }

  char summary[256];
  std::snprintf(summary, sizeof(summary),
                "swapchain=%d timeline=%d extMem=%d extMemFd=%d ohosExtMem=%d xeg=%d "
                "colorspaceExt=%d hdr10Attach=%d (of %u)",
                caps.extKhrSwapchain ? 1 : 0, caps.extTimelineSemaphore ? 1 : 0,
                caps.extExternalMemory ? 1 : 0, caps.extExternalMemoryFd ? 1 : 0,
                caps.extOhosExternalMemory ? 1 : 0, caps.xegSpatialUpscale ? 1 : 0,
                caps.extSwapchainColorspace ? 1 : 0, caps.hdr10AttachmentOk ? 1 : 0,
                caps.deviceExtensionCount);
  caps.deviceExtensions = summary;
  return caps;
}

}  // namespace

bool VkApi::LoadInstance(VkInstance instance) {
  if (GetInstanceProcAddr == nullptr || instance == VK_NULL_HANDLE) {
    return false;
  }
  PFN_vkGetInstanceProcAddr gipa = GetInstanceProcAddr;
  DestroyInstance = Resolve<PFN_vkDestroyInstance>(gipa, instance, "vkDestroyInstance");
  EnumeratePhysicalDevices =
      Resolve<PFN_vkEnumeratePhysicalDevices>(gipa, instance, "vkEnumeratePhysicalDevices");
  GetPhysicalDeviceProperties = Resolve<PFN_vkGetPhysicalDeviceProperties>(
      gipa, instance, "vkGetPhysicalDeviceProperties");
  GetPhysicalDeviceImageFormatProperties =
      Resolve<PFN_vkGetPhysicalDeviceImageFormatProperties>(
          gipa, instance, "vkGetPhysicalDeviceImageFormatProperties");
  GetPhysicalDeviceQueueFamilyProperties = Resolve<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      gipa, instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  GetPhysicalDeviceMemoryProperties = Resolve<PFN_vkGetPhysicalDeviceMemoryProperties>(
      gipa, instance, "vkGetPhysicalDeviceMemoryProperties");
  EnumerateDeviceExtensionProperties = Resolve<PFN_vkEnumerateDeviceExtensionProperties>(
      gipa, instance, "vkEnumerateDeviceExtensionProperties");
  GetPhysicalDeviceSurfaceSupportKHR = Resolve<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
      gipa, instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
  GetPhysicalDeviceSurfaceCapabilitiesKHR =
      Resolve<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
          gipa, instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  GetPhysicalDeviceSurfaceFormatsKHR = Resolve<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
      gipa, instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  GetPhysicalDeviceSurfacePresentModesKHR =
      Resolve<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
          gipa, instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
  CreateDevice = Resolve<PFN_vkCreateDevice>(gipa, instance, "vkCreateDevice");
  DestroySurfaceKHR = Resolve<PFN_vkDestroySurfaceKHR>(gipa, instance, "vkDestroySurfaceKHR");
  CreateSurfaceOHOS = Resolve<PFN_vkCreateSurfaceOHOS>(gipa, instance, "vkCreateSurfaceOHOS");
  return CreateDevice != nullptr && EnumeratePhysicalDevices != nullptr &&
         GetPhysicalDeviceProperties != nullptr && GetPhysicalDeviceQueueFamilyProperties != nullptr &&
         GetPhysicalDeviceMemoryProperties != nullptr &&
         EnumerateDeviceExtensionProperties != nullptr && DestroyInstance != nullptr;
}

bool VkApi::LoadDevice(VkDevice device, VkInstance instance) {
  if (device == VK_NULL_HANDLE) {
    return false;
  }
  PFN_vkGetDeviceProcAddr gdpa = GetDeviceProcAddr;
  PFN_vkGetInstanceProcAddr gipa = GetInstanceProcAddr;
  DestroyDevice = ResolveDevice<PFN_vkDestroyDevice>(gdpa, gipa, device, instance, "vkDestroyDevice");
  DeviceWaitIdle = ResolveDevice<PFN_vkDeviceWaitIdle>(gdpa, gipa, device, instance, "vkDeviceWaitIdle");
  GetDeviceQueue = ResolveDevice<PFN_vkGetDeviceQueue>(gdpa, gipa, device, instance, "vkGetDeviceQueue");
  QueueSubmit = ResolveDevice<PFN_vkQueueSubmit>(gdpa, gipa, device, instance, "vkQueueSubmit");
  QueueWaitIdle = ResolveDevice<PFN_vkQueueWaitIdle>(gdpa, gipa, device, instance, "vkQueueWaitIdle");
  CreateSwapchainKHR = ResolveDevice<PFN_vkCreateSwapchainKHR>(gdpa, gipa, device, instance, "vkCreateSwapchainKHR");
  DestroySwapchainKHR = ResolveDevice<PFN_vkDestroySwapchainKHR>(gdpa, gipa, device, instance, "vkDestroySwapchainKHR");
  GetSwapchainImagesKHR = ResolveDevice<PFN_vkGetSwapchainImagesKHR>(gdpa, gipa, device, instance, "vkGetSwapchainImagesKHR");
  AcquireNextImageKHR = ResolveDevice<PFN_vkAcquireNextImageKHR>(gdpa, gipa, device, instance, "vkAcquireNextImageKHR");
  QueuePresentKHR = ResolveDevice<PFN_vkQueuePresentKHR>(gdpa, gipa, device, instance, "vkQueuePresentKHR");
  CreateSemaphore = ResolveDevice<PFN_vkCreateSemaphore>(gdpa, gipa, device, instance, "vkCreateSemaphore");
  DestroySemaphore = ResolveDevice<PFN_vkDestroySemaphore>(gdpa, gipa, device, instance, "vkDestroySemaphore");
  CreateFence = ResolveDevice<PFN_vkCreateFence>(gdpa, gipa, device, instance, "vkCreateFence");
  DestroyFence = ResolveDevice<PFN_vkDestroyFence>(gdpa, gipa, device, instance, "vkDestroyFence");
  WaitForFences = ResolveDevice<PFN_vkWaitForFences>(gdpa, gipa, device, instance, "vkWaitForFences");
  ResetFences = ResolveDevice<PFN_vkResetFences>(gdpa, gipa, device, instance, "vkResetFences");
  CreateRenderPass = ResolveDevice<PFN_vkCreateRenderPass>(gdpa, gipa, device, instance, "vkCreateRenderPass");
  DestroyRenderPass = ResolveDevice<PFN_vkDestroyRenderPass>(gdpa, gipa, device, instance, "vkDestroyRenderPass");
  CreateFramebuffer = ResolveDevice<PFN_vkCreateFramebuffer>(gdpa, gipa, device, instance, "vkCreateFramebuffer");
  DestroyFramebuffer = ResolveDevice<PFN_vkDestroyFramebuffer>(gdpa, gipa, device, instance, "vkDestroyFramebuffer");
  CreateImageView = ResolveDevice<PFN_vkCreateImageView>(gdpa, gipa, device, instance, "vkCreateImageView");
  DestroyImageView = ResolveDevice<PFN_vkDestroyImageView>(gdpa, gipa, device, instance, "vkDestroyImageView");
  CreateCommandPool = ResolveDevice<PFN_vkCreateCommandPool>(gdpa, gipa, device, instance, "vkCreateCommandPool");
  DestroyCommandPool = ResolveDevice<PFN_vkDestroyCommandPool>(gdpa, gipa, device, instance, "vkDestroyCommandPool");
  AllocateCommandBuffers = ResolveDevice<PFN_vkAllocateCommandBuffers>(gdpa, gipa, device, instance, "vkAllocateCommandBuffers");
  FreeCommandBuffers = ResolveDevice<PFN_vkFreeCommandBuffers>(gdpa, gipa, device, instance, "vkFreeCommandBuffers");
  ResetCommandBuffer = ResolveDevice<PFN_vkResetCommandBuffer>(gdpa, gipa, device, instance, "vkResetCommandBuffer");
  BeginCommandBuffer = ResolveDevice<PFN_vkBeginCommandBuffer>(gdpa, gipa, device, instance, "vkBeginCommandBuffer");
  EndCommandBuffer = ResolveDevice<PFN_vkEndCommandBuffer>(gdpa, gipa, device, instance, "vkEndCommandBuffer");
  CmdBeginRenderPass = ResolveDevice<PFN_vkCmdBeginRenderPass>(gdpa, gipa, device, instance, "vkCmdBeginRenderPass");
  CmdEndRenderPass = ResolveDevice<PFN_vkCmdEndRenderPass>(gdpa, gipa, device, instance, "vkCmdEndRenderPass");
  CmdPipelineBarrier = ResolveDevice<PFN_vkCmdPipelineBarrier>(gdpa, gipa, device, instance, "vkCmdPipelineBarrier");
  CmdCopyBufferToImage = ResolveDevice<PFN_vkCmdCopyBufferToImage>(gdpa, gipa, device, instance, "vkCmdCopyBufferToImage");
  CmdCopyImageToBuffer = ResolveDevice<PFN_vkCmdCopyImageToBuffer>(gdpa, gipa, device, instance, "vkCmdCopyImageToBuffer");
  CreateImage = ResolveDevice<PFN_vkCreateImage>(gdpa, gipa, device, instance, "vkCreateImage");
  DestroyImage = ResolveDevice<PFN_vkDestroyImage>(gdpa, gipa, device, instance, "vkDestroyImage");
  GetImageMemoryRequirements = ResolveDevice<PFN_vkGetImageMemoryRequirements>(gdpa, gipa, device, instance, "vkGetImageMemoryRequirements");
  CreateBuffer = ResolveDevice<PFN_vkCreateBuffer>(gdpa, gipa, device, instance, "vkCreateBuffer");
  DestroyBuffer = ResolveDevice<PFN_vkDestroyBuffer>(gdpa, gipa, device, instance, "vkDestroyBuffer");
  GetBufferMemoryRequirements = ResolveDevice<PFN_vkGetBufferMemoryRequirements>(gdpa, gipa, device, instance, "vkGetBufferMemoryRequirements");
  AllocateMemory = ResolveDevice<PFN_vkAllocateMemory>(gdpa, gipa, device, instance, "vkAllocateMemory");
  FreeMemory = ResolveDevice<PFN_vkFreeMemory>(gdpa, gipa, device, instance, "vkFreeMemory");
  BindImageMemory = ResolveDevice<PFN_vkBindImageMemory>(gdpa, gipa, device, instance, "vkBindImageMemory");
  BindBufferMemory = ResolveDevice<PFN_vkBindBufferMemory>(gdpa, gipa, device, instance, "vkBindBufferMemory");
  MapMemory = ResolveDevice<PFN_vkMapMemory>(gdpa, gipa, device, instance, "vkMapMemory");
  UnmapMemory = ResolveDevice<PFN_vkUnmapMemory>(gdpa, gipa, device, instance, "vkUnmapMemory");
  FlushMappedMemoryRanges = ResolveDevice<PFN_vkFlushMappedMemoryRanges>(gdpa, gipa, device, instance, "vkFlushMappedMemoryRanges");  InvalidateMappedMemoryRanges = ResolveDevice<PFN_vkInvalidateMappedMemoryRanges>(gdpa, gipa, device, instance, "vkInvalidateMappedMemoryRanges");
  CreateQueryPool = ResolveDevice<PFN_vkCreateQueryPool>(gdpa, gipa, device, instance, "vkCreateQueryPool");
  DestroyQueryPool = ResolveDevice<PFN_vkDestroyQueryPool>(gdpa, gipa, device, instance, "vkDestroyQueryPool");
  CmdResetQueryPool = ResolveDevice<PFN_vkCmdResetQueryPool>(gdpa, gipa, device, instance, "vkCmdResetQueryPool");
  CmdWriteTimestamp = ResolveDevice<PFN_vkCmdWriteTimestamp>(gdpa, gipa, device, instance, "vkCmdWriteTimestamp");
  GetQueryPoolResults = ResolveDevice<PFN_vkGetQueryPoolResults>(gdpa, gipa, device, instance, "vkGetQueryPoolResults");
  CreateShaderModule = ResolveDevice<PFN_vkCreateShaderModule>(gdpa, gipa, device, instance, "vkCreateShaderModule");
  DestroyShaderModule = ResolveDevice<PFN_vkDestroyShaderModule>(gdpa, gipa, device, instance, "vkDestroyShaderModule");
  CreateDescriptorSetLayout = ResolveDevice<PFN_vkCreateDescriptorSetLayout>(gdpa, gipa, device, instance, "vkCreateDescriptorSetLayout");
  DestroyDescriptorSetLayout = ResolveDevice<PFN_vkDestroyDescriptorSetLayout>(gdpa, gipa, device, instance, "vkDestroyDescriptorSetLayout");
  CreatePipelineLayout = ResolveDevice<PFN_vkCreatePipelineLayout>(gdpa, gipa, device, instance, "vkCreatePipelineLayout");
  DestroyPipelineLayout = ResolveDevice<PFN_vkDestroyPipelineLayout>(gdpa, gipa, device, instance, "vkDestroyPipelineLayout");
  DestroyPipeline = ResolveDevice<PFN_vkDestroyPipeline>(gdpa, gipa, device, instance, "vkDestroyPipeline");
  CreateDescriptorPool = ResolveDevice<PFN_vkCreateDescriptorPool>(gdpa, gipa, device, instance, "vkCreateDescriptorPool");
  DestroyDescriptorPool = ResolveDevice<PFN_vkDestroyDescriptorPool>(gdpa, gipa, device, instance, "vkDestroyDescriptorPool");
  ResetDescriptorPool = ResolveDevice<PFN_vkResetDescriptorPool>(gdpa, gipa, device, instance, "vkResetDescriptorPool");
  AllocateDescriptorSets = ResolveDevice<PFN_vkAllocateDescriptorSets>(gdpa, gipa, device, instance, "vkAllocateDescriptorSets");
  UpdateDescriptorSets = ResolveDevice<PFN_vkUpdateDescriptorSets>(gdpa, gipa, device, instance, "vkUpdateDescriptorSets");
  CmdBindPipeline = ResolveDevice<PFN_vkCmdBindPipeline>(gdpa, gipa, device, instance, "vkCmdBindPipeline");
  CmdBindDescriptorSets = ResolveDevice<PFN_vkCmdBindDescriptorSets>(gdpa, gipa, device, instance, "vkCmdBindDescriptorSets");

  CreateGraphicsPipelines = ResolveDevice<PFN_vkCreateGraphicsPipelines>(gdpa, gipa, device, instance, "vkCreateGraphicsPipelines");
  CreateSampler = ResolveDevice<PFN_vkCreateSampler>(gdpa, gipa, device, instance, "vkCreateSampler");
  DestroySampler = ResolveDevice<PFN_vkDestroySampler>(gdpa, gipa, device, instance, "vkDestroySampler");
  CmdSetViewport = ResolveDevice<PFN_vkCmdSetViewport>(gdpa, gipa, device, instance, "vkCmdSetViewport");
  CmdSetScissor = ResolveDevice<PFN_vkCmdSetScissor>(gdpa, gipa, device, instance, "vkCmdSetScissor");
  CmdDraw = ResolveDevice<PFN_vkCmdDraw>(gdpa, gipa, device, instance, "vkCmdDraw");

  // Optional (the device may not advertise them): a null here just means no video import / 超分
  // through the presenter, never a load failure.
  GetNativeBufferPropertiesOHOS = ResolveDevice<PFN_vkGetNativeBufferPropertiesOHOS>(
      gdpa, gipa, device, instance, "vkGetNativeBufferPropertiesOHOS");

  return CreateSwapchainKHR != nullptr && AcquireNextImageKHR != nullptr &&
         QueuePresentKHR != nullptr && GetSwapchainImagesKHR != nullptr &&
         QueueSubmit != nullptr && CreateCommandPool != nullptr;
}

VkApi& GetVkApi() {
  std::call_once(g_apiOnce, []() { g_api = LoadVulkan(); });
  return g_api;
}

std::string VkResultName(int32_t result) {
  switch (result) {
    case VK_SUCCESS:
      return "VK_SUCCESS";
    case VK_NOT_READY:
      return "VK_NOT_READY";
    case VK_TIMEOUT:
      return "VK_TIMEOUT";
    case VK_EVENT_SET:
      return "VK_EVENT_SET";
    case VK_EVENT_RESET:
      return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
      return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
      return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
      return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
      return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
      return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR:
      return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
      return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case VK_SUBOPTIMAL_KHR:
      return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
      return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR:
      return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
    default:
      break;
  }
  char buf[48];
  std::snprintf(buf, sizeof(buf), "VkResult(%d)", result);
  return std::string(buf);
}

namespace {

void FillPresenterVerdict(VulkanCapabilities* caps) {
  if (caps == nullptr) {
    return;
  }
  caps->presenterSupported = false;
  caps->presenterUnsupportedCode.clear();

  // Vulkan acceleration is real-device-only: the emulator reports standard capabilities it
  // does not honour, so the x86_64 emulator ABI (not probing) is the reliable exclusion.
#if defined(__x86_64__)
  caps->presenterUnsupportedCode = "emulator";
#else
  if (!caps->loaderPresent) {
    caps->presenterUnsupportedCode = "no-vulkan";
  } else if (!caps->instanceOk) {
    caps->presenterUnsupportedCode = "no-instance";
  } else if (!caps->deviceFound) {
    caps->presenterUnsupportedCode = "no-device";
  } else if (!caps->memHostVisible) {
    caps->presenterUnsupportedCode = "no-host-memory";
  } else if (!caps->extOhosSurface || !caps->extKhrSwapchain) {
    caps->presenterUnsupportedCode = "no-surface";
  } else {
    caps->presenterSupported = true;
  }
#endif
  HMRDP_LOGI("vulkan verdict: presenter=%{public}d(%{public}s)",
             caps->presenterSupported ? 1 : 0,
             caps->presenterUnsupportedCode.empty() ? "-" : caps->presenterUnsupportedCode.c_str());

  caps->srFsrSupported = false;
  caps->srXengineSupported = false;
  caps->srXengineUnsupportedCode.clear();
  caps->srSupported = false;
  caps->srUnsupportedCode.clear();
  if (!caps->presenterSupported) {
    const std::string code = caps->presenterUnsupportedCode.empty()
                                 ? "no-surface"
                                 : caps->presenterUnsupportedCode;
    caps->srUnsupportedCode = code;
    caps->srXengineUnsupportedCode = code;
  } else {
#if defined(HMRDP_HAVE_FSR)
    caps->srFsrSupported = true;
#endif
    if (!caps->xegLibrary) {
      caps->srXengineUnsupportedCode = "no-xengine";
    } else if (!caps->xegSpatialUpscale) {
      caps->srXengineUnsupportedCode = "no-extension";
    } else {
      caps->srXengineSupported = true;
    }
    if (caps->srFsrSupported || caps->srXengineSupported) {
      caps->srSupported = true;
    } else {
      caps->srUnsupportedCode = caps->srXengineUnsupportedCode.empty()
                                    ? "no-extension"
                                    : caps->srXengineUnsupportedCode;
    }
  }
  HMRDP_LOGI("vulkan verdict: sr=%{public}d(fsr=%{public}d,xengine=%{public}d,%{public}s)",
             caps->srSupported ? 1 : 0, caps->srFsrSupported ? 1 : 0,
             caps->srXengineSupported ? 1 : 0,
             caps->srUnsupportedCode.empty() ? "-" : caps->srUnsupportedCode.c_str());

  // Not a UI gate for RDP (its 超分 input is the CPU desktop picture); it decides whether the
  // Moonlight 超分 path, which imports the decoder's NV12 buffer as a byte image and converts it in
  // the shader, can run on this device. Only the OHOS external-memory import is needed for that.
  caps->srVideoImportSupported = caps->presenterSupported && caps->extOhosExternalMemory &&
                                 caps->videoImportFormatOk;
  HMRDP_LOGI("vulkan verdict: video import=%{public}d (extMem=%{public}d, r8Linear=%{public}d)",
             caps->srVideoImportSupported ? 1 : 0, caps->extOhosExternalMemory ? 1 : 0,
             caps->videoImportFormatOk ? 1 : 0);

  // HDR10 presentation, device half. The surface half (a format and a colour space the window
  // actually offers) is observed when a swapchain is created and logged from there; both halves
  // together are what an HDR stream's output stage needs.
  caps->hdrPresentSupported = false;
  caps->hdrPresentUnsupportedCode.clear();
  if (!caps->presenterSupported) {
    caps->hdrPresentUnsupportedCode = caps->presenterUnsupportedCode.empty()
                                          ? "no-surface"
                                          : caps->presenterUnsupportedCode;
  } else if (!caps->extSwapchainColorspace) {
    caps->hdrPresentUnsupportedCode = "no-colorspace-ext";
  } else if (!caps->hdr10AttachmentOk) {
    caps->hdrPresentUnsupportedCode = "no-10bit-attachment";
  } else if (!caps->srVideoImportSupported) {
    caps->hdrPresentUnsupportedCode = "no-video-import";
  } else {
    caps->hdrPresentSupported = true;
  }
  HMRDP_LOGI("vulkan verdict: hdr static: colorspaceExt=%{public}d hdr10Attachment=%{public}d",
             caps->extSwapchainColorspace ? 1 : 0, caps->hdr10AttachmentOk ? 1 : 0);
  HMRDP_LOGI("vulkan verdict: hdr presentation=%{public}d(%{public}s)",
             caps->hdrPresentSupported ? 1 : 0,
             caps->hdrPresentUnsupportedCode.empty() ? "-" : caps->hdrPresentUnsupportedCode.c_str());
}

}  // namespace

SurfaceHdrSupport ObserveSurfaceHdrSupport(VkApi& api, VkPhysicalDevice device,
                                          VkSurfaceKHR surface) {
  SurfaceHdrSupport support;
  if (api.GetPhysicalDeviceSurfaceFormatsKHR == nullptr || device == VK_NULL_HANDLE ||
      surface == VK_NULL_HANDLE) {
    return support;
  }
  uint32_t count = 0;
  if (api.GetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr) != VK_SUCCESS ||
      count == 0) {
    return support;
  }
  std::vector<VkSurfaceFormatKHR> formats(count);
  if (api.GetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data()) !=
      VK_SUCCESS) {
    return support;
  }
  support.formatCount = count;
  for (const VkSurfaceFormatKHR& format : formats) {
    if (!support.formats.empty()) {
      support.formats += ", ";
    }
    support.formats += SurfaceFormatName(format.format) + "/" + ColorSpaceName(format.colorSpace);
    // A lone VK_FORMAT_UNDEFINED entry means "any format": the surface lets the application choose,
    // so both halves are available by construction.
    const bool anyFormat = format.format == VK_FORMAT_UNDEFINED;
    if (anyFormat || format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
      if (anyFormat || format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        support.hdr10FormatOk = true;
        support.hdr10ColorSpaceOk = true;
      }
    }
  }
  return support;
}

std::string VulkanCapabilities::Describe() const {
  if (!loaderPresent) {
    return "vulkan: unavailable (libvulkan.so: " + loadError + ")";
  }
  if (!instanceOk) {
    return "vulkan: loader " + VersionString(loaderApiVersion) + ", instance failed: " + probeError;
  }
  if (!deviceFound) {
    return "vulkan: loader " + VersionString(loaderApiVersion) + ", no device: " + probeError;
  }
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      "vulkan: loader=%s device=%s api=%s driver=0x%08x type=%s "
      "ohosSurface=%d swapchain=%d timeline=%d extMem=%d extMemFd=%d ohosExtMem=%d "
      "hostVisDevLocal=%d qf=%u",
      VersionString(loaderApiVersion).c_str(), deviceName, VersionString(deviceApiVersion).c_str(),
      driverVersion, DeviceTypeName(deviceType).c_str(), extOhosSurface ? 1 : 0,
      extKhrSwapchain ? 1 : 0, extTimelineSemaphore ? 1 : 0, extExternalMemory ? 1 : 0,
      extExternalMemoryFd ? 1 : 0, extOhosExternalMemory ? 1 : 0,
      memHostVisibleDeviceLocal ? 1 : 0, queueFamilyCount);
  return std::string(buf);
}

std::string VulkanCapabilities::DescribeLines() const {
  if (!usable()) {
    return "Vulkan: " + Describe();
  }
  std::string out = "Vulkan loader " + VersionString(loaderApiVersion) + "\n";
  out += "  device " + std::string(deviceName) + "\n";
  out += "  api " + VersionString(deviceApiVersion) + " driver 0x" +
         std::to_string(driverVersion) + " " + DeviceTypeName(deviceType) + "\n";
  out += "  ext OHOS_surface=" + std::string(extOhosSurface ? "1" : "0") +
         " swapchain=" + std::string(extKhrSwapchain ? "1" : "0") +
         " timeline=" + std::string(extTimelineSemaphore ? "1" : "0") +
         " extMemFd=" + std::string(extExternalMemoryFd ? "1" : "0") +
         " OHOS_extMem=" + std::string(extOhosExternalMemory ? "1" : "0") +
         " xeg=" + std::string(xegSpatialUpscale ? "1" : "0") +
         " colorspaceExt=" + std::string(extSwapchainColorspace ? "1" : "0") +
         " hdr10Attach=" + std::string(hdr10AttachmentOk ? "1" : "0") + "\n";
  out += "  mem hostVisible=" + std::string(memHostVisible ? "1" : "0") +
         " coherent=" + std::string(memHostCoherent ? "1" : "0") +
         " hostVisDevLocal=" + std::string(memHostVisibleDeviceLocal ? "1" : "0") + "\n";
  out += "  mem types: " + memoryTypes + "\n";
  out += "  queue families=" + std::to_string(queueFamilyCount) +
         " graphics=" + std::to_string(graphicsQueueFamilies) +
         " transfer=" + std::to_string(transferQueueFamilies) + "\n";
  out += "  device ext: " + deviceExtensions;
  return out;
}

const VulkanCapabilities& GetVulkanCapabilities() {
  static std::once_flag once;
  static VulkanCapabilities caps;
  std::call_once(once, []() {
    caps = ProbeVulkan();
    FillPresenterVerdict(&caps);
  });
  return caps;
}

VkInstanceContext& VkInstanceContext::Instance() {
  static VkInstanceContext context;
  return context;
}

VkInstanceContext::~VkInstanceContext() {
  VkApi& api = GetVkApi();
  if (instance_ != VK_NULL_HANDLE) {
    if (api.DestroyInstance != nullptr) {
      api.DestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
  }
}

bool VkInstanceContext::EnsureInstance() {
  if (instance_ != VK_NULL_HANDLE) {
    return true;
  }
  VkApi& api = GetVkApi();
  if (!api.loaded) {
    error_ = "libvulkan.so unavailable: " + api.loadError;
    return false;
  }

  const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "HmRdp";
  appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
  appInfo.pEngineName = "HmRdp";
  appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_0;

  std::vector<const char*> enabledExtensions(extensions, extensions + 2);
  // A surface only reports the colour spaces of the instance extensions that are enabled, and
  // VK_COLOR_SPACE_HDR10_ST2084_EXT belongs to VK_EXT_swapchain_colorspace. Enabling it changes
  // nothing for an SDR swapchain (the format is picked the same way), but without it an HDR10
  // surface would look unsupported and the HDR readout would be a false negative.
  {
    uint32_t count = 0;
    if (api.EnumerateInstanceExtensionProperties != nullptr) {
      api.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
      std::vector<VkExtensionProperties> exts(count);
      if (count > 0) {
        api.EnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
        if (HasExtension(exts, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
          enabledExtensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }
      }
    }
  }

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
  createInfo.ppEnabledExtensionNames = enabledExtensions.data();

  VkInstance instance = VK_NULL_HANDLE;
  const VkResult result = api.CreateInstance(&createInfo, nullptr, &instance);
  if (result != VK_SUCCESS) {
    // VK_OHOS_surface is the whole point: without it there is no way to present.
    error_ = "vkCreateInstance failed: " + VkResultName(result);
    return false;
  }
  instance_ = instance;
  if (!api.LoadInstance(instance_)) {
    error_ = "instance entry points missing";
    if (api.DestroyInstance != nullptr) {
      api.DestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
    return false;
  }
  HMRDP_LOGI("vulkan instance ready");
  return true;
}

bool VkInstanceContext::CreateSurface(void* nativeWindow, VkSurfaceKHR* out) {
  if (nativeWindow == nullptr || out == nullptr) {
    error_ = "null native window";
    return false;
  }
  if (!EnsureInstance()) {
    return false;
  }
  VkApi& api = GetVkApi();
  if (api.CreateSurfaceOHOS == nullptr) {
    error_ = "vkCreateSurfaceOHOS missing";
    return false;
  }
  VkSurfaceCreateInfoOHOS createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
  createInfo.pNext = nullptr;
  createInfo.flags = 0;
  createInfo.window = static_cast<OHNativeWindow*>(nativeWindow);

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const VkResult result = api.CreateSurfaceOHOS(instance_, &createInfo, nullptr, &surface);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateSurfaceOHOS failed: " + VkResultName(result);
    return false;
  }
  *out = surface;
  return true;
}

void VkInstanceContext::DestroySurface(VkSurfaceKHR surface) {
  if (surface == VK_NULL_HANDLE) {
    return;
  }
  VkApi& api = GetVkApi();
  if (api.DestroySurfaceKHR != nullptr && instance_ != VK_NULL_HANDLE) {
    api.DestroySurfaceKHR(instance_, surface, nullptr);
  }
}

VkDeviceContext::~VkDeviceContext() {
  Destroy();
}

bool VkDeviceContext::PickPhysicalDevice(VkSurfaceKHR surface) {
  VkApi& api = GetVkApi();
  const VkInstance instance = this->instance();
  uint32_t count = 0;
  if (api.EnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
    error_ = "no physical device";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  if (api.EnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) {
    error_ = "vkEnumeratePhysicalDevices failed";
    return false;
  }

  uint32_t bestScore = 0;
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties props{};
    api.GetPhysicalDeviceProperties(device, &props);

    uint32_t familyCount = 0;
    api.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    if (familyCount == 0) {
      continue;
    }
    std::vector<VkQueueFamilyProperties> families(familyCount);
    api.GetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
      if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
        continue;
      }
      VkBool32 present = VK_FALSE;
      if (api.GetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present) != VK_SUCCESS) {
        continue;
      }
      if (present == VK_TRUE) {
        chosen = i;
        break;
      }
    }
    if (chosen == UINT32_MAX) {
      continue;
    }

    uint32_t score = 1;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score = 4;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
      score = 3;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
      score = 2;
    }
    if (score > bestScore) {
      bestScore = score;
      physical_ = device;
      queueFamily_ = chosen;
    }
  }
  if (physical_ == VK_NULL_HANDLE) {
    error_ = "no device with a graphics + present queue family";
    return false;
  }
  return true;
}

bool VkDeviceContext::EnsureDevice(VkSurfaceKHR surface) {
  if (surface == VK_NULL_HANDLE) {
    error_ = "no presentation surface";
    return false;
  }
  if (!VkInstanceContext::Instance().EnsureInstance()) {
    error_ = VkInstanceContext::Instance().lastError();
    return false;
  }
  const VkInstance instance = this->instance();
  VkApi& shared = GetVkApi();
  if (device_ != VK_NULL_HANDLE) {
    VkBool32 present = VK_FALSE;
    if (shared.GetPhysicalDeviceSurfaceSupportKHR != nullptr &&
        shared.GetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, surface, &present) ==
            VK_SUCCESS &&
        present == VK_TRUE) {
      return true;
    }
    HMRDP_LOGI("vulkan: rebuilding device for a present-capable queue family");
    Destroy();
  }
  if (physical_ == VK_NULL_HANDLE && !PickPhysicalDevice(surface)) {
    return false;
  }

  uint32_t extCount = 0;
  shared.EnumerateDeviceExtensionProperties(physical_, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> extensions(extCount);
  if (extCount > 0) {
    shared.EnumerateDeviceExtensionProperties(physical_, nullptr, &extCount, extensions.data());
  }
  const bool hasSwapchain = HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  if (!hasSwapchain) {
    error_ = "VK_KHR_swapchain not supported";
    return false;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = queueFamily_;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;

  // Optional extensions for the video import (an AVCodec NV12 buffer imported as a VkImage). Each is
  // independently advertised; a missing one only disables that path, never the device.
  std::vector<const char*> enabledExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  if (HasExtension(extensions, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) {
    enabledExtensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
  }
  if (HasExtension(extensions, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME)) {
    enabledExtensions.push_back(VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME);
  }

  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
  deviceInfo.ppEnabledExtensionNames = enabledExtensions.data();

  VkDevice device = VK_NULL_HANDLE;
  const VkResult result = shared.CreateDevice(physical_, &deviceInfo, nullptr, &device);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateDevice failed: " + VkResultName(result);
    return false;
  }
  device_ = device;
  api_ = shared;
  if (!api_.LoadDevice(device_, instance)) {
    error_ = "device entry points missing";
    if (api_.DestroyDevice != nullptr) {
      api_.DestroyDevice(device_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    return false;
  }
  api_.GetDeviceQueue(device_, queueFamily_, 0, &queue_);

  VkPhysicalDeviceProperties props{};
  shared.GetPhysicalDeviceProperties(physical_, &props);
  HMRDP_LOGI("vulkan device ready: %{public}s (queue family %{public}u)",
             props.deviceName, queueFamily_);
  return true;
}

uint32_t VkDeviceContext::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const {
  return FindMemoryType(typeBits, required, 0);
}

uint32_t VkDeviceContext::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                                         VkMemoryPropertyFlags excluded) const {
  if (physical_ == VK_NULL_HANDLE) {
    return UINT32_MAX;
  }
  VkApi& api = GetVkApi();
  VkPhysicalDeviceMemoryProperties props{};
  api.GetPhysicalDeviceMemoryProperties(physical_, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) == 0) {
      continue;
    }
    const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
    if ((flags & required) != required) {
      continue;
    }
    if ((flags & excluded) != 0) {
      continue;
    }
    return i;
  }
  return UINT32_MAX;
}

void VkDeviceContext::Destroy() {
  if (device_ != VK_NULL_HANDLE) {
    if (api_.DestroyDevice != nullptr) {
      api_.DestroyDevice(device_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
    // Drop this device's function pointers with it, so a later EnsureDevice cannot
    // call into a destroyed device.
    api_ = VkApi{};
  }
}

bool VkDeviceContext::CreateSurface(void* nativeWindow, VkSurfaceKHR* out) {
  if (VkInstanceContext::Instance().CreateSurface(nativeWindow, out)) {
    return true;
  }
  error_ = VkInstanceContext::Instance().lastError();
  return false;
}

void VkDeviceContext::DestroySurface(VkSurfaceKHR surface) {
  VkInstanceContext::Instance().DestroySurface(surface);
}

bool CreateDeviceImage(VkApi& api, VkDeviceContext& context, VkFormat format, int width, int height,
                       VkImageUsageFlags usage, VkImage* image, VkDeviceMemory* memory) {
  const VkDevice device = context.device();
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent.width = static_cast<uint32_t>(width);
  info.extent.height = static_cast<uint32_t>(height);
  info.extent.depth = 1;
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (api.CreateImage(device, &info, nullptr, image) != VK_SUCCESS) {
    *image = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements req{};
  api.GetImageMemoryRequirements(device, *image, &req);
  const uint32_t typeIndex =
      context.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (typeIndex == UINT32_MAX) {
    api.DestroyImage(device, *image, nullptr);
    *image = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = typeIndex;
  if (api.AllocateMemory(device, &alloc, nullptr, memory) != VK_SUCCESS) {
    api.DestroyImage(device, *image, nullptr);
    *image = VK_NULL_HANDLE;
    return false;
  }
  if (api.BindImageMemory(device, *image, *memory, 0) != VK_SUCCESS) {
    api.FreeMemory(device, *memory, nullptr);
    api.DestroyImage(device, *image, nullptr);
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool CreateImageView2D(VkApi& api, VkDeviceContext& context, VkFormat format, VkImage image,
                       VkImageView* view) {
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = format;
  info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  info.subresourceRange.levelCount = 1;
  info.subresourceRange.layerCount = 1;
  return api.CreateImageView(context.device(), &info, nullptr, view) == VK_SUCCESS;
}

}  // namespace hmrdp
