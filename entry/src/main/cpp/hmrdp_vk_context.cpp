/*
 * HmRdp - Vulkan context: loader, capability probe and the process-wide
 * instance/device. See hmrdp_vk_context.h and VULKAN-TODO.md §3.2 / §4.1.
 */
#include "hmrdp_vk_context.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "hmrdp_log.h"

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

// Device commands are resolved through vkGetDeviceProcAddr first (cheaper
// dispatch), falling back to vkGetInstanceProcAddr for entries the driver only
// exposes at instance level.
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
  // the whole lifetime, exactly like the EGL display.
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
    for (size_t i = 0; i < exts.size(); ++i) {
      if (i != 0) {
        caps.instanceExtensions += ", ";
      }
      caps.instanceExtensions += exts[i].extensionName;
    }
  }

  if (!VkContext::Instance().EnsureInstance()) {
    caps.probeError = VkContext::Instance().lastError();
    return caps;
  }
  caps.instanceOk = true;
  const VkInstance instance = VkContext::Instance().instance();

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

  // Report the device the renderer would actually pick: best device type first.
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
    if ((flags & VK_QUEUE_COMPUTE_BIT) != 0) {
      caps.computeQueueFamilies++;
    }
    if ((flags & VK_QUEUE_TRANSFER_BIT) != 0) {
      caps.transferQueueFamilies++;
    }
    if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0 && (flags & VK_QUEUE_COMPUTE_BIT) != 0) {
      caps.hasGraphicsComputeQueue = true;
    }
    if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0 && (flags & VK_QUEUE_COMPUTE_BIT) != 0) {
      caps.hasDedicatedComputeQueue = true;
    }
  }

  // Filtered summary: the full device list is far too long for the dev panel.
  char summary[256];
  std::snprintf(summary, sizeof(summary),
                "swapchain=%d timeline=%d extMem=%d extMemFd=%d ohosExtMem=%d (of %u)",
                caps.extKhrSwapchain ? 1 : 0, caps.extTimelineSemaphore ? 1 : 0,
                caps.extExternalMemory ? 1 : 0, caps.extExternalMemoryFd ? 1 : 0,
                caps.extOhosExternalMemory ? 1 : 0, caps.deviceExtensionCount);
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

  // Only the core set is required; the surface/swapchain entries are reported by
  // their callers so a partially supported driver still yields a useful probe.
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
  CmdCopyImage = ResolveDevice<PFN_vkCmdCopyImage>(gdpa, gipa, device, instance, "vkCmdCopyImage");
  CmdBlitImage = ResolveDevice<PFN_vkCmdBlitImage>(gdpa, gipa, device, instance, "vkCmdBlitImage");
  CmdClearColorImage = ResolveDevice<PFN_vkCmdClearColorImage>(gdpa, gipa, device, instance, "vkCmdClearColorImage");
  CmdCopyBufferToImage = ResolveDevice<PFN_vkCmdCopyBufferToImage>(gdpa, gipa, device, instance, "vkCmdCopyBufferToImage");
  CmdCopyImageToBuffer = ResolveDevice<PFN_vkCmdCopyImageToBuffer>(gdpa, gipa, device, instance, "vkCmdCopyImageToBuffer");
  CmdFillBuffer = ResolveDevice<PFN_vkCmdFillBuffer>(gdpa, gipa, device, instance, "vkCmdFillBuffer");
  CmdCopyBuffer = ResolveDevice<PFN_vkCmdCopyBuffer>(gdpa, gipa, device, instance, "vkCmdCopyBuffer");
  CmdUpdateBuffer = ResolveDevice<PFN_vkCmdUpdateBuffer>(gdpa, gipa, device, instance, "vkCmdUpdateBuffer");
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
  FlushMappedMemoryRanges = ResolveDevice<PFN_vkFlushMappedMemoryRanges>(gdpa, gipa, device, instance, "vkFlushMappedMemoryRanges");
  InvalidateMappedMemoryRanges = ResolveDevice<PFN_vkInvalidateMappedMemoryRanges>(gdpa, gipa, device, instance, "vkInvalidateMappedMemoryRanges");
  // Compute: optional (a device without it just keeps the transfer-only engine).
  CreateShaderModule = ResolveDevice<PFN_vkCreateShaderModule>(gdpa, gipa, device, instance, "vkCreateShaderModule");
  DestroyShaderModule = ResolveDevice<PFN_vkDestroyShaderModule>(gdpa, gipa, device, instance, "vkDestroyShaderModule");
  CreateDescriptorSetLayout = ResolveDevice<PFN_vkCreateDescriptorSetLayout>(gdpa, gipa, device, instance, "vkCreateDescriptorSetLayout");
  DestroyDescriptorSetLayout = ResolveDevice<PFN_vkDestroyDescriptorSetLayout>(gdpa, gipa, device, instance, "vkDestroyDescriptorSetLayout");
  CreatePipelineLayout = ResolveDevice<PFN_vkCreatePipelineLayout>(gdpa, gipa, device, instance, "vkCreatePipelineLayout");
  DestroyPipelineLayout = ResolveDevice<PFN_vkDestroyPipelineLayout>(gdpa, gipa, device, instance, "vkDestroyPipelineLayout");
  CreateComputePipelines = ResolveDevice<PFN_vkCreateComputePipelines>(gdpa, gipa, device, instance, "vkCreateComputePipelines");
  DestroyPipeline = ResolveDevice<PFN_vkDestroyPipeline>(gdpa, gipa, device, instance, "vkDestroyPipeline");
  CreateDescriptorPool = ResolveDevice<PFN_vkCreateDescriptorPool>(gdpa, gipa, device, instance, "vkCreateDescriptorPool");
  DestroyDescriptorPool = ResolveDevice<PFN_vkDestroyDescriptorPool>(gdpa, gipa, device, instance, "vkDestroyDescriptorPool");
  AllocateDescriptorSets = ResolveDevice<PFN_vkAllocateDescriptorSets>(gdpa, gipa, device, instance, "vkAllocateDescriptorSets");
  UpdateDescriptorSets = ResolveDevice<PFN_vkUpdateDescriptorSets>(gdpa, gipa, device, instance, "vkUpdateDescriptorSets");
  CmdBindPipeline = ResolveDevice<PFN_vkCmdBindPipeline>(gdpa, gipa, device, instance, "vkCmdBindPipeline");
  CmdBindDescriptorSets = ResolveDevice<PFN_vkCmdBindDescriptorSets>(gdpa, gipa, device, instance, "vkCmdBindDescriptorSets");
  CmdDispatch = ResolveDevice<PFN_vkCmdDispatch>(gdpa, gipa, device, instance, "vkCmdDispatch");

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
      "hostVisDevLocal=%d qf=%u gfxCompute=%d dedicatedCompute=%d",
      VersionString(loaderApiVersion).c_str(), deviceName, VersionString(deviceApiVersion).c_str(),
      driverVersion, DeviceTypeName(deviceType).c_str(), extOhosSurface ? 1 : 0,
      extKhrSwapchain ? 1 : 0, extTimelineSemaphore ? 1 : 0, extExternalMemory ? 1 : 0,
      extExternalMemoryFd ? 1 : 0, extOhosExternalMemory ? 1 : 0,
      memHostVisibleDeviceLocal ? 1 : 0, queueFamilyCount, hasGraphicsComputeQueue ? 1 : 0,
      hasDedicatedComputeQueue ? 1 : 0);
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
         " OHOS_extMem=" + std::string(extOhosExternalMemory ? "1" : "0") + "\n";
  out += "  mem hostVisible=" + std::string(memHostVisible ? "1" : "0") +
         " coherent=" + std::string(memHostCoherent ? "1" : "0") +
         " hostVisDevLocal=" + std::string(memHostVisibleDeviceLocal ? "1" : "0") + "\n";
  out += "  mem types: " + memoryTypes + "\n";
  out += "  queue families=" + std::to_string(queueFamilyCount) +
         " graphics=" + std::to_string(graphicsQueueFamilies) +
         " compute=" + std::to_string(computeQueueFamilies) +
         " transfer=" + std::to_string(transferQueueFamilies) +
         " gfxCompute=" + std::string(hasGraphicsComputeQueue ? "1" : "0") +
         " dedicatedCompute=" + std::string(hasDedicatedComputeQueue ? "1" : "0") + "\n";
  out += "  device ext: " + deviceExtensions;
  return out;
}

