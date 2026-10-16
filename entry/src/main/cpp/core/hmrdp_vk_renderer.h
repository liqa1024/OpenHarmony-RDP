#ifndef HMRDP_VK_RENDERER_H
#define HMRDP_VK_RENDERER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "hmrdp_presenter.h"
#include "hmrdp_vk_context.h"
#include "hmrdp_vk_video.h"
#include "hmrdp_xeg.h"

namespace hmrdp {

class VkRenderer : public FramePresenter {
 public:
  VkRenderer();
  ~VkRenderer();

  VkRenderer(const VkRenderer&) = delete;
  VkRenderer& operator=(const VkRenderer&) = delete;

  void SetSurface(void* nativeWindow, int width, int height) override;
  void ResizeSurface(int width, int height) override;
  void DestroySurface() override;

  bool Prepare() override;
  bool PresentBgra(const uint8_t* data, int srcStride, int desktopWidth, int desktopHeight,
                   const PresentRect* rects, int rectCount) override;
  bool PresentVideo(const void* nativeBuffer, int inputWidth, int inputHeight) override;
  VkFormat format() const;

  uint8_t* AcquireDesktopBuffer(int width, int height, int* stride) override;
  void BeginDesktopBufferWrite() override;
  void ReleaseDesktopBuffer() override;
  bool usesDesktopBuffer() const override;
  void SetSuperResolution(bool enabled, SuperResolutionBackend backend, int outputWidth,
                          int outputHeight) override;
  uint64_t TakePresentWaitUs() override;
  void TakeGpuTimings(uint64_t* copyUs, uint64_t* srUs, uint64_t* blitUs) override;

  void Reset() override;

  bool ready() const;
  std::string lastError() const;
  std::string Describe() const;

 private:
  static constexpr uint32_t kFramesInFlight = 2;

  bool EnsureSwapchainLocked();
  bool CreateSwapchainLocked();
  void DestroySwapchainLocked();
  void DestroyFrameResourcesLocked();
  bool PresentSolidLocked(uint8_t r, uint8_t g, uint8_t b);
  bool AcquireFrameLocked(uint32_t* imageIndex, bool* retry);
  bool SubmitAndPresentLocked(VkCommandBuffer cmd, uint32_t imageIndex);
  bool EnsurePresentPipelineLocked();
  void DestroyPresentPipelineLocked();
  bool EnsurePresentTimerLocked();
  void CollectPresentTimerLocked(uint32_t slot);
  void UpdatePresentDescriptorLocked(uint32_t slot, VkImageView view);
  void RecordPresentQuadLocked(VkCommandBuffer cmd, int srcWidth, int srcHeight,
                               uint32_t imageIndex);
  bool EnsureDesktopImageLocked(int width, int height);
  void DestroyDesktopImageLocked();
  // Waits out every frame in flight, for a rare (re)creation whose targets the GPU may still read.
  void DrainInFlightLocked();
  bool EnsureStageLocked(size_t bytes);
  void DestroyStageLocked();
  bool CreateDesktopBufferLocked(int width, int height);
  void DestroyDesktopBufferLocked();

  bool EnsureSuperResolutionLocked(int inputWidth, int inputHeight, VkImageView inputView);
  bool EnsureXengineLocked(int inputWidth, int inputHeight, int outputWidth, int outputHeight);
  bool EnsureFsrLocked(int inputWidth, int inputHeight, VkImageView inputView, int outputWidth,
                       int outputHeight);
  bool RecordSuperResolutionLocked(VkCommandBuffer cmd, VkImageView inputView);
  void DestroySuperResolutionLocked();
  void DestroyFsrLocked();

  // The picture the shared 超分 / letterbox tail consumes. Each producer supplies the image, the
  // view, its extent and how it was last written (`producerStage`/`producerAccess`); the tail owns
  // the dependency barrier on that write, so no producer can forget it (RDP's is a buffer->image
  // transfer, Moonlight's is the NV12 convert render pass). The image must stay in GENERAL.
  struct PresentInput {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
    VkPipelineStageFlags producerStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkAccessFlags producerAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
  };

  // Shared tail of every present: input dependency barrier, (optional) 超分 of `input.view` into the
  // output image, the GPU timestamps around it, then the letterbox quad into the acquired swapchain
  // image and the submit. `srActive` makes the source the output image at the output extent.
  bool RecordPresentTailLocked(VkCommandBuffer cmd, uint32_t slot, uint32_t imageIndex,
                               const PresentInput& input, bool srActive, bool timers);

  mutable std::mutex mutex_;

  VkDeviceContext vk_;

  void* pendingWindow_ = nullptr;
  int surfaceWidth_ = 0;
  int surfaceHeight_ = 0;
  bool surfaceDirty_ = true;

  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_ = {0, 0};
  VkFormat renderPassFormat_ = VK_FORMAT_UNDEFINED;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkImageView desktopImageView_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout presentSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout presentPipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool presentPool_ = VK_NULL_HANDLE;
  // One set per frame in flight: a set may not be rewritten while a frame that bound it is in flight.
  VkDescriptorSet presentSets_[kFramesInFlight] = {};
  VkPipeline presentPipeline_ = VK_NULL_HANDLE;
  VkFormat presentPipelineFormat_ = VK_FORMAT_UNDEFINED;
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  // One per swapchain image: a signal semaphore must not be reused until its image is re-acquired.
  std::vector<VkSemaphore> renderFinished_;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;

