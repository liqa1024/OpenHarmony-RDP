#ifndef HMRDP_VK_VIDEO_H
#define HMRDP_VK_VIDEO_H

#include <cstdint>

#include "hmrdp_vk_context.h"

namespace hmrdp {

// Moonlight 超分: turns one decoded frame of an offscreen consumer surface into the same kind of
// picture the gdi desktop produces, so the shared 超分 / letterbox tail cannot tell them apart.
//
// The frame is a graphics-allocator buffer (an OH_NativeImage buffer, never a decoder's own
// OH_AVBuffer — see doc_agent/rdp-present.md §5). Its YUV -> RGB conversion is done here, in the
// shader, rather than by the driver's external-format conversion path: the whole allocation is
// imported as an R8 "byte image" (row pitch x plane rows, luma first, then the interleaved chroma
// plane, which is how the allocator lays such a buffer out) and the convert pass reads luma and
// chroma bytes directly. Why the driver path is avoided, and the geometry rule it forces, are in
// doc_agent/rdp-present.md §5.
//
// Everything here is per-frame scratch plus the objects that only depend on the picture size:
// import/release pairs with the caller's acquire/release of the surface buffer, and the caller owns
// the command buffer the convert pass is recorded into.
class VkVideoImport {
 public:
  explicit VkVideoImport(VkDeviceContext& context);
  ~VkVideoImport();

  VkVideoImport(const VkVideoImport&) = delete;
  VkVideoImport& operator=(const VkVideoImport&) = delete;

  // One imported frame's Vulkan objects; scratch, alive only between Import and Release.
  struct Frame {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // The byte image's geometry: the buffer's row pitch and the first row of the interleaved chroma
    // plane. The visible picture is the top-left `visibleWidth`x`visibleHeight` of the luma plane,
    // which the convert pass reads directly.
    int bufferWidth = 0;
    int bufferHeight = 0;
    int chromaRow = 0;
  };

  // Allocates the picture image and the convert pipeline for a session-resolution picture; false
  // when the picture cannot be built. Rebuilds when the size changes; safe to call every frame.
  bool Ensure(int pictureWidth, int pictureHeight);

  // True when Ensure would tear the picture down and rebuild it: the caller drains the frames still
  // reading the old picture first (see VkRenderer::DrainInFlightLocked).
  bool needsRebuild(int pictureWidth, int pictureHeight) const;

  bool Import(const void* nativeBuffer, int visibleWidth, int visibleHeight, Frame* out);
  void Release(Frame* frame);

  // Records: the imported image's UNDEFINED -> SHADER_READ_ONLY barrier, the descriptor write (byte
  // image + frame parameters) and the convert pass (luma + interleaved chroma -> RGB). Ends with the
  // picture in GENERAL for the shared 超分 / letterbox tail.
  void RecordConvert(VkCommandBuffer cmd, const Frame& frame, int visibleWidth, int visibleHeight);

  VkImage pictureImage() const { return picture_.image; }
  VkImageView pictureView() const { return picture_.view; }
  int pictureWidth() const { return picture_.width; }
  int pictureHeight() const { return picture_.height; }

  std::string lastError() const { return error_; }

  void Destroy();

 private:
  // The picture the convert pass writes: image + view + its extent, rebuilt only on a size change.
  struct Picture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
  };

  bool EnsureConvertPipeline();
  void DestroyConvertPipeline();
  // The YUV -> RGB rows for a buffer colour space (falling back to the driver's suggestion), each
  // already scaled for 0..255 sample values: value = cY*luma + cCb*Cb + cCr*Cr + offset.
  void UpdateColorRows(int colorSpace, int suggestedModel, int suggestedRange);
  // Writes the frame parameters (geometry + colour rows) into the uniform buffer the convert
  // pipeline reads; std140, four vec4s.
  bool WriteFrameParams(const Frame& frame, int visibleWidth, int visibleHeight);

  VkDeviceContext& context_;
  std::string error_;

  Picture picture_;
  float colorRows_[12] = {};

  // One-shot: a per-frame import failure would otherwise log at the stream rate, and the imported
  // buffer's shape only needs printing once.
  bool importWarned_ = false;
  bool importLogged_ = false;

  VkRenderPass convertRenderPass_ = VK_NULL_HANDLE;
  VkFramebuffer convertFramebuffer_ = VK_NULL_HANDLE;
  // Plain sampler (no YCbCr conversion): the pass fetches bytes with texelFetch.
  VkSampler byteSampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout convertSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout convertPipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool convertPool_ = VK_NULL_HANDLE;
  VkDescriptorSet convertSet_ = VK_NULL_HANDLE;
  VkPipeline convertPipeline_ = VK_NULL_HANDLE;
  static constexpr size_t kFrameParamsBytes = 4 * 4 * sizeof(float);
  VkBuffer paramsBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory paramsMemory_ = VK_NULL_HANDLE;
  void* paramsMapped_ = nullptr;
};

}

#endif