const VulkanCapabilities& GetVulkanCapabilities() {
  static std::once_flag once;
  static VulkanCapabilities caps;
  std::call_once(once, []() { caps = ProbeVulkan(); });
  return caps;
}

VkContext& VkContext::Instance() {
  static VkContext context;
  return context;
}

VkContext::~VkContext() {
  Shutdown();
}

std::string VkContext::lastError() const {
  return error_;
}

bool VkContext::EnsureInstance() {
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
  // Request 1.0: universally supported. Anything newer is enabled through
  // extensions so a device whose driver is older than its loader still works.
  appInfo.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = 2;
  createInfo.ppEnabledExtensionNames = extensions;

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

bool VkContext::PickPhysicalDevice(VkSurfaceKHR surface) {
  VkApi& api = GetVkApi();
  uint32_t count = 0;
  if (api.EnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || count == 0) {
    error_ = "no physical device";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  if (api.EnumeratePhysicalDevices(instance_, &count, devices.data()) != VK_SUCCESS) {
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
      // Without a surface (offline harness) any graphics family will do;
      // otherwise the family must also be able to present to it.
      if (surface == VK_NULL_HANDLE) {
        chosen = i;
        break;
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
    error_ = surface == VK_NULL_HANDLE ? "no device with a graphics queue family"
                                      : "no device with a graphics + present queue family";
    return false;
  }
  return true;
}

bool VkContext::EnsureDevice(VkSurfaceKHR surface) {
  VkApi& api = GetVkApi();
  if (device_ != VK_NULL_HANDLE) {
    if (surface == VK_NULL_HANDLE) {
      return true;
    }
    // The device may have been created by the offline harness without a surface.
    // If its queue family cannot present to this one, rebuild with a family that
    // can (VULKAN-TODO §4.2 item 1 keeps one device for the process once it is usable).
    VkBool32 present = VK_FALSE;
    if (api.GetPhysicalDeviceSurfaceSupportKHR != nullptr &&
        api.GetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, surface, &present) ==
            VK_SUCCESS &&
        present == VK_TRUE) {
      return true;
    }
    HMRDP_LOGI("vulkan: rebuilding device for a present-capable queue family");
    DestroyDevice();
  }
  if (surface == VK_NULL_HANDLE) {
    // No surface: the instance alone is not enough, but a device is still needed.
    if (!EnsureInstance()) {
      return false;
    }
  } else if (!EnsureInstance()) {
    return false;
  }
  if (physical_ == VK_NULL_HANDLE && !PickPhysicalDevice(surface)) {
    return false;
  }

  uint32_t extCount = 0;
  api.EnumerateDeviceExtensionProperties(physical_, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> extensions(extCount);
  if (extCount > 0) {
    api.EnumerateDeviceExtensionProperties(physical_, nullptr, &extCount, extensions.data());
  }
  const bool hasSwapchain = HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  if (surface != VK_NULL_HANDLE && !hasSwapchain) {
    error_ = "VK_KHR_swapchain not supported";
    return false;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = queueFamily_;
  queueInfo.queueCount = 1;
  queueInfo.pQueuePriorities = &priority;

  const char* enabledExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  if (surface != VK_NULL_HANDLE && hasSwapchain) {
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = enabledExtensions;
  }

  VkDevice device = VK_NULL_HANDLE;
  const VkResult result = api.CreateDevice(physical_, &deviceInfo, nullptr, &device);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateDevice failed: " + VkResultName(result);
    return false;
  }
  device_ = device;
  if (!api.LoadDevice(device_, instance_)) {
    error_ = "device entry points missing";
    if (api.DestroyDevice != nullptr) {
      api.DestroyDevice(device_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    return false;
  }
  api.GetDeviceQueue(device_, queueFamily_, 0, &queue_);

  VkPhysicalDeviceProperties props{};
  api.GetPhysicalDeviceProperties(physical_, &props);
  HMRDP_LOGI("vulkan device ready: %{public}s (queue family %{public}u)",
             props.deviceName, queueFamily_);
  return true;
}

uint32_t VkContext::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const {
  return FindMemoryType(typeBits, required, 0);
}

uint32_t VkContext::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
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

void VkContext::DestroyDevice() {
  VkApi& api = GetVkApi();
  if (device_ != VK_NULL_HANDLE) {
    if (api.DestroyDevice != nullptr) {
      api.DestroyDevice(device_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
  }
}

void VkContext::Shutdown() {
  VkApi& api = GetVkApi();
  DestroyDevice();
  if (instance_ != VK_NULL_HANDLE) {
    if (api.DestroyInstance != nullptr) {
      api.DestroyInstance(instance_, nullptr);
    }
    instance_ = VK_NULL_HANDLE;
  }
}

bool VkContext::CreateSurface(void* nativeWindow, VkSurfaceKHR* out) {
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

void VkContext::DestroySurface(VkSurfaceKHR surface) {
  if (surface == VK_NULL_HANDLE) {
    return;
  }
  VkApi& api = GetVkApi();
  if (api.DestroySurfaceKHR != nullptr && instance_ != VK_NULL_HANDLE) {
    api.DestroySurfaceKHR(instance_, surface, nullptr);
  }
}

}  // namespace hmrdp
