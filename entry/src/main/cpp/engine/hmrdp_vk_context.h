/*
 * HmRdp - Vulkan context: runtime loader, capability probe and the
 * process-wide VkInstance/VkDevice shared by the rendering backend.
 *
 * libvulkan.so is loaded with dlopen instead of being linked: a device without a
 * usable Vulkan driver must degrade to FreeRDP's gdi path, not fail to load the
 * application (same rule as the OHAudio sink, see AGENTS.md). Every command is
 * therefore fetched through vkGetInstanceProcAddr / vkGetDeviceProcAddr.
 */
#ifndef HMRDP_VK_CONTEXT_H
#define HMRDP_VK_CONTEXT_H

#include <cstdint>
#include <string>

// Every Vulkan command is resolved at runtime (see the file comment above).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#ifndef VK_USE_PLATFORM_OHOS
#define VK_USE_PLATFORM_OHOS 1
#endif
#include <vulkan/vulkan.h>

namespace hmrdp {

// Runtime-resolved Vulkan entry points. `loaded` is false when libvulkan.so is
// missing or exposes no loader; every other member is then null.
//
// Global commands are resolved by GetVkApi(); the instance- and device-level
// entries are only valid after LoadInstance() / LoadDevice() (per the loader
// spec, vkGetInstanceProcAddr with a null instance is only guaranteed to return
// the global commands).
struct VkApi {
  bool loaded = false;
  std::string loadError;

  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;

  // Global commands.
  PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
  PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
  PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
  PFN_vkCreateInstance CreateInstance = nullptr;

  // Instance level (valid after LoadInstance).
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

  // Device level (valid after LoadDevice).
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
  // GPU timestamps (dev perf attribution): the presenter's own present-cost probe.
  PFN_vkCreateQueryPool CreateQueryPool = nullptr;
  PFN_vkDestroyQueryPool DestroyQueryPool = nullptr;
  PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
  PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;
  PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;

  // The presenter draws its letterboxed picture with one quad, so it needs the
  // shader / descriptor / graphics-pipeline entry points. Resolved
  // opportunistically: a device that cannot do them simply fails to present.
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

  // Fills the instance-level commands. Returns false if a required entry is
  // absent (the platform then cannot be used).
  bool LoadInstance(VkInstance instance);
  // Fills the device-level commands.
  bool LoadDevice(VkDevice device, VkInstance instance);
};

// Loads libvulkan.so and the global commands (cached for the process), then
// `VkInstanceContext::EnsureInstance()` fills in the instance-level ones. Only
// this process-wide table holds global + instance entries; each device resolves
// its own entries into a private copy (VkDeviceContext::api()), so a device can
// never reach another device's function pointers.
VkApi& GetVkApi();

// "VK_SUCCESS" / "VK_ERROR_..." for logs.
std::string VkResultName(int32_t result);

// Capability report: what the platform actually offers. It is a snapshot for the
// dev panel and decides whether the Vulkan presenter can be used at all.
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
  uint32_t deviceType = 0;  // VkPhysicalDeviceType
  char deviceName[256] = {0};

  bool extKhrSurface = false;
  bool extOhosSurface = false;
  bool extKhrSwapchain = false;
  bool extTimelineSemaphore = false;
  bool extExternalMemory = false;
  bool extExternalMemoryFd = false;
  bool extOhosExternalMemory = false;

  // XEngine (超分): whether libxengine.so is loadable, and whether the probed
  // device advertises its XEG_spatial_upscale feature.
  bool xegLibrary = false;
  bool xegSpatialUpscale = false;

  bool memHostVisible = false;
  bool memHostCoherent = false;
  // A DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT type: when present, an upload does
  // not need a staging copy.
  bool memHostVisibleDeviceLocal = false;

  uint32_t queueFamilyCount = 0;
  uint32_t graphicsQueueFamilies = 0;
  uint32_t transferQueueFamilies = 0;

  uint32_t instanceExtensionCount = 0;
  uint32_t deviceExtensionCount = 0;
  // Comma-separated summaries for the on-device panel.
  std::string instanceExtensions;
  std::string deviceExtensions;
  std::string memoryTypes;

