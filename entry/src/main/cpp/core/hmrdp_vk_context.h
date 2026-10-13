// libvulkan.so is dlopen()ed (not linked) so a device without a usable driver degrades to gdi instead
// of failing to load; every command is fetched at runtime via vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#ifndef HMRDP_VK_CONTEXT_H
#define HMRDP_VK_CONTEXT_H

#include <cstdint>
#include <string>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#ifndef VK_USE_PLATFORM_OHOS
#define VK_USE_PLATFORM_OHOS 1
#endif
#include <vulkan/vulkan.h>

namespace hmrdp {

struct VkApi {
  bool loaded = false;
  std::string loadError;

  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;

  PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
  PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
  PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
  PFN_vkCreateInstance CreateInstance = nullptr;

  PFN_vkDestroyInstance DestroyInstance = nullptr;
  PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
  PFN_vkCreateDevice CreateDevice = nullptr;
  PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
  PFN_vkCreateSurfaceOHOS CreateSurfaceOHOS = nullptr;

  PFN_vkDestroyDevice DestroyDevice = nullptr;
  PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
  PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
  PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
  PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
  PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
  PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
  PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
  PFN_vkCreateSemaphore CreateSemaphore = nullptr;
  PFN_vkDestroySemaphore DestroySemaphore = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkDestroyFence DestroyFence = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
  PFN_vkCreateRenderPass CreateRenderPass = nullptr;
  PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
  PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
  PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkDestroyImageView DestroyImageView = nullptr;
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
  PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
  PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkUnmapMemory UnmapMemory = nullptr;
  PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;
  PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;
  PFN_vkCreateQueryPool CreateQueryPool = nullptr;
  PFN_vkDestroyQueryPool DestroyQueryPool = nullptr;
  PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
  PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;
  PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;

  PFN_vkCreateShaderModule CreateShaderModule = nullptr;
  PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
  PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
  PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
  PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
  PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
  PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
  PFN_vkCreateSampler CreateSampler = nullptr;
  PFN_vkDestroySampler DestroySampler = nullptr;
  PFN_vkCmdSetViewport CmdSetViewport = nullptr;
  PFN_vkCmdSetScissor CmdSetScissor = nullptr;
  PFN_vkCmdDraw CmdDraw = nullptr;
  PFN_vkDestroyPipeline DestroyPipeline = nullptr;
  PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
  PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
  PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
  PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
  PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
  PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
  PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;

  bool LoadInstance(VkInstance instance);
  bool LoadDevice(VkDevice device, VkInstance instance);
};

VkApi& GetVkApi();

std::string VkResultName(int32_t result);

struct VulkanCapabilities {
  bool loaderPresent = false;
  std::string loadError;
  bool instanceOk = false;
  bool deviceFound = false;
  std::string probeError;

  uint32_t loaderApiVersion = 0;
  uint32_t deviceApiVersion = 0;
  uint32_t driverVersion = 0;
  uint32_t vendorId = 0;
  uint32_t deviceId = 0;
  uint32_t deviceType = 0;
  char deviceName[256] = {0};

  bool extKhrSurface = false;
  bool extOhosSurface = false;
  bool extKhrSwapchain = false;
  bool extTimelineSemaphore = false;
  bool extExternalMemory = false;
  bool extExternalMemoryFd = false;
  bool extOhosExternalMemory = false;

  bool xegLibrary = false;
  bool xegSpatialUpscale = false;

  bool memHostVisible = false;
  bool memHostCoherent = false;
  bool memHostVisibleDeviceLocal = false;

  uint32_t queueFamilyCount = 0;
  uint32_t graphicsQueueFamilies = 0;
  uint32_t transferQueueFamilies = 0;

  uint32_t instanceExtensionCount = 0;
  uint32_t deviceExtensionCount = 0;
  std::string instanceExtensions;
  std::string deviceExtensions;
  std::string memoryTypes;

  bool usable() const { return deviceFound; }
  std::string Describe() const;
  std::string DescribeLines() const;

  bool presenterSupported = false;
  std::string presenterUnsupportedCode;

  bool srFsrSupported = false;
  bool srXengineSupported = false;
  std::string srXengineUnsupportedCode;
  bool srSupported = false;
  std::string srUnsupportedCode;
};

const VulkanCapabilities& GetVulkanCapabilities();

class VkInstanceContext {
 public:
  static VkInstanceContext& Instance();

  bool EnsureInstance();
  VkInstance instance() const { return instance_; }
  std::string lastError() const { return error_; }
  VkApi& api() { return GetVkApi(); }

  bool CreateSurface(void* nativeWindow, VkSurfaceKHR* out);
  void DestroySurface(VkSurfaceKHR surface);

 private:
  VkInstanceContext() = default;
  ~VkInstanceContext();
  VkInstanceContext(const VkInstanceContext&) = delete;
  VkInstanceContext& operator=(const VkInstanceContext&) = delete;

  VkInstance instance_ = VK_NULL_HANDLE;
  std::string error_;
};

class VkDeviceContext {
 public:
  VkDeviceContext() = default;
  ~VkDeviceContext();
  VkDeviceContext(const VkDeviceContext&) = delete;
  VkDeviceContext& operator=(const VkDeviceContext&) = delete;

  bool EnsureDevice(VkSurfaceKHR surface);
  void Destroy();
  bool ready() const { return device_ != VK_NULL_HANDLE; }
  std::string lastError() const { return error_; }

  VkApi& api() { return api_; }
  VkInstance instance() const { return VkInstanceContext::Instance().instance(); }
  VkPhysicalDevice physicalDevice() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queueFamily() const { return queueFamily_; }

  uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
  uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags excluded) const;

  bool CreateSurface(void* nativeWindow, VkSurfaceKHR* out);
  void DestroySurface(VkSurfaceKHR surface);

 private:
  bool PickPhysicalDevice(VkSurfaceKHR surface);

  VkApi api_;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  std::string error_;
};

}

#endif
