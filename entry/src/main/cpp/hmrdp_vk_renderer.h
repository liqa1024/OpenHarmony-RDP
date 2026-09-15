/*
 * HmRdp - Vulkan presenter for one XComponent surface.
 *
 * instance -> device -> VkSurfaceKHR -> swapchain, plus the letterboxed blit of
 * the GPU desktop engine's composed screen image onto the swapchain (image to
 * image, no CPU round trip).
 *
 * CPU (gdi) frames do NOT come through here: they go through WinPresenter and the
 * native window buffer queue, so the soft-decode path needs no GPU API at all
 * (hmrdp_win_presenter.h). This presenter only serves the engine screen.
 */
#ifndef HMRDP_VK_RENDERER_H
#define HMRDP_VK_RENDERER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "hmrdp_vk_context.h"

namespace hmrdp {

class VkRenderer {
 public:
  VkRenderer();
  ~VkRenderer();

  VkRenderer(const VkRenderer&) = delete;
  VkRenderer& operator=(const VkRenderer&) = delete;

  // Called from the ArkUI (main) thread when the XComponent surface changes.
  void SetSurface(void* nativeWindow, int width, int height);
  void ResizeSurface(int width, int height);
  void DestroySurface();

  // Creates the swapchain and presents one black frame, so the surface shows
  // something before the first real frame. Also pins the image format the engine
  // (and the CPU frame upload) must be created with.
  bool Prepare();
  // Blits an image that lives on the same VkDevice (the GPU desktop engine's
  // composed screen, VK_IMAGE_LAYOUT_GENERAL) into the swapchain, letterboxed and
  // cleared to black outside the picture. No CPU readback: one device, one
  // queue, image-to-image. `imageFormat` must equal the swapchain format -
  // channel order cannot be converted by a blit.
  bool PresentImage(VkImage image, VkFormat imageFormat, int width, int height);
  // Swapchain image format (VK_FORMAT_UNDEFINED until a swapchain exists).
  VkFormat format() const;

  void Reset();

  bool ready() const;
  std::string lastError() const;
  // Swapchain facts for the dev panel / logs.
  std::string Describe() const;

 private:
  static constexpr uint32_t kFramesInFlight = 2;

  // All of these expect mutex_ to be held.
  bool EnsureSwapchainLocked();
  bool CreateSwapchainLocked();
  void DestroySwapchainLocked();
  void DestroyFrameResourcesLocked();
  // Clears the swapchain image to one colour and presents it (Prepare's body);
  // re-creates the swapchain when it is out of date.
  bool PresentSolidLocked(uint8_t r, uint8_t g, uint8_t b);
  // Acquires the swapchain image after waiting for this frame slot's fence, and
  // resets that fence. `retry` is set when the swapchain was out of date and the
  // caller must rebuild it and call again.
  bool AcquireFrameLocked(uint32_t* imageIndex, bool* retry);
  // Records the letterboxed copy/blit of `src` (VK_IMAGE_LAYOUT_GENERAL) into the
  // acquired swapchain image, and leaves that image in PRESENT_SRC_KHR.
  void RecordBlitLocked(VkCommandBuffer cmd, VkImage src, int srcWidth, int srcHeight,
                        uint32_t imageIndex);
  // Ends `cmd`, submits it with this frame slot's fence and presents the image.
  bool SubmitAndPresentLocked(VkCommandBuffer cmd, uint32_t imageIndex);

  // Mutable because the const inspectors (ready/lastError/Describe) lock it too.
  mutable std::mutex mutex_;

  void* pendingWindow_ = nullptr;
  int surfaceWidth_ = 0;
  int surfaceHeight_ = 0;
  bool surfaceDirty_ = true;

  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_ = {0, 0};
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  // One per swapchain image: a signal semaphore must not be reused until the
  // image it presented has been re-acquired.
  std::vector<VkSemaphore> renderFinished_;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;

  VkCommandPool commandPool_ = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffers_[kFramesInFlight] = {};
  VkSemaphore imageAvailable_[kFramesInFlight] = {};
  VkFence inFlight_[kFramesInFlight] = {};
  uint32_t frameIndex_ = 0;
  // Presents reached the screen since construction. Logged periodically so a
  // long real-device run is verifiable from hilog alone (no UI needed).
  uint64_t presentCount_ = 0;


  std::string error_;
  std::string info_;
};

}  // namespace hmrdp

#endif  // HMRDP_VK_RENDERER_H
