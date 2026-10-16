#ifndef HMRDP_VK_VIDEO_H
#define HMRDP_VK_VIDEO_H

#include <cstdint>
#include <string>
#include <vector>

#include "hmrdp_video_frame.h"
#include "hmrdp_vk_context.h"

namespace hmrdp {

// Moonlight 超分: turns one decoded frame into the same kind of picture the gdi desktop produces, so
// the shared 超分 / letterbox tail cannot tell them apart.
//
// The frame is a graphics-allocator buffer handed out by the codec (an OH_NativeBuffer, never the
// codec's internal OH_AVBuffer). It is imported as an R8 "byte image" over the whole allocation -
// `stride` bytes per row, luma first, then the interleaved chroma plane at row `sliceHeight` - and
// the convert pass reads luma and chroma bytes directly and does the YUV -> RGB itself. That byte
// layout is the codec's own description of a buffer-mode frame (`VideoFrameGeometry`); the importer
// only falls back to the allocation's config when the codec states nothing, and it refuses a buffer
// whose allocation cannot hold the layout it just described.
//
// The byte view is what makes the conversion ours: the importer reads the codec's own bytes and the
// convert pass applies colour-space-correct coefficients, so nothing depends on the driver
// interpreting the buffer. See doc_agent/rdp-present.md §5 for the layout rule.
//
// Imports are cached per buffer: a decoder's output buffers are a small fixed pool reused for the
// whole session, so each buffer is imported (image + memory + view + descriptor set) once and every
// later frame is a lookup. Nothing here is allocated or destroyed per frame.
class VkVideoImport {
 public:
  explicit VkVideoImport(VkDeviceContext& context);
  ~VkVideoImport();

  VkVideoImport(const VkVideoImport&) = delete;
  VkVideoImport& operator=(const VkVideoImport&) = delete;

  // One imported frame, borrowed from the cache: alive as long as the picture it was imported for.
  struct Frame {
    VkImage image = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };

  // Allocates the picture image and the convert pipeline for a session-resolution picture; false
  // when the picture cannot be built. Rebuilds when the size changes (which drops the imports too);
  // safe to call every frame.
  bool Ensure(int pictureWidth, int pictureHeight);

  // True when Ensure would tear the picture down and rebuild it: the caller drains the frames still
  // reading the old picture first (see VkRenderer::DrainInFlightLocked).
  bool needsRebuild(int pictureWidth, int pictureHeight) const;

  // True when one more import would drop the whole cache: the caller drains its in-flight frames
  // before calling Import.
  bool importCacheFull() const;

  // Resolves the frame's layout (codec values first, the allocation's config where the codec states
  // nothing), imports the buffer if it has not been seen yet, and writes the frame parameters the
  // convert pass reads. False when the buffer cannot be imported or its layout is unusable.
  bool Import(const void* nativeBuffer, const VideoFrameGeometry& geometry, Frame* out);

  // Records: the imported image's UNDEFINED -> SHADER_READ_ONLY barrier and the convert pass (luma +
  // interleaved chroma -> RGB). Ends with the picture in GENERAL for the shared 超分 / letterbox tail.
  void RecordConvert(VkCommandBuffer cmd, const Frame& frame);

  VkImage pictureImage() const { return picture_.image; }
  VkImageView pictureView() const { return picture_.view; }

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

  // One codec buffer's import: image + memory + view + the descriptor set that binds it.
  struct Imported {
    uint32_t seq = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    int chromaRow = 0;
  };

  // Bounded so a platform that keeps handing out fresh buffers cannot grow the cache without limit;
  // the caller drains before a miss is served at the cap.
  static constexpr size_t kMaxImports = 8;

  bool EnsureConvertPipeline();
  void DestroyConvertPipeline();
  void ClearImports();
  bool CreateImported(const void* nativeBuffer, const VideoFrameGeometry& geometry, Imported* out);
  void DestroyImported(Imported* imported);
  // The YUV -> RGB rows for a buffer colour space (falling back to the driver's suggestion), each
  // already scaled for 0..255 sample values: value = cY*luma + cCb*Cb + cCr*Cr + offset.
  void UpdateColorRows(int colorSpace, int suggestedModel, int suggestedRange);
  // Writes the frame parameters (visible box, crop origin, chroma row, colour rows) into the uniform
  // buffer the convert pipeline reads; std140, five vec4s.
  void WriteFrameParams(const VideoFrameGeometry& geometry, int chromaRow);

  VkDeviceContext& context_;
  std::string error_;

  Picture picture_;
  std::vector<Imported> imports_;
  float colorRows_[12] = {};

  // One-shot: an import refused every frame would otherwise log at the stream rate, and the imported
  // buffer's shape only needs printing once.
  bool importWarned_ = false;
  bool importLogged_ = false;
  bool derivedLayoutLogged_ = false;

  VkRenderPass convertRenderPass_ = VK_NULL_HANDLE;
  VkFramebuffer convertFramebuffer_ = VK_NULL_HANDLE;
  // Plain sampler (no YCbCr conversion): the pass fetches bytes with texelFetch.
  VkSampler byteSampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout convertSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout convertPipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool convertPool_ = VK_NULL_HANDLE;
  VkPipeline convertPipeline_ = VK_NULL_HANDLE;
  static constexpr size_t kFrameParamsBytes = 5 * 4 * sizeof(float);
  VkBuffer paramsBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory paramsMemory_ = VK_NULL_HANDLE;
  void* paramsMapped_ = nullptr;
};

}

#endif
