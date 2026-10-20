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
// interpreting the buffer. See doc_agent/present.md §4 for the layout rule.
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

  // True when the cache is at its cap and this buffer is not in it: the caller drains the frames
  // still reading the cached imports before calling Import (which then evicts the least recently
  // used one).
  bool needsDrainFor(const void* nativeBuffer) const;

  // Resolves the frame's layout (codec values first, the allocation's config where the codec states
  // nothing), imports the buffer if it has not been seen yet, and writes the frame parameters the
  // convert pass reads. `slot` is the presenter frame slot whose picture/UBO this frame uses.
  bool Import(const void* nativeBuffer, const VideoFrameGeometry& geometry, uint32_t slot,
              Frame* out);

  // The colour space the session negotiated with the host for these frames (OH_NativeBuffer_ColorSpace,
  // 0 = not stated). The importer still renders what each buffer itself declares; this is only what that
  // declaration is checked against, and what a buffer stating nothing falls back to.
  void SetExpectedColorSpace(int colorSpace);

  // True once an imported buffer declared a colour space disagreeing with the expected one - another
  // matrix family or range, or one this byte importer cannot express at all. The picture may then differ
  // from what the host encoded, so the session says so instead of presenting it silently.
  bool colorMismatch() const { return colorMismatch_; }

  // Records: the imported image's UNDEFINED -> SHADER_READ_ONLY barrier and the convert pass (luma +
  // interleaved chroma -> RGB) into that slot's picture. Ends with the picture in GENERAL for the
  // shared 超分 / letterbox tail.
  void RecordConvert(VkCommandBuffer cmd, const Frame& frame, uint32_t slot);

  VkImage pictureImage(uint32_t slot) const {
    return slot < kSlots ? pictures_[slot].image : VK_NULL_HANDLE;
  }
  VkImageView pictureView(uint32_t slot) const {
    return slot < kSlots ? pictures_[slot].view : VK_NULL_HANDLE;
  }

  std::string lastError() const { return error_; }

  void Destroy();

  // One picture per presenter frame slot so the next frame can be converted while the GPU still
  // samples the previous one (the same depth as VkRenderer's swapchain slots).
  static constexpr uint32_t kSlots = 2;

 private:
  // The picture the convert pass writes: image + view + its extent, rebuilt only on a size change.
  struct Picture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
  };

  // One codec buffer's import: image + memory + view + one descriptor set per presenter slot (each
  // binds that slot's frame-parameter buffer).
  struct Imported {
    uint32_t seq = 0;
    uint64_t lastUsed = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet sets[kSlots] = {};
    int chromaRow = 0;
    bool cbFirst = true;
    float colorRows[12] = {};
  };

  // Bounded so a platform that keeps handing out fresh buffers cannot grow the cache without limit;
  // the caller drains before a miss is served at the cap.
  static constexpr size_t kMaxImports = 8;

  bool EnsureConvertPipeline();
  void DestroyConvertPipeline();
  void ClearImports();
  // Re-allocates and re-writes every cached import's descriptor sets. Needed after resetting the
  // descriptor pool when an eviction has to reclaim a set; the images/views are unchanged.
  void RebuildSets();
  bool AllocateImportSet(VkImageView view, uint32_t slot, VkDescriptorSet* out);
  bool CreateImported(const void* nativeBuffer, const VideoFrameGeometry& geometry, Imported* out);
  void DestroyImported(Imported* imported);
  // The YUV -> RGB rows for a buffer colour space (falling back to the driver's suggestion), each
  // already scaled for 0..255 sample values: value = cY*luma + cCb*Cb + cCr*Cr + offset.
  void UpdateColorRows(int colorSpace, int suggestedModel, int suggestedRange, float* out);
  // Writes the frame parameters (visible box, crop origin, chroma row, chroma order, colour rows)
  // into the slot's uniform buffer the convert pipeline reads; std140, five vec4s.
  void WriteFrameParams(const VideoFrameGeometry& geometry, int chromaRow, const float* colorRows,
                        uint32_t slot);

  VkDeviceContext& context_;
  std::string error_;

  Picture pictures_[kSlots];
  std::vector<Imported> imports_;
  uint64_t useCounter_ = 0;

  // A failed Ensure is latched for that size: retrying every frame would allocate/destroy the
  // picture at the stream rate. A size change (a new stream) clears it.
  bool failed_ = false;
  int failedWidth_ = 0;
  int failedHeight_ = 0;

  // The negotiated expectation and the verdict on the buffers seen so far. Destroy clears the verdict
  // (every import decides it again) but keeps the expectation, which the session sets once per session
  // before any frame arrives.
  int expectedColorSpace_ = 0;
  bool colorMismatch_ = false;

  // One-shot: an import refused every frame would otherwise log at the stream rate, and the imported
  // buffer's shape only needs printing once.
  bool importWarned_ = false;
  bool importLogged_ = false;
  bool importFailed_ = false;
  bool derivedLayoutLogged_ = false;
  bool formatWarned_ = false;
  bool colorFallbackWarned_ = false;

  VkRenderPass convertRenderPass_ = VK_NULL_HANDLE;
  VkFramebuffer convertFramebuffer_[kSlots] = {};
  // Plain sampler (no YCbCr conversion): the pass fetches bytes with texelFetch.
  VkSampler byteSampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout convertSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout convertPipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool convertPool_ = VK_NULL_HANDLE;
  VkPipeline convertPipeline_ = VK_NULL_HANDLE;
  static constexpr size_t kFrameParamsBytes = 5 * 4 * sizeof(float);
  VkBuffer paramsBuffer_[kSlots] = {};
  VkDeviceMemory paramsMemory_[kSlots] = {};
  void* paramsMapped_[kSlots] = {};
  bool paramsCoherent_[kSlots] = {true, true};
};

}

#endif