  bool usable() const { return deviceFound; }
  // One-line summary (logs) and a multi-line variant (dev panel).
  std::string Describe() const;
  std::string DescribeLines() const;

  // --- Presenter verdict ----------------------------------------------------
  // Whether the Vulkan *presenter* can be used: a device that can blit to the
  // XComponent surface plus host-visible memory for the frame buffer. When it is
  // false the GLES presenter takes over (hmrdp_presenter.h). `presenterUnsupportedCode`
  // is a short stable token so the UI layer owns the wording: "emulator" /
  // "no-vulkan" / "no-instance" / "no-device" / "no-host-memory" / "no-surface".
  bool presenterSupported = false;
  std::string presenterUnsupportedCode;

  // --- Super resolution (超分) verdicts ---------------------------------------
  // 超分 rides on the Vulkan presenter (both backends render into a presenter
  // image), so the presenter verdict comes first. On top of that, FSR needs only
  // the presenter's own pipeline - a build-time question, not a device one - while
  // XEngine needs the device's XEG_spatial_upscale extension. `srSupported` means
  // "at least one backend", with `srUnsupportedCode` carrying the reason when none
  // is; the per-backend flags let the UI offer them separately.
  bool srFsrSupported = false;
  bool srXengineSupported = false;
  // "no-xengine" / "no-extension", or the presenter code when that is the blocker.
  std::string srXengineUnsupportedCode;
  bool srSupported = false;
  std::string srUnsupportedCode;
};

// Cached capability probe. Brings up a short-lived VkInstance (reusing the shared
// one) but never a device, so it is safe to call before any surface exists.
const VulkanCapabilities& GetVulkanCapabilities();

// Process-wide Vulkan instance, shared and never destroyed while the process
// lives. It is the only Vulkan object sessions share: surfaces are created from
// it, while every presenter owns its own device (VkDeviceContext), so two
// sessions never submit on the same queue (doc_agent/architecture.md §4.1).
class VkInstanceContext {
 public:
  static VkInstanceContext& Instance();

  // Creates the instance once. Returns false and stays unusable on failure.
  bool EnsureInstance();
  VkInstance instance() const { return instance_; }
  std::string lastError() const { return error_; }
  // The instance-level command set (global + instance commands).
  VkApi& api() { return GetVkApi(); }

  // Wraps the XComponent OHNativeWindow into a VkSurfaceKHR (created from the
  // shared instance).
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

// One presenter's Vulkan device + queue, created from the shared instance for the
// surface it presents to. Owned by the presenter and destroyed with it - never
// shared between sessions.
class VkDeviceContext {
 public:
  VkDeviceContext() = default;
  ~VkDeviceContext();
  VkDeviceContext(const VkDeviceContext&) = delete;
  VkDeviceContext& operator=(const VkDeviceContext&) = delete;

  // Creates the device with one graphics + present queue family for `surface`.
  // When a surface is supplied later and the existing queue family cannot present
  // to it, the device is rebuilt. Returns false and stays unusable on failure.
  bool EnsureDevice(VkSurfaceKHR surface);
  // Destroys the device (idempotent). The shared instance survives.
  void Destroy();
  bool ready() const { return device_ != VK_NULL_HANDLE; }
  std::string lastError() const { return error_; }

  // This device's command set: global + instance entries plus its own device
  // entries, so a call can never reach another device's function pointers.
  VkApi& api() { return api_; }
  VkInstance instance() const { return VkInstanceContext::Instance().instance(); }
  VkPhysicalDevice physicalDevice() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queueFamily() const { return queueFamily_; }

  // Index of a memory type satisfying `typeBits` and `required` (and carrying none
  // of `excluded`), or UINT32_MAX.
  uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
  uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags excluded) const;

  // Surface helpers routed through the shared instance, so a presenter has one
  // entry point for both the instance and its own device.
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

}  // namespace hmrdp

#endif  // HMRDP_VK_CONTEXT_H
