/*
 * HmRdp - Vulkan presenter for one XComponent surface.
 *
 * instance -> device -> VkSurfaceKHR -> swapchain, plus the letterboxed blit of
 * a source picture onto the swapchain. Two sources are supported:
 *
 *  - `PresentImage`: the GPU desktop engine's composed screen image (image to
 *    image, no CPU round trip);
 *  - `PresentBgraFrame`: one CPU (gdi) frame region uploaded into a persistent
 *    desktop image - the live fallback path when the GPU engine does not take
 *    over, and the replay harness' CPU route.
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
  // Presents one CPU (gdi) frame region through a persistent desktop image.
  // `data` is the whole desktop buffer (top-down BGRA, `srcStride` bytes/row) and
  // `x,y,width,height` the region that changed; `desktopWidth/Height` are the
  // desktop dimensions used for the letterbox. The first frame after a (re)create
  // and every desktop-size change upload the whole desktop, so the caller does
  // not have to track that itself; R/B are swapped when the swapchain is RGBA8.
  bool PresentBgraFrame(const uint8_t* data, int srcStride, int desktopWidth, int desktopHeight,
                        int x, int y, int width, int height);
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
  // Upload path of PresentBgraFrame: creates/keeps the desktop image and its
  // staging buffers, and records one dirty-rect upload into `cmd`.
  bool EnsureDesktopImageLocked(int width, int height);
  void DestroyDesktopImageLocked();
  bool EnsureStageLocked(size_t bytes);
  void DestroyStageLocked();

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

  // CPU frame path: the accumulated desktop picture the dirty rects are uploaded
  // into. It is kept in VK_IMAGE_LAYOUT_GENERAL for its whole life, so no layout
  // tracking is needed (the blit reads it in GENERAL, like the engine screen).
  VkImage desktopImage_ = VK_NULL_HANDLE;
  VkDeviceMemory desktopImageMemory_ = VK_NULL_HANDLE;
  VkFormat desktopImageFormat_ = VK_FORMAT_UNDEFINED;
  int desktopImageWidth_ = 0;
  int desktopImageHeight_ = 0;
  // Set when the desktop image was just (re)created: the next frame must upload
  // the whole desktop, because the image starts out undefined.
  bool desktopImageFullUpload_ = true;
  // One host-visible staging buffer per frame in flight, so a frame's upload can
  // never race the copy of the frame that is still executing.
  VkBuffer stageBuffers_[kFramesInFlight] = {};
  VkDeviceMemory stageMemories_[kFramesInFlight] = {};
  void* stageMapped_[kFramesInFlight] = {};
  size_t stageCapacities_[kFramesInFlight] = {};

  std::string error_;
  std::string info_;
};

}  // namespace hmrdp

#endif  // HMRDP_VK_RENDERER_H