  static constexpr uint32_t kPresentTimestampsPerSlot = 4;
  VkQueryPool presentTimer_[kFramesInFlight] = {};
  double presentNsPerTick_ = 0.0;
  uint64_t presentTimerCopyNs_ = 0;
  uint64_t presentTimerSrNs_ = 0;
  uint64_t presentTimerBlitNs_ = 0;
  uint64_t presentTimerFrames_ = 0;
  uint64_t gpuWindowCopyNs_ = 0;
  uint64_t gpuWindowSrNs_ = 0;
  uint64_t gpuWindowBlitNs_ = 0;

  VkCommandPool commandPool_ = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffers_[kFramesInFlight] = {};
  VkSemaphore imageAvailable_[kFramesInFlight] = {};
  VkFence inFlight_[kFramesInFlight] = {};
  uint32_t frameIndex_ = 0;
  uint64_t presentCount_ = 0;
  uint64_t presentWaitUs_ = 0;

  VkImage desktopImage_ = VK_NULL_HANDLE;
  VkDeviceMemory desktopImageMemory_ = VK_NULL_HANDLE;
  VkFormat desktopImageFormat_ = VK_FORMAT_UNDEFINED;
  int desktopImageWidth_ = 0;
  int desktopImageHeight_ = 0;
  bool desktopImageFullUpload_ = true;
  // One per frame in flight so an upload cannot race the previous frame's copy.
  VkBuffer stageBuffers_[kFramesInFlight] = {};
  VkDeviceMemory stageMemories_[kFramesInFlight] = {};
  void* stageMapped_[kFramesInFlight] = {};
  size_t stageCapacities_[kFramesInFlight] = {};

  VkBuffer desktopBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory desktopBufferMemory_ = VK_NULL_HANDLE;
  void* desktopBufferMapped_ = nullptr;
  size_t desktopBufferBytes_ = 0;
  int desktopBufferWidth_ = 0;
  int desktopBufferHeight_ = 0;
  int desktopBufferStride_ = 0;
  bool desktopBufferCoherent_ = false;
  VkFence desktopBufferFence_ = VK_NULL_HANDLE;
  bool desktopBufferFencePending_ = false;

  std::string error_;
  std::string info_;

  bool srRequested_ = false;
  SuperResolutionBackend srBackend_ = SuperResolutionBackend::kXengine;
  int srOutputWidth_ = 0;
  int srOutputHeight_ = 0;
  bool srFailed_ = false;
  XegSpatialUpscale srUpscale_;
  VkImage srImage_ = VK_NULL_HANDLE;
  VkDeviceMemory srImageMemory_ = VK_NULL_HANDLE;
  VkImageView srImageView_ = VK_NULL_HANDLE;
  int srImageWidth_ = 0;
  int srImageHeight_ = 0;
  int srInputWidth_ = 0;
  int srInputHeight_ = 0;
  // The image view SR samples (RDP's desktop image, or the Moonlight video input image). FSR bakes
  // it into its EASU descriptor, so it is remembered to detect a change.
  VkImageView srInputView_ = VK_NULL_HANDLE;

  VkRenderPass fsrRenderPass_ = VK_NULL_HANDLE;
  VkSampler fsrSampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout fsrSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout fsrPipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool fsrPool_ = VK_NULL_HANDLE;
  VkDescriptorSet fsrEasuSet_ = VK_NULL_HANDLE;
  VkDescriptorSet fsrRcasSet_ = VK_NULL_HANDLE;
  VkPipeline fsrEasuPipeline_ = VK_NULL_HANDLE;
  VkPipeline fsrRcasPipeline_ = VK_NULL_HANDLE;
  VkImage fsrEasuImage_ = VK_NULL_HANDLE;
  VkDeviceMemory fsrEasuImageMemory_ = VK_NULL_HANDLE;
  VkImageView fsrEasuImageView_ = VK_NULL_HANDLE;
  VkFramebuffer fsrEasuFramebuffer_ = VK_NULL_HANDLE;
  VkFramebuffer fsrRcasFramebuffer_ = VK_NULL_HANDLE;
  static constexpr size_t kFsrParamsBytes = 16 * sizeof(float);
  VkBuffer fsrEasuParams_ = VK_NULL_HANDLE;
  VkDeviceMemory fsrEasuParamsMemory_ = VK_NULL_HANDLE;
  void* fsrEasuParamsMapped_ = nullptr;
  VkBuffer fsrRcasParams_ = VK_NULL_HANDLE;
  VkDeviceMemory fsrRcasParamsMemory_ = VK_NULL_HANDLE;
  void* fsrRcasParamsMapped_ = nullptr;
  bool fsrParamsCoherent_ = false;

  // Moonlight 超分 video input: an imported NV12 frame is converted into a session-resolution
  // picture, which then plays the desktop picture's role for the shared 超分 / letterbox tail
  // (`PresentVideo`). The import/convert machinery lives in hmrdp_vk_video.h.
  VkVideoImport videoImport_;
};

}

#endif
