/*
 * HmRdp - Vulkan GFX surface engine (V2). See hmrdp_vk_desktop.h.
 */
#include "hmrdp_vk_desktop.h"

#include <freerdp/codec/region.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "hmrdp_log.h"
#include "hmrdp_vk_renderer.h"

// Generated at build time by cmake/EmbedSpirv.cmake (see CMakeLists.txt).
#include "rfx_compose.comp.h"
#include "rfx_decode.comp.h"

namespace hmrdp {
namespace {

// Progressive decode chunking, identical to the GLES engine (hmrdp_rfx.cpp §1):
// per-chunk scratch holds `kRfxChunkTiles * 3` component streams of 4096 int16.
constexpr uint32_t kRfxChunkTiles = 512;
constexpr uint32_t kMetaStride = 64;  // bytes per (tile,component) stream job

// YCbCr->BGRA fixed-point factors, matching FreeRDP prim_colors.c
// (general_yCbCrToRGB_16s8u_P3AC4R_BGRX, divisor 16) and hmrdp_rfx.cpp.
constexpr int32_t kKr = static_cast<int32_t>(1.402525f * (1 << 16));
constexpr int32_t kKcrG = static_cast<int32_t>(0.714401f * (1 << 16));
constexpr int32_t kKcbG = static_cast<int32_t>(0.343730f * (1 << 16));
constexpr int32_t kKcbB = static_cast<int32_t>(1.769905f * (1 << 16));

// One (tile,component) decode job, filled by the CPU container parser and
// consumed by the decode compute shader through the `meta` SSBO.
struct StreamJob {
  uint32_t type = 0;   // 0 = kFirst, 2 = kUpgrade
  uint32_t flags = 0;  // bit0 = RFX_TILE_DIFFERENCE (kFirst)
  uint32_t tileStream = 0;
  uint32_t payloadOff = 0;
  uint32_t payloadLen = 0;
  uint32_t srlOff = 0;
  uint32_t srlLen = 0;
  uint32_t rawOff = 0;
  uint32_t rawLen = 0;
  uint8_t shift[10] = {0};
  uint8_t newBit[10] = {0};
};

// uint8 nibbles [HL1 LH1 HH1 HL2 LH2 HH2 HL3 LH3 HH3 LL3] in the RfxQuant.
void QuantArray(const RfxQuant& q, uint8_t out[10]) {
  out[0] = q.HL1;
  out[1] = q.LH1;
  out[2] = q.HH1;
  out[3] = q.HL2;
  out[4] = q.LH2;
  out[5] = q.HH2;
  out[6] = q.HL3;
  out[7] = q.LH3;
  out[8] = q.HH3;
  out[9] = q.LL3;
}

// The screen is the only image the engine owns; surfaces / cache entries are
// persistent-mapped host-visible buffers (V2, VULKAN-TODO §4.2 item 2). It needs
// to be a transfer destination (fill / compose) and source (present blit).
//
// STORAGE is deliberately absent: with it set, the platform's Vulkan layer
// silently dropped every transfer to and from images (the 0xFF initialisation
// read back as all zeros).
constexpr VkImageUsageFlags kImageUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// Surfaces / cache need to be a transfer source and destination. STORAGE_BUFFER
// is included already because V3's Progressive compute shader writes tiles
// straight into these buffers (VULKAN-TODO §4.2 item 4), and adding it later
// would mean re-allocating every live surface.
constexpr VkBufferUsageFlags kSurfaceBufferUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

inline int Align16(int value) {
  return (value + 15) & ~15;
}

// Swaps the R and B bytes of one packed pixel. The engine images are BGRA8 when
// combined with FreeRDP's byte order; for an RGBA8 swapchain the CPU boundaries
// swizzle instead, so the renderer can still blit image-to-image.
inline uint32_t SwapRb(uint32_t pixel) {
  return (pixel & 0xFF00FF00u) | ((pixel & 0x000000FFu) << 16) | ((pixel & 0x00FF0000u) >> 16);
}

inline uint32_t GpuRd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint16_t GpuRd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

VkImageSubresourceLayers ColorLayers() {
  VkImageSubresourceLayers layers{};
  layers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  layers.mipLevel = 0;
  layers.baseArrayLayer = 0;
  layers.layerCount = 1;
  return layers;
}

// CPU pixel writes into a persistent-mapped surface buffer: direct, strided and
// free of staging (the whole point of the V2 storage change). These are the
// pixel commands FreeRDP's gdi path also performs on the CPU; only Compose (and
// later the Progressive decoder) touches the device.
void CpuFillRect(uint8_t* base, int stride, int left, int top, int width, int height,
                 uint32_t texel) {
  for (int y = 0; y < height; ++y) {
    uint32_t* row =
        reinterpret_cast<uint32_t*>(base + static_cast<size_t>(top + y) * stride) + left;
    for (int x = 0; x < width; ++x) {
      row[x] = texel;
    }
  }
}

void CpuCopyRows(const uint8_t* srcBase, int srcStride, int srcX, int srcY, uint8_t* dstBase,
                 int dstStride, int dstX, int dstY, int width, int height) {
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    const uint8_t* src =
        srcBase + static_cast<size_t>(srcY + y) * srcStride + static_cast<size_t>(srcX) * 4;
    uint8_t* dst =
        dstBase + static_cast<size_t>(dstY + y) * dstStride + static_cast<size_t>(dstX) * 4;
    std::memcpy(dst, src, rowBytes);
  }
}

}  // namespace

struct GfxVkDesktop::Impl {
  // A host-visible, persistently mapped linear buffer (V2 storage). `stride` is
  // the row pitch in bytes, exactly FreeRDP's scanline semantics, so the CPU can
  // address any pixel directly.
  struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    size_t capacity = 0;
    int width = 0;
    int height = 0;
    int stride = 0;

    bool valid() const { return buffer != VK_NULL_HANDLE && mapped != nullptr; }
  };

  struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;

    bool valid() const { return image != VK_NULL_HANDLE; }
  };

  // Persistent per-(tile,component) Progressive state (V3): `cur`/`sign` hold
  // 4096 int16 per (tile,component) stream and `bp` the 10 bit-position bytes.
  // They survive across messages and start zeroed for the surface's grid.
  struct RfxState {
    GpuBuffer cur;
    GpuBuffer sign;
    GpuBuffer bp;

    bool valid() const { return cur.valid(); }
  };

  struct Surface {
    GpuSurface meta;
    GpuBuffer gpu;
    RfxState rfx;
    // Tiles decoded in the current RDPGFX frame. FreeRDP's update_tiles
    // re-composites all of them (clipped by the current message's region rects)
    // on *every* Progressive message of the frame - PROGRESSIVE_SURFACE_CONTEXT::
    // numUpdatedTiles is reset only when the frame id changes - and the engine
    // has to mirror that (see DecodeProgressive). rameTileSeen dedups and is
    // sized gridSize.
    std::vector<uint32_t> frameTiles;
    std::vector<uint8_t> frameTileSeen;
    // Last RDPGFX frame id this surface saw (FreeRDP compares it the same way).
    uint32_t frameId = 0;
  };

  // A bitmap-cache slot. `gpu` is a grow-only allocation; `width`/`height` are
  // what the last SurfaceToCache actually stored, so a bigger slot that shrank
  // again is reused instead of re-allocated.
  struct CacheEntry {
    GpuBuffer gpu;
    int width = 0;
    int height = 0;
  };

  VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
  bool swapRb = false;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool recording = false;
  // Writes that later reads in the recorded command buffer must be ordered
  // after. Kept apart because the source stage differs: a CPU write to a mapped
  // buffer is HOST_WRITE, a vkCmdFillBuffer/vkCmdCopyBufferToImage write is
  // TRANSFER_WRITE (VULKAN-TODO §7.2 "CPU 写的表面 -> GPU 读").
  bool pendingHostWrites = false;
  bool pendingDeviceWrites = false;
  // A compute dispatch is recorded but has not been submitted yet. The surface
  // buffers are both GPU-written (Progressive compute) and CPU-accessed
  // (ClearCodec read-modify-write, bitmap cache, ReadSurface), so a CPU access
  // while a dispatch is only *recorded* would read the pre-dispatch pixels and
  // then be overwritten when the dispatch finally runs at the next Flush - the
  // exact ordering gdi does not have. SyncForCpuAccess() submits and waits first.
  bool computeInFlight = false;

  std::map<uint16_t, Surface> surfaces;
  std::map<uint16_t, CacheEntry> cache;
  GpuImage screen;

  // Screen initialisation / fill pattern: vkCmdFillBuffer writes the 32-bit
  // colour, then one vkCmdCopyBufferToImage paints it. The screen is the only
  // image left, so this is the only path that still needs a device-side fill.
  VkBuffer fillBuffer = VK_NULL_HANDLE;
  VkDeviceMemory fillMemory = VK_NULL_HANDLE;
  size_t fillCapacity = 0;

  // Screen readback staging (host-visible, mapped). Surfaces are read directly
  // from their own mapping, so no per-surface readback buffer is needed.
  VkBuffer readBuffer = VK_NULL_HANDLE;
  VkDeviceMemory readMemory = VK_NULL_HANDLE;
  void* readMapped = nullptr;
  size_t readCapacity = 0;

  // Overlap-safe staging for SurfaceToSurface (same-surface copies).
  std::vector<uint8_t> copyScratch;

  // --- Progressive decode (V3) --------------------------------------------
  // One compute pipeline per shader stage, the descriptor layouts they use and
  // the append-only CPU staging arena that feeds their SSBOs. All of it is
  // created once in Init(); if any part is unavailable `rfxReady` stays false and
  // Progressive keeps falling back to "unsupported" (counted, never silent).
  bool rfxReady = false;
  // ClearCodec (V4) stays on the CPU: FreeRDP's clear_decompress, applied
  // directly to the persistent-mapped surface. No GPU round trip is needed.
  std::unique_ptr<GfxClearDecoder> clearDecoder;
  VkDescriptorSetLayout decodeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout decodePipeLayout = VK_NULL_HANDLE;
  VkPipeline decodePipe = VK_NULL_HANDLE;
  VkDescriptorSetLayout composeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout composePipeLayout = VK_NULL_HANDLE;
  VkPipeline composePipe = VK_NULL_HANDLE;
  VkDescriptorPool rfxPool = VK_NULL_HANDLE;

  // Per-chunk decode scratch: `comp` then `temp`, kRfxChunkTiles*3 streams of
  // 4096 int16 entries (mirrors the GLES coef buffer).
  GpuBuffer coef;

  // A host-visible SSBO input arena. Buffers are kept and reused; their `used`
  // offset is rewound whenever a new command buffer starts (which only happens
  // after the previous submission's fence wait, so no in-flight reader remains).
  struct StageBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    size_t capacity = 0;
    size_t used = 0;
  };
  std::vector<StageBuffer> stageBuffers;
  size_t stageIndex = 0;
  bool pendingComputeWrites = false;

  // FreeRDP's WBT block state machine (persists across messages, like
  // progressive->state): a REGION outside FRAME_BEGIN..FRAME_END is ignored.
  RfxProgressiveState rfxState;
  uint64_t rfxFirstTiles = 0;
  uint64_t rfxUpgradeTiles = 0;
  uint64_t rfxChunks = 0;
  // Container diagnostics (mirrors RfxParseStats): what the capture actually
  // carries, so a pixel mismatch can be attributed to a codec path.
  uint64_t rfxRegions = 0;
  uint64_t rfxSimpleTiles = 0;
  uint64_t rfxDiffTiles = 0;
  uint64_t rfxNonExtrapolate = 0;
  uint64_t rfxSkippedTiles = 0;
  uint64_t rfxParseErrors = 0;
  // Progressive tiles re-composited because FreeRDP's update_tiles re-stamps the
  // whole frame's tile list on every message (see DecodeProgressive).
  uint64_t restamped = 0;
  uint64_t rfxMultiRegion = 0;
  // StartFrame PDUs whose frame id repeated (FreeRDP keeps its tile list then).
  uint64_t frameIdRepeats = 0;
  // Dev: how many commands were dropped because the engine had no such surface.
  uint32_t missingSurfaceLog = 0;
  uint64_t rfxOriginNonZero = 0;

  // Screen dirty rectangle (0xFF/0 initialised, mirrors GfxGpuDesktop).
  bool screenDirtyValid = false;
  int screenDirtyL = 0;
  int screenDirtyT = 0;
  int screenDirtyR = 0;
  int screenDirtyB = 0;

  // Coverage counters: "unsupported" = an unimplemented/failed codec command
  // was applied. The split counters say *which* codec is responsible
  // (ClearCodec is V4). Reported by Stats() so nothing fails silently (V5).
  uint64_t unsupportedCount = 0;
  uint64_t clearUnsupported = 0;
  uint64_t clearDecoded = 0;
  uint64_t progressiveFailed = 0;
  // Regions whose composite update_tiles rejected (see DecodeProgressive).
  uint64_t progressiveComposeSkipped = 0;
  uint64_t submits = 0;

  // Dev instrumentation: recording/CPU cost per command class, plus the
  // barrier/flush counts (a translation layer can make either dominate).
  struct OpStat {
    uint64_t us = 0;
    uint64_t count = 0;
  };
  OpStat statFill;
  OpStat statUpload;
  OpStat statCache;
  OpStat statCopy;
  OpStat statLifecycle;
  OpStat statProgressive;
  OpStat statCompose;
  OpStat statRead;
  uint64_t barriers = 0;
  uint64_t flushWaitUs = 0;
  // Compose decisions: distinguishes "nothing mapped" from "nothing dirty".
  uint64_t composeCopies = 0;
  uint64_t composeSkipUnmapped = 0;
  uint64_t composeSkipClean = 0;

  static int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // NOTE: no vkInvalidateMappedMemoryRanges before host reads. The buffers ask
  // for HOST_COHERENT, so the mapping is by definition coherent with device
  // writes and an explicit invalidate is redundant.
  static void AddStat(OpStat* stat, int64_t startUs) {
    stat->us += static_cast<uint64_t>(NowUs() - startUs);
    stat->count++;
  }

  // Resources whose last referencing submission may still be in flight. They are
  // released only after a fence wait: destroying them eagerly would either be a
  // use-after-free or force a submit+wait per command (VULKAN-TODO §7.2 "never
  // destroy in-flight resources", §7.3 "never wait per command").
  struct DeferredResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    bool mapped = false;
  };
  std::vector<DeferredResource> pendingDestroy;

  void DeferGpuBuffer(GpuBuffer* buffer) {
    if (buffer == nullptr) {
      return;
    }
    if (buffer->buffer != VK_NULL_HANDLE || buffer->memory != VK_NULL_HANDLE) {
      DeferredResource deferred;
      deferred.buffer = buffer->buffer;
      deferred.bufferMemory = buffer->memory;
      deferred.mapped = (buffer->mapped != nullptr);
      pendingDestroy.push_back(deferred);
    }
    *buffer = GpuBuffer{};
  }

  void ReleasePending() {
    if (pendingDestroy.empty()) {
      return;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    for (const DeferredResource& deferred : pendingDestroy) {
      if (deferred.image != VK_NULL_HANDLE && vk.DestroyImage != nullptr) {
        vk.DestroyImage(dev, deferred.image, nullptr);
      }
      if (deferred.imageMemory != VK_NULL_HANDLE && vk.FreeMemory != nullptr) {
        vk.FreeMemory(dev, deferred.imageMemory, nullptr);
      }
      if (deferred.buffer != VK_NULL_HANDLE && vk.DestroyBuffer != nullptr) {
        vk.DestroyBuffer(dev, deferred.buffer, nullptr);
      }
      if (deferred.bufferMemory != VK_NULL_HANDLE) {
        if (deferred.mapped && vk.UnmapMemory != nullptr) {
          vk.UnmapMemory(dev, deferred.bufferMemory);
        }
        if (vk.FreeMemory != nullptr) {
          vk.FreeMemory(dev, deferred.bufferMemory, nullptr);
        }
      }
    }
    pendingDestroy.clear();
  }

  VkDevice device() const { return VkContext::Instance().device(); }
  VkApi& api() const { return GetVkApi(); }

  Surface* Find(uint16_t id) {
    const auto it = surfaces.find(id);
    return it == surfaces.end() ? nullptr : &it->second;
  }
  const Surface* Find(uint16_t id) const {
    const auto it = surfaces.find(id);
    return it == surfaces.end() ? nullptr : &it->second;
  }

  static void MarkSurfaceDirty(Surface& s, int left, int top, int right, int bottom) {
    if (right <= left || bottom <= top) {
      return;
    }
    GpuSurface& m = s.meta;
    if (!m.dirtyValid) {
      m.dirtyValid = true;
      m.dirtyLeft = left;
      m.dirtyTop = top;
      m.dirtyRight = right;
      m.dirtyBottom = bottom;
      return;
    }
    if (left < m.dirtyLeft) m.dirtyLeft = left;
    if (top < m.dirtyTop) m.dirtyTop = top;
    if (right > m.dirtyRight) m.dirtyRight = right;
    if (bottom > m.dirtyBottom) m.dirtyBottom = bottom;
  }

  void MarkScreenDirty(int left, int top, int right, int bottom) {
    if (right <= left || bottom <= top) {
      return;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > screen.width) right = screen.width;
    if (bottom > screen.height) bottom = screen.height;
    if (right <= left || bottom <= top) {
      return;
    }
    if (!screenDirtyValid) {
      screenDirtyValid = true;
      screenDirtyL = left;
      screenDirtyT = top;
      screenDirtyR = right;
      screenDirtyB = bottom;
      return;
    }
    if (left < screenDirtyL) screenDirtyL = left;
    if (top < screenDirtyT) screenDirtyT = top;
    if (right > screenDirtyR) screenDirtyR = right;
    if (bottom > screenDirtyB) screenDirtyB = bottom;
  }

  // --- Resource helpers ----------------------------------------------------

  // Prefer DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT (the unified-memory case the
  // target device offers); fall back to plain host-visible. Never assume a pure
  // DEVICE_LOCAL type exists (VULKAN-TODO §7.2).
  uint32_t FindHostVisibleType(uint32_t typeBits) const {
    uint32_t type = VkContext::Instance().FindMemoryType(
        typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
      type = VkContext::Instance().FindMemoryType(
          typeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    return type;
  }

  // Allocates, binds and persistently maps a linear buffer of `bytes`.
  bool AllocateMappedBuffer(size_t bytes, VkBufferUsageFlags usage, VkBuffer* outBuffer,
                            VkDeviceMemory* outMemory, void** outMapped) {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || bytes == 0) {
      return false;
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vk.CreateBuffer(dev, &info, nullptr, &buffer) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    vk.GetBufferMemoryRequirements(dev, buffer, &req);
    const uint32_t type = FindHostVisibleType(req.memoryTypeBits);
    if (type == UINT32_MAX) {
      vk.DestroyBuffer(dev, buffer, nullptr);
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vk.AllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
      vk.DestroyBuffer(dev, buffer, nullptr);
      return false;
    }
    if (vk.BindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS) {
      vk.FreeMemory(dev, memory, nullptr);
      vk.DestroyBuffer(dev, buffer, nullptr);
      return false;
    }
    void* mapped = nullptr;
    if (vk.MapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
      vk.FreeMemory(dev, memory, nullptr);
      vk.DestroyBuffer(dev, buffer, nullptr);
      return false;
    }
    *outBuffer = buffer;
    *outMemory = memory;
    *outMapped = mapped;
    return true;
  }

  // A surface / cache-entry store: 16B-aligned stride, `stride * height` bytes.
  bool CreateGpuBuffer(GpuBuffer* out, int width, int height) {
    if (out == nullptr || width <= 0 || height <= 0) {
      return false;
    }
    const int stride = Align16(width * 4);
    const size_t bytes = static_cast<size_t>(stride) * height;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!AllocateMappedBuffer(bytes, kSurfaceBufferUsage, &buffer, &memory, &mapped)) {
      return false;
    }
    out->buffer = buffer;
    out->memory = memory;
    out->mapped = static_cast<uint8_t*>(mapped);
    out->capacity = bytes;
    out->width = width;
    out->height = height;
    out->stride = stride;
    return true;
  }

  void DestroyGpuBuffer(GpuBuffer* buffer) {
    if (buffer == nullptr) {
      return;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev != VK_NULL_HANDLE) {
      if (buffer->buffer != VK_NULL_HANDLE && vk.DestroyBuffer != nullptr) {
        vk.DestroyBuffer(dev, buffer->buffer, nullptr);
      }
      if (buffer->memory != VK_NULL_HANDLE) {
        if (buffer->mapped != nullptr && vk.UnmapMemory != nullptr) {
          vk.UnmapMemory(dev, buffer->memory);
        }
        if (vk.FreeMemory != nullptr) {
          vk.FreeMemory(dev, buffer->memory, nullptr);
        }
      }
    }
    *buffer = GpuBuffer{};
  }

  bool CreateImage(GpuImage* out, int width, int height) {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || width <= 0 || height <= 0) {
      return false;
    }
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
    info.usage = kImageUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vk.CreateImage(dev, &info, nullptr, &image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    vk.GetImageMemoryRequirements(dev, image, &req);
    uint32_t type = VkContext::Instance().FindMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
      type = VkContext::Instance().FindMemoryType(req.memoryTypeBits, 0);
    }
    if (type == UINT32_MAX) {
      vk.DestroyImage(dev, image, nullptr);
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vk.AllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
      vk.DestroyImage(dev, image, nullptr);
      return false;
    }
    if (vk.BindImageMemory(dev, image, memory, 0) != VK_SUCCESS) {
      vk.FreeMemory(dev, memory, nullptr);
      vk.DestroyImage(dev, image, nullptr);
      return false;
    }
    out->image = image;
    out->memory = memory;
    out->width = width;
    out->height = height;
    return true;
  }

  void DestroyImage(GpuImage* image) {
    if (image == nullptr) {
      return;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev != VK_NULL_HANDLE) {
      if (image->image != VK_NULL_HANDLE && vk.DestroyImage != nullptr) {
        vk.DestroyImage(dev, image->image, nullptr);
      }
      if (image->memory != VK_NULL_HANDLE && vk.FreeMemory != nullptr) {
        vk.FreeMemory(dev, image->memory, nullptr);
      }
    }
    *image = GpuImage{};
  }

  // Grow-on-demand mapped scratch for the screen readback. Rounded up so small
  // sizes do not reallocate on every read.
  bool EnsureReadBuffer(size_t bytes) {
    if (bytes == 0) {
      return false;
    }
    if (readBuffer != VK_NULL_HANDLE && readCapacity >= bytes) {
      return true;
    }
    if (readBuffer != VK_NULL_HANDLE) {
      DeferredResource deferred;
      deferred.buffer = readBuffer;
      deferred.bufferMemory = readMemory;
      deferred.mapped = (readMapped != nullptr);
      pendingDestroy.push_back(deferred);
      readBuffer = VK_NULL_HANDLE;
      readMemory = VK_NULL_HANDLE;
      readMapped = nullptr;
      readCapacity = 0;
    }
    const size_t want = (bytes + (1u << 20) - 1u) & ~((1u << 20) - 1u);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!AllocateMappedBuffer(want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              &buffer, &memory, &mapped)) {
      return false;
    }
    readBuffer = buffer;
    readMemory = memory;
    readMapped = mapped;
    readCapacity = want;
    return true;
  }

  // Device-side scratch for the screen's initial fill (never mapped: it is
  // written by vkCmdFillBuffer and read by vkCmdCopyBufferToImage).
  bool EnsureFillBuffer(size_t bytes) {
    if (bytes == 0) {
      return false;
    }
    if (fillBuffer != VK_NULL_HANDLE && fillCapacity >= bytes) {
      return true;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE) {
      return false;
    }
    if (fillBuffer != VK_NULL_HANDLE) {
      DeferredResource deferred;
      deferred.buffer = fillBuffer;
      deferred.bufferMemory = fillMemory;
      pendingDestroy.push_back(deferred);
      fillBuffer = VK_NULL_HANDLE;
      fillMemory = VK_NULL_HANDLE;
      fillCapacity = 0;
    }
    const size_t want = (bytes + (1u << 20) - 1u) & ~((1u << 20) - 1u);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = want;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer created = VK_NULL_HANDLE;
    if (vk.CreateBuffer(dev, &info, nullptr, &created) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    vk.GetBufferMemoryRequirements(dev, created, &req);
    uint32_t type = VkContext::Instance().FindMemoryType(req.memoryTypeBits,
                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
      type = VkContext::Instance().FindMemoryType(req.memoryTypeBits, 0);
    }
    if (type == UINT32_MAX) {
      vk.DestroyBuffer(dev, created, nullptr);
      return false;
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vk.AllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
      vk.DestroyBuffer(dev, created, nullptr);
      return false;
    }
    if (vk.BindBufferMemory(dev, created, memory, 0) != VK_SUCCESS) {
      vk.FreeMemory(dev, memory, nullptr);
      vk.DestroyBuffer(dev, created, nullptr);
      return false;
    }
    fillBuffer = created;
    fillMemory = memory;
    fillCapacity = want;
    return true;
  }

  // --- Command buffer ------------------------------------------------------

  bool EnsureRecording() {
    if (recording) {
      return true;
    }
    VkApi& vk = api();
    if (vk.ResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
      return false;
    }
    // A new command buffer only starts after the previous submission's fence
    // wait, so the staging arena can be rewound and the descriptor pool reset:
    // neither is referenced by any in-flight command buffer.
    ResetStage();
    if (rfxPool != VK_NULL_HANDLE && vk.ResetDescriptorPool != nullptr) {
      vk.ResetDescriptorPool(device(), rfxPool, 0);
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.BeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) {
      return false;
    }
    recording = true;
    return true;
  }

  // Makes every prior write visible to every later read. Deliberately coarse:
  // correctness over barrier count (VULKAN-TODO §7.2). The host leg is required
  // because surface pixels are written by the CPU directly into the mapping.
  // Emitted lazily, never a per-command device wait.
  void BarrierBeforeRead() {
    if (!pendingHostWrites && !pendingDeviceWrites && !pendingComputeWrites) {
      return;
    }
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    VkPipelineStageFlags srcStage = 0;
    if (pendingHostWrites) {
      barrier.srcAccessMask |= VK_ACCESS_HOST_WRITE_BIT;
      srcStage |= VK_PIPELINE_STAGE_HOST_BIT;
    }
    if (pendingDeviceWrites) {
      barrier.srcAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
      srcStage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (pendingComputeWrites) {
      barrier.srcAccessMask |= VK_ACCESS_SHADER_WRITE_BIT;
      srcStage |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    api().CmdPipelineBarrier(commandBuffer, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
    pendingHostWrites = false;
    pendingDeviceWrites = false;
    pendingComputeWrites = false;
    barriers++;
  }

  void HostWrote() { pendingHostWrites = true; }

  // Makes the mapped surface/cache buffers safe to touch on the CPU: a recorded
  // compute dispatch (Progressive decode) has not executed yet, so the mapping
  // still holds the *previous* pixels. Submitting and waiting (Flush also emits
  // the COMPUTE -> HOST barrier) puts the mapping in the same state gdi's surface
  // has when it performs a CPU read-modify-write. No-op when nothing is pending.
  bool SyncForCpuAccess() {
    // Progressive decode/compose is *recorded* and only executes on Flush(), and a
    // CPU access to a mapped surface must see its result; a Flush with nothing
    // recorded is nearly free, so this only needs to run when compute is in
    // flight (measured: making it unconditional changes no metric, it only adds
    // sync points - see VULKAN-TODO §7.3).
    if (!computeInFlight) {
      return true;
    }
    return Flush();
  }

  // UNDEFINED -> GENERAL. Explicit so no command relies on an implicit
  // transition (the image contents are meaningless either way).
  void TransitionToGeneral(VkImage image) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    api().CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &barrier);
  }

  bool Flush() {
    if (!recording) {
      return true;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    const int64_t submitStart = NowUs();
    recording = false;
    if (pendingDeviceWrites || computeInFlight) {
      // Make device writes visible to the *host* before the command buffer ends.
      // This is the spec-mandated dependency for a CPU read of device-written
      // memory (dstStage HOST / dstAccess HOST_READ); most drivers do not insist
      // on it, the platform layer here does. Compute writes (Progressive) need
      // the same leg: the surface mapping is read back by ClearCodec/bitmap cache.
      VkMemoryBarrier toHost{};
      toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      toHost.srcAccessMask = 0;
      VkPipelineStageFlags toHostStage = 0;
      if (pendingDeviceWrites) {
        toHost.srcAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
        toHostStage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
      }
      if (computeInFlight) {
        toHost.srcAccessMask |= VK_ACCESS_SHADER_WRITE_BIT;
        toHostStage |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      }
      toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vk.CmdPipelineBarrier(commandBuffer, toHostStage, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost,
                            0, nullptr, 0, nullptr);
    }
    if (vk.EndCommandBuffer(commandBuffer) != VK_SUCCESS) {
      return false;
    }
    // pendingDeviceWrites deliberately stays set: the next command buffer is a
    // separate submission, and a read in it needs its own barrier (the fence wait
    // alone was not enough on the platform layer - a copy that followed a
    // readback silently read stale data). pendingHostWrites is untouched: it is
    // only cleared by a barrier that actually ordered a host write before a
    // device read, so a CPU write not yet consumed by the GPU stays pending.
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    vk.ResetFences(dev, 1, &fence);
    if (vk.QueueSubmit(VkContext::Instance().queue(), 1, &submit, fence) != VK_SUCCESS) {
      return false;
    }
    if (vk.WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
    ++submits;
    // Conservative: whatever was written is now complete, but a barrier is still
    // emitted before the next command buffer's first read. The compute writes
    // have executed too, so a following CPU access sees the real pixels.
    pendingDeviceWrites = true;
    computeInFlight = false;
    flushWaitUs += static_cast<uint64_t>(NowUs() - submitStart);
    // The queue is idle now, so everything deferred while recording is safe to
    // destroy (and we are already recording a fresh command buffer).
    ReleasePending();
    return true;
  }

  // Fills a whole image with one 32-bit texel, through vkCmdFillBuffer +
  // vkCmdCopyBufferToImage. vkCmdClearColorImage is deliberately not used: this
  // platform's Vulkan layer silently produced zeros for it and vkCmdBlitImage
  // outright crashes in it. Only transfer commands have proven reliable.
  bool FillImage(GpuImage* image, uint32_t texel) {
    if (image == nullptr || !image->valid()) {
      return false;
    }
    const size_t bytes = static_cast<size_t>(image->width) * image->height * 4;
    if (!EnsureFillBuffer(bytes) || !EnsureRecording()) {
      return false;
    }
    api().CmdFillBuffer(commandBuffer, fillBuffer, 0, static_cast<VkDeviceSize>(bytes), texel);
    pendingDeviceWrites = true;
    BarrierBeforeRead();
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = static_cast<uint32_t>(image->width);
    region.bufferImageHeight = static_cast<uint32_t>(image->height);
    region.imageSubresource = ColorLayers();
    region.imageExtent.width = static_cast<uint32_t>(image->width);
    region.imageExtent.height = static_cast<uint32_t>(image->height);
    region.imageExtent.depth = 1;
    api().CmdCopyBufferToImage(commandBuffer, fillBuffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &region);
    pendingDeviceWrites = true;
    return true;
  }

  bool PrepareImage(GpuImage* image, int width, int height) {
    if (!CreateImage(image, width, height)) {
      return false;
    }
    if (!EnsureRecording()) {
      DestroyImage(image);
      return false;
    }
    TransitionToGeneral(image->image);
    return true;
  }

  // Compose: copies the clipped dirty rect out of a persistently mapped surface
  // buffer into the screen image. `bufferRowLength` carries the surface stride,
  // so the desktop size does not have to be a multiple of anything.
  bool CopyBufferRegionToScreen(const GpuBuffer& src, int srcX, int srcY, int dstX, int dstY,
                                int width, int height) {
    if (width <= 0 || height <= 0) {
      return true;
    }
    if (!src.valid() || screen.image == VK_NULL_HANDLE) {
      return false;
    }
    if (!EnsureRecording()) {
      return false;
    }
    BarrierBeforeRead();
    VkBufferImageCopy region{};
    region.bufferOffset =
        static_cast<VkDeviceSize>(srcY) * static_cast<VkDeviceSize>(src.stride) +
        static_cast<VkDeviceSize>(srcX) * 4;
    region.bufferRowLength = static_cast<uint32_t>(src.stride / 4);
    region.bufferImageHeight = 0;
    region.imageSubresource = ColorLayers();
    region.imageOffset = {dstX, dstY, 0};
    region.imageExtent.width = static_cast<uint32_t>(width);
    region.imageExtent.height = static_cast<uint32_t>(height);
    region.imageExtent.depth = 1;
    api().CmdCopyBufferToImage(commandBuffer, src.buffer, screen.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &region);
    pendingDeviceWrites = true;
    return true;
  }

  // --- Progressive decode helpers -----------------------------------------

  // A mapped host-visible buffer of an arbitrary byte size (Progressive state
  // and scratch). The GpuBuffer stride fields are unused here.
  bool CreateRawBuffer(GpuBuffer* out, size_t bytes) {
    if (out == nullptr || bytes == 0) {
      return false;
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!AllocateMappedBuffer(bytes, kSurfaceBufferUsage, &buffer, &memory, &mapped)) {
      return false;
    }
    out->buffer = buffer;
    out->memory = memory;
    out->mapped = static_cast<uint8_t*>(mapped);
    out->capacity = bytes;
    out->width = 0;
    out->height = 0;
    out->stride = 0;
    return true;
  }

  bool GrowStage(size_t minBytes) {
    size_t want = minBytes + 4096;
    if (want < (1u << 20)) {
      want = 1u << 20;
    }
    StageBuffer sb;
    if (!AllocateMappedBuffer(
            want, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            &sb.buffer, &sb.memory, reinterpret_cast<void**>(&sb.mapped))) {
      return false;
    }
    sb.capacity = want;
    sb.used = 0;
    stageBuffers.push_back(sb);
    return true;
  }

  // Copies `bytes` into the arena and reports the backing buffer plus offset. A
  // new arena buffer is allocated only when the current one is full, so an
  // already-recorded descriptor never points at a destroyed buffer.
  bool StageAppend(const void* data, size_t bytes, VkBuffer* outBuffer, VkDeviceSize* outOffset) {
    if (bytes == 0) {
      *outBuffer = VK_NULL_HANDLE;
      *outOffset = 0;
      return true;
    }
    constexpr size_t kAlign = 256;
    for (;;) {
      if (stageIndex >= stageBuffers.size()) {
        if (!GrowStage(bytes)) {
          return false;
        }
      }
      StageBuffer& sb = stageBuffers[stageIndex];
      const size_t off = (sb.used + kAlign - 1) & ~(kAlign - 1);
      if (off + bytes <= sb.capacity) {
        std::memcpy(sb.mapped + off, data, bytes);
        sb.used = off + bytes;
        // The shader reads this through an SSBO: it is a host write that the
        // next dispatch needs an availability/visibility barrier for.
        pendingHostWrites = true;
        *outBuffer = sb.buffer;
        *outOffset = static_cast<VkDeviceSize>(off);
        return true;
      }
      stageIndex++;
    }
  }

  void ResetStage() {
    stageIndex = 0;
    for (StageBuffer& sb : stageBuffers) {
      sb.used = 0;
    }
  }

  void DestroyStage() {
    VkApi& vk = api();
    const VkDevice dev = device();
    for (StageBuffer& sb : stageBuffers) {
      if (dev != VK_NULL_HANDLE) {
        if (sb.buffer != VK_NULL_HANDLE && vk.DestroyBuffer != nullptr) {
          vk.DestroyBuffer(dev, sb.buffer, nullptr);
        }
        if (sb.memory != VK_NULL_HANDLE) {
          if (sb.mapped != nullptr && vk.UnmapMemory != nullptr) {
            vk.UnmapMemory(dev, sb.memory);
          }
          if (vk.FreeMemory != nullptr) {
            vk.FreeMemory(dev, sb.memory, nullptr);
          }
        }
      }
    }
    stageBuffers.clear();
    stageIndex = 0;
  }

  bool CreateRfxPipelines() {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (vk.CreateShaderModule == nullptr || vk.CreateComputePipelines == nullptr ||
        vk.CmdPushConstants == nullptr || vk.ResetDescriptorPool == nullptr) {
      return false;
    }

    VkDescriptorSetLayoutBinding decodeBindings[6] = {};
    for (int i = 0; i < 6; ++i) {
      decodeBindings[i].binding = static_cast<uint32_t>(i);
      decodeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      decodeBindings[i].descriptorCount = 1;
      decodeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo decodeLayoutInfo{};
    decodeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    decodeLayoutInfo.bindingCount = 6;
    decodeLayoutInfo.pBindings = decodeBindings;
    if (vk.CreateDescriptorSetLayout(dev, &decodeLayoutInfo, nullptr, &decodeSetLayout) !=
        VK_SUCCESS) {
      return false;
    }

    VkDescriptorSetLayoutBinding composeBindings[4] = {};
    for (int i = 0; i < 4; ++i) {
      composeBindings[i].binding = static_cast<uint32_t>(i);
      composeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      composeBindings[i].descriptorCount = 1;
      composeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo composeLayoutInfo{};
    composeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    composeLayoutInfo.bindingCount = 4;
    composeLayoutInfo.pBindings = composeBindings;
    if (vk.CreateDescriptorSetLayout(dev, &composeLayoutInfo, nullptr, &composeSetLayout) !=
        VK_SUCCESS) {
      return false;
    }

    VkPushConstantRange decodeRange{};
    decodeRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    decodeRange.offset = 0;
    decodeRange.size = 3 * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo decodePipeInfo{};
    decodePipeInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    decodePipeInfo.setLayoutCount = 1;
    decodePipeInfo.pSetLayouts = &decodeSetLayout;
    decodePipeInfo.pushConstantRangeCount = 1;
    decodePipeInfo.pPushConstantRanges = &decodeRange;
    if (vk.CreatePipelineLayout(dev, &decodePipeInfo, nullptr, &decodePipeLayout) != VK_SUCCESS) {
      return false;
    }

    VkPushConstantRange composeRange{};
    composeRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    composeRange.offset = 0;
    composeRange.size = 9 * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo composePipeInfo{};
    composePipeInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    composePipeInfo.setLayoutCount = 1;
    composePipeInfo.pSetLayouts = &composeSetLayout;
    composePipeInfo.pushConstantRangeCount = 1;
    composePipeInfo.pPushConstantRanges = &composeRange;
    if (vk.CreatePipelineLayout(dev, &composePipeInfo, nullptr, &composePipeLayout) != VK_SUCCESS) {
      return false;
    }

    auto makePipeline = [&](const uint32_t* words, uint32_t count, VkPipelineLayout layout,
                            VkPipeline* out) -> bool {
      VkShaderModuleCreateInfo moduleInfo{};
      moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      moduleInfo.codeSize = static_cast<size_t>(count) * sizeof(uint32_t);
      moduleInfo.pCode = words;
      VkShaderModule module = VK_NULL_HANDLE;
      if (vk.CreateShaderModule(dev, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        return false;
      }
      VkPipelineShaderStageCreateInfo stage{};
      stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      stage.module = module;
      stage.pName = "main";
      VkComputePipelineCreateInfo pipelineInfo{};
      pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      pipelineInfo.stage = stage;
      pipelineInfo.layout = layout;
      const VkResult result =
          vk.CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, out);
      vk.DestroyShaderModule(dev, module, nullptr);
      return result == VK_SUCCESS;
    };
    if (!makePipeline(kRfxDecodeSpv, kRfxDecodeSpvWords, decodePipeLayout, &decodePipe)) {
      return false;
    }
    if (!makePipeline(kRfxComposeSpv, kRfxComposeSpvWords, composePipeLayout, &composePipe)) {
      return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 4096;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1024;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vk.CreateDescriptorPool(dev, &poolInfo, nullptr, &rfxPool) != VK_SUCCESS) {
      return false;
    }
    return true;
  }

  bool AllocSet(VkDescriptorSetLayout layout, VkDescriptorSet* out) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = rfxPool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    return api().AllocateDescriptorSets(device(), &info, out) == VK_SUCCESS;
  }

  void WriteBuffer(VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize offset,
                   VkDeviceSize range) {
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer;
    bufferInfo.offset = offset;
    bufferInfo.range = range;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    api().UpdateDescriptorSets(device(), 1, &write, 0, nullptr);
  }

  // Host/transfer/compute writes -> compute reads. Emitted before a dispatch so
  // the staging data (CPU) and any previous dispatch's output are visible.
  void BarrierBeforeCompute() {
    if (!pendingHostWrites && !pendingDeviceWrites && !pendingComputeWrites) {
      return;
    }
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    VkPipelineStageFlags srcStage = 0;
    if (pendingHostWrites) {
      barrier.srcAccessMask |= VK_ACCESS_HOST_WRITE_BIT;
      srcStage |= VK_PIPELINE_STAGE_HOST_BIT;
    }
    if (pendingDeviceWrites) {
      barrier.srcAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
      srcStage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (pendingComputeWrites) {
      barrier.srcAccessMask |= VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      srcStage |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    api().CmdPipelineBarrier(commandBuffer, srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
    pendingHostWrites = false;
    pendingDeviceWrites = false;
    pendingComputeWrites = false;
    barriers++;
  }

  bool CreateRfxState(RfxState* state, int gridW, int gridH) {
    if (state == nullptr || gridW <= 0 || gridH <= 0) {
      return false;
    }
    const size_t gridStreams = static_cast<size_t>(gridW) * static_cast<size_t>(gridH) * 3u;
    const size_t stateCoefBytes = gridStreams * 4096u * 2u;
    // 12 bytes per stream (not 10): see kBitPosStride in rfx_decode.comp - a
    // 10-byte stride lets adjacent streams share a 32-bit word and lose
    // read-modify-write updates.
    const size_t stateBpBytes = gridStreams * 12u;
    RfxState created;
    if (!CreateRawBuffer(&created.cur, stateCoefBytes) ||
        !CreateRawBuffer(&created.sign, stateCoefBytes) ||
        !CreateRawBuffer(&created.bp, stateBpBytes)) {
      DestroyGpuBuffer(&created.cur);
      DestroyGpuBuffer(&created.sign);
      DestroyGpuBuffer(&created.bp);
      return false;
    }
    std::memset(created.cur.mapped, 0, stateCoefBytes);
    std::memset(created.sign.mapped, 0, stateCoefBytes);
    std::memset(created.bp.mapped, 0, stateBpBytes);
    HostWrote();
    *state = created;
    return true;
  }

  // Decodes one Progressive ("WBT") message into the surface buffer: parse the
  // container on the CPU, then for each chunk of <=512 tiles run the decode then
  // the YCbCr compose compute dispatch. Mirrors GfxGpuDesktop::DecodeMessage.
  bool DecodeProgressive(uint16_t surfaceId, const uint8_t* payload, size_t size, int originX,
                         int originY) {
    if (!rfxReady || payload == nullptr || size == 0) {
      return false;
    }
    if (originX != 0 || originY != 0) {
      rfxOriginNonZero++;
    }
    Surface* surface = Find(surfaceId);
    if (surface == nullptr || !surface->gpu.valid() || !surface->rfx.valid()) {
      return false;
    }
    const int gridW = surface->meta.gridW;
    const int gridH = surface->meta.gridH;
    const int surfaceW = surface->meta.width;
    const int surfaceH = surface->meta.height;

    struct TileJob {
      uint32_t x = 0;
      uint32_t y = 0;
      uint32_t rectOffset = 0;
      uint32_t rectCount = 0;
      StreamJob streams[3];
    };
    std::vector<TileJob> tiles;
    tiles.reserve(256);
    std::vector<uint32_t> rectPool;
    rectPool.reserve(256);
    // This message's region rects (absolute, device pixels) - the clip its
    // composite uses, shared by every tile of the region.
    // The message-wide compose clip: FreeRDP's clippingRects (the band-merged
    // union of the region's rects), as x,y,w,h quadruples.
    std::vector<int32_t> msgRects;
    // Tiles this message decodes, in message order.
    std::vector<uint32_t> msgTiles;
    // Message-wide compose clip (FreeRDP's `clippingRects`, merged - see the
    // callback) and its rect count.
    bool msgClipReady = false;
    uint32_t msgClipCount = 0;
    RfxParseStats stats;
    const bool parsed = ParseRfxProgressive(
        payload, size,
        [&](const RfxTileRef& t) {
          if (t.quants == nullptr) {
            return;
          }
          const bool upgrade = (t.type == RfxTileType::kUpgrade);
          if (!upgrade && t.type != RfxTileType::kFirst) {
            rfxSkippedTiles++;  // kSimple not used by this server
            return;
          }
          if (t.xIdx >= static_cast<uint32_t>(gridW) ||
              t.yIdx >= static_cast<uint32_t>(gridH)) {
            rfxSkippedTiles++;  // tile outside this surface's grid
            return;
          }
          TileJob job;
          job.x = t.xIdx;
          job.y = t.yIdx;
          // The compose clip is FreeRDP's `clippingRects`: update_tiles builds it
          // by unioning the region's rects with region16_union_rect(), which is a
          // *band/coalescing* union - merging items that overlap a band into one
          // (bounding-box) rect, so it also covers the gaps between them. Compositing
          // with the raw rects instead (as the engine used to) misses exactly those
          // gap pixels. Build the same region with FreeRDP's own code, once per
          // message (all tiles of a region share its rects).
          if (!msgClipReady) {
            REGION16 clip;
            region16_init(&clip);
            for (uint16_t ri = 0; ri < t.numRects; ++ri) {
              const RfxRect& r = t.rects[ri];
              // Region rects are relative to the command's destRect origin,
              // exactly like FreeRDP's gdi (update_tiles:
              // clippingRect.left = nXDst + rect->x).
              RECTANGLE_16 cr;
              cr.left = static_cast<UINT16>(r.x + originX);
              cr.top = static_cast<UINT16>(r.y + originY);
              cr.right = static_cast<UINT16>(cr.left + r.width);
              cr.bottom = static_cast<UINT16>(cr.top + r.height);
              region16_union_rect(&clip, &clip, &cr);
            }
            UINT32 mergedCount = 0;
            const RECTANGLE_16* merged = region16_rects(&clip, &mergedCount);
            for (UINT32 i = 0; i < mergedCount; ++i) {
              msgRects.push_back(merged[i].left);
              msgRects.push_back(merged[i].top);
              msgRects.push_back(static_cast<int32_t>(merged[i].right - merged[i].left));
              msgRects.push_back(static_cast<int32_t>(merged[i].bottom - merged[i].top));
            }
            region16_uninit(&clip);
            // One shared clip for every tile: the rect pool is the same slice for
            // all of them, so it is built once below.
            for (size_t i = 0; i + 1 < msgRects.size(); i += 4) {
              const uint32_t rx = static_cast<uint32_t>(msgRects[i]);
              const uint32_t ry = static_cast<uint32_t>(msgRects[i + 1]);
              rectPool.push_back(rx | (ry << 16));
              rectPool.push_back(static_cast<uint32_t>(msgRects[i + 2]) |
                                 (static_cast<uint32_t>(msgRects[i + 3]) << 16));
            }
            msgClipCount = static_cast<uint32_t>(msgRects.size() / 4);
            msgClipReady = true;
          }
          job.rectOffset = 0;
          job.rectCount = msgClipCount;
          const RfxQuant* qv[3] = {&t.quants[t.quantIdxY], &t.quants[t.quantIdxCb],
                                   &t.quants[t.quantIdxCr]};
          RfxQuant prog[3];
          if (t.quality != 0xFF && t.progQuants != nullptr && t.quality < t.numProgQuant) {
            prog[0] = t.progQuants[t.quality].y;
            prog[1] = t.progQuants[t.quality].cb;
            prog[2] = t.progQuants[t.quality].cr;
          }
          const uint8_t* data[3] = {t.yData, t.cbData, t.crData};
          const uint16_t len[3] = {t.yLen, t.cbLen, t.crLen};
          const uint8_t* srl[3] = {t.ySrlData, t.cbSrlData, t.crSrlData};
          const uint16_t srlLen[3] = {t.ySrlLen, t.cbSrlLen, t.crSrlLen};
          const uint8_t* raw[3] = {t.yRawData, t.cbRawData, t.crRawData};
          const uint16_t rawLen[3] = {t.yRawLen, t.cbRawLen, t.crRawLen};
          const uint32_t tileIndex = static_cast<uint32_t>(t.yIdx) * static_cast<uint32_t>(gridW) +
                                     t.xIdx;
          msgTiles.push_back(tileIndex);
          for (int c = 0; c < 3; ++c) {
            uint8_t qa[10];
            uint8_t pa[10];
            QuantArray(*qv[c], qa);
            QuantArray(prog[c], pa);
            StreamJob& sj = job.streams[c];
            sj.type = upgrade ? 2u : 0u;
            sj.flags = t.flags & 1u;
            sj.tileStream = tileIndex * 3u + static_cast<uint32_t>(c);
            for (int i = 0; i < 10; ++i) {
              const int nb = static_cast<int>(qa[i]) + static_cast<int>(pa[i]);
              sj.newBit[i] = static_cast<uint8_t>(nb);
              const int sh = nb - 1;
              sj.shift[i] = static_cast<uint8_t>(sh < 0 ? 0 : sh);
            }
            if (upgrade) {
              sj.srlOff = static_cast<uint32_t>(srl[c] - payload);
              sj.srlLen = srlLen[c];
              sj.rawOff = static_cast<uint32_t>(raw[c] - payload);
              sj.rawLen = rawLen[c];
            } else {
              sj.payloadOff = static_cast<uint32_t>(data[c] - payload);
              sj.payloadLen = len[c];
            }
          }
          tiles.push_back(job);
        },
        &stats, &rfxState);
    if (!parsed) {
      // FreeRDP rejects the whole message on a malformed / invalid region header
      // (tileSize, numRects < 1, numQuant > 7, quant nibbles outside [6,15], ...)
      // and decodes nothing at all - including the per-tile state. Decoding the
      // tiles that happened to parse would make the surface diverge from gdi.
      rfxParseErrors += stats.errors;
      return true;
    }
    rfxRegions += stats.regions;
    // A message normally carries exactly one REGION block; when it carries more,
    // FreeRDP's update_tiles clips *every* tile with the rects of the LAST one
    // (`region->rects` is overwritten per block, and update_tiles runs after the
    // whole message was parsed). The engine clips each tile with its own region's
    // rects, so such messages diverge.
    if (stats.regions > 1) {
      rfxMultiRegion++;
    }
    rfxSimpleTiles += stats.simpleTiles;
    rfxDiffTiles += stats.diffTiles;
    rfxNonExtrapolate += (stats.regions > stats.extrapolateRegions)
                             ? (stats.regions - stats.extrapolateRegions)
                             : 0u;
    rfxParseErrors += stats.errors;
    // FreeRDP's update_tiles re-composites *every* tile decoded in the current
    // frame on each Progressive message, clipped by that message's region rects
    // (PROGRESSIVE_SURFACE_CONTEXT::numUpdatedTiles is reset only when the frame
    // id changes). Mirror it: register this message's tiles in the frame list and
    // prepend one "reverse" job (DWT from the persistent `cur` coefficients, see
    // type 3 in rfx_decode.comp) for every earlier frame tile whose rect
    // intersects this message's clips. Tiles this message decodes itself are
    // skipped - it composites them with their fresh coefficients anyway.
    const size_t gridSize = static_cast<size_t>(gridW) * static_cast<size_t>(gridH);
    if (surface->frameTileSeen.size() != gridSize) {
      surface->frameTileSeen.assign(gridSize, 0);
      surface->frameTiles.clear();
    }
    for (const uint32_t idx : msgTiles) {
      if (idx < gridSize && surface->frameTileSeen[idx] == 0) {
        surface->frameTileSeen[idx] = 1;
        surface->frameTiles.push_back(idx);
      }
    }
    const uint32_t msgRectCount = static_cast<uint32_t>(msgRects.size() / 4);
    std::vector<TileJob> restamps;
    if (msgRectCount > 0 && !surface->frameTiles.empty()) {
      for (const uint32_t idx : surface->frameTiles) {
        if (idx >= gridSize) {
          continue;
        }
        bool isMsgTile = false;
        for (const uint32_t m : msgTiles) {
          if (m == idx) {
            isMsgTile = true;
            break;
          }
        }
        if (isMsgTile) {
          continue;
        }
        const int tx = originX + static_cast<int>(idx % static_cast<uint32_t>(gridW)) * 64;
        const int ty = originY + static_cast<int>(idx / static_cast<uint32_t>(gridW)) * 64;
        bool hit = false;
        for (uint32_t ri = 0; ri < msgRectCount && !hit; ++ri) {
          const int32_t rx = msgRects[ri * 4];
          const int32_t ry = msgRects[ri * 4 + 1];
          const int32_t rw = msgRects[ri * 4 + 2];
          const int32_t rh = msgRects[ri * 4 + 3];
          if (tx < rx + rw && tx + 64 > rx && ty < ry + rh && ty + 64 > ry) {
            hit = true;
          }
        }
        if (!hit) {
          continue;
        }
        restamped++;
        TileJob job;
        job.x = idx % static_cast<uint32_t>(gridW);
        job.y = idx / static_cast<uint32_t>(gridW);
        job.rectOffset = 0;
        job.rectCount = msgRectCount;
        for (int c = 0; c < 3; ++c) {
          job.streams[c].type = 3u;  // reverse: rebuild the tile from `cur`
          job.streams[c].tileStream = idx * 3u + static_cast<uint32_t>(c);
        }
        restamps.push_back(job);
      }
    }
    if (!restamps.empty()) {
      // Earlier frame tiles first, this message's own tiles last (FreeRDP
      // composites in updatedTileIndices order, i.e. the same order).
      tiles.insert(tiles.begin(), restamps.begin(), restamps.end());
    }
    if (tiles.empty()) {
      return true;
    }
    if (!EnsureRecording()) {
      return false;
    }
    // FreeRDP's update_tiles composites a tile only inside
    // region16_intersect(clippingRects, tileRect) and *fails the whole region*
    // (composites nothing at all) as soon as one of those intersection rects
    // leaves the surface:
    //     if (rect->left + width > surface->width) goto fail;
    //     if (rect->top + height > surface->height) goto fail;
    // The intersection is a subset of the tile, so this triggers for every region
    // that touches the right/bottom tile column whenever the desktop size is not
    // a multiple of 64 - which is the normal case. The tile state has already
    // advanced by then, so the engine must still decode (state) but must not
    // composite any pixel of the region.
    bool composeRegion = true;
    for (const TileJob& job : tiles) {
      const int tx = originX + static_cast<int>(job.x) * 64;
      const int ty = originY + static_cast<int>(job.y) * 64;
      for (uint32_t ri = 0; ri < job.rectCount; ++ri) {
        const size_t wi = static_cast<size_t>(job.rectOffset + ri) * 2;
        if (wi + 1 >= rectPool.size()) {
          break;
        }
        const uint32_t w0 = rectPool[wi];
        const uint32_t w1 = rectPool[wi + 1];
        const int rx = static_cast<int>(w0 & 0xFFFFu);
        const int ry = static_cast<int>(w0 >> 16);
        const int rw = static_cast<int>(w1 & 0xFFFFu);
        const int rh = static_cast<int>(w1 >> 16);
        const int l = rx > tx ? rx : tx;
        const int t = ry > ty ? ry : ty;
        const int r = (rx + rw) < (tx + 64) ? (rx + rw) : (tx + 64);
        const int b = (ry + rh) < (ty + 64) ? (ry + rh) : (ty + 64);
        if (r <= l || b <= t) {
          continue;
        }
        if (r > surfaceW || b > surfaceH) {
          composeRegion = false;
        }
      }
    }
    if (!composeRegion) {
      progressiveComposeSkipped++;
    }
    VkApi& vk = api();

    // The payload is bound at its arena offset; every job offset is relative to
    // it (they were computed as `ptr - payload`). The rect pool is shared by all
    // chunks via the tile meta offsets.
    VkBuffer payloadBuffer = VK_NULL_HANDLE;
    VkDeviceSize payloadOffset = 0;
    if (!StageAppend(payload, size, &payloadBuffer, &payloadOffset)) {
      return false;
    }
    VkBuffer rectBuffer = VK_NULL_HANDLE;
    VkDeviceSize rectOffset = 0;
    if (!rectPool.empty()) {
      if (!StageAppend(rectPool.data(), rectPool.size() * sizeof(uint32_t), &rectBuffer,
                       &rectOffset)) {
        return false;
      }
    }

    const uint32_t chunkTiles = kRfxChunkTiles;
    for (size_t start = 0; start < tiles.size(); start += chunkTiles) {
      uint32_t count = static_cast<uint32_t>(tiles.size() - start);
      if (count > chunkTiles) {
        count = chunkTiles;
      }
      const uint32_t streams = count * 3;

      std::vector<uint8_t> meta(static_cast<size_t>(streams) * kMetaStride, 0);
      std::vector<uint32_t> tileMeta(static_cast<size_t>(count) * 4, 0);
      for (uint32_t t = 0; t < count; ++t) {
        const TileJob& job = tiles[start + t];
        // Tile pixel origin = destRect origin + 64 * tile index (gdi's
        // updateRect = nXDst + tile->x).
        const int tilePx = originX + static_cast<int>(job.x) * 64;
        const int tilePy = originY + static_cast<int>(job.y) * 64;
        tileMeta[t * 4] = static_cast<uint32_t>(tilePx);
        tileMeta[t * 4 + 1] = static_cast<uint32_t>(tilePy);
        tileMeta[t * 4 + 2] = job.rectOffset;
        tileMeta[t * 4 + 3] = job.rectCount;
        MarkSurfaceDirty(*surface, tilePx, tilePy, tilePx + 64, tilePy + 64);
        for (int c = 0; c < 3; ++c) {
          uint8_t* rec = &meta[(static_cast<size_t>(t) * 3 + c) * kMetaStride];
          const StreamJob& sj = job.streams[c];
          rec[0] = static_cast<uint8_t>(sj.type);
          rec[1] = static_cast<uint8_t>(sj.flags);
          std::memcpy(rec + 4, &sj.tileStream, 4);
          std::memcpy(rec + 8, &sj.payloadOff, 4);
          std::memcpy(rec + 12, &sj.payloadLen, 4);
          std::memcpy(rec + 16, &sj.srlOff, 4);
          std::memcpy(rec + 20, &sj.srlLen, 4);
          std::memcpy(rec + 24, &sj.rawOff, 4);
          std::memcpy(rec + 28, &sj.rawLen, 4);
          std::memcpy(rec + 32, sj.shift, 10);
          std::memcpy(rec + 42, sj.newBit, 10);
          if (sj.type == 0u) {
            rfxFirstTiles++;
          } else {
            rfxUpgradeTiles++;
          }
        }
      }

      VkBuffer metaBuffer = VK_NULL_HANDLE;
      VkDeviceSize metaOffset = 0;
      VkBuffer tileMetaBuffer = VK_NULL_HANDLE;
      VkDeviceSize tileMetaOffset = 0;
      if (!StageAppend(meta.data(), meta.size(), &metaBuffer, &metaOffset) ||
          !StageAppend(tileMeta.data(), tileMeta.size() * sizeof(uint32_t), &tileMetaBuffer,
                       &tileMetaOffset)) {
        return false;
      }

      VkDescriptorSet decodeSet = VK_NULL_HANDLE;
      if (!AllocSet(decodeSetLayout, &decodeSet)) {
        return false;
      }
      WriteBuffer(decodeSet, 0, payloadBuffer, payloadOffset, size);
      WriteBuffer(decodeSet, 1, metaBuffer, metaOffset, meta.size());
      WriteBuffer(decodeSet, 2, coef.buffer, 0, VK_WHOLE_SIZE);
      WriteBuffer(decodeSet, 3, surface->rfx.cur.buffer, 0, VK_WHOLE_SIZE);
      WriteBuffer(decodeSet, 4, surface->rfx.sign.buffer, 0, VK_WHOLE_SIZE);
      WriteBuffer(decodeSet, 5, surface->rfx.bp.buffer, 0, VK_WHOLE_SIZE);

      BarrierBeforeCompute();
      vk.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipe);
      vk.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipeLayout, 0, 1,
                               &decodeSet, 0, nullptr);
      struct DecodePush {
        uint32_t numStreams;
        uint32_t compBase;
        uint32_t tempBase;
      } dpush{streams, 0u, streams * 4096u};
      vk.CmdPushConstants(commandBuffer, decodePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(dpush), &dpush);
      vk.CmdDispatch(commandBuffer, (streams + 63u) / 64u, 1, 1);
      pendingComputeWrites = true;
      computeInFlight = true;

      if (!composeRegion) {
        // update_tiles failed for the region: the tile state has been advanced by
        // the decode above, but no pixel of the region is composited.
        continue;
      }
      VkDescriptorSet composeSet = VK_NULL_HANDLE;
      if (!AllocSet(composeSetLayout, &composeSet)) {
        return false;
      }
      WriteBuffer(composeSet, 0, tileMetaBuffer, tileMetaOffset, tileMeta.size() * sizeof(uint32_t));
      WriteBuffer(composeSet, 1, coef.buffer, 0, VK_WHOLE_SIZE);
      WriteBuffer(composeSet, 2, surface->gpu.buffer, 0, VK_WHOLE_SIZE);
      WriteBuffer(composeSet, 3, rectBuffer != VK_NULL_HANDLE ? rectBuffer : metaBuffer, rectOffset,
                  rectPool.empty() ? 4u : rectPool.size() * sizeof(uint32_t));

      BarrierBeforeCompute();
      vk.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, composePipe);
      vk.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, composePipeLayout, 0, 1,
                               &composeSet, 0, nullptr);
      struct ComposePush {
        uint32_t numTiles;
        uint32_t compBase;
        int32_t surfaceW;
        int32_t surfaceH;
        int32_t kr;
        int32_t kcrG;
        int32_t kcbG;
        int32_t kcbB;
        uint32_t swapRb;
      } cpush{count, 0u, surfaceW, surfaceH, kKr, kKcrG, kKcbG, kKcbB, swapRb ? 1u : 0u};
      vk.CmdPushConstants(commandBuffer, composePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(cpush), &cpush);
      vk.CmdDispatch(commandBuffer, (count + 63u) / 64u, 1, 1);
      pendingComputeWrites = true;
      computeInFlight = true;
      rfxChunks++;
    }
    return true;
  }

  // V4: ClearCodec is not self-contained (band pixels it does not cover keep the
  // current surface value), so it is a CPU read-modify-write. Since V2 the
  // surface IS the mapping, so FreeRDP's clear_decompress runs straight on it -
  // no staging, no GPU round trip (VULKAN-TODO §4.2 item 3).
  bool ClearCodecDecode(uint16_t surfaceId, const uint8_t* payload, uint32_t payloadLen, int left,
                        int top, int width, int height) {
    if (clearDecoder == nullptr || payload == nullptr || payloadLen == 0 || width <= 0 ||
        height <= 0) {
      return false;
    }
    Surface* surface = Find(surfaceId);
    if (surface == nullptr || !surface->gpu.valid()) {
      return false;
    }
    // ClearCodec is a CPU read-modify-write: it must see the pixels a recorded
    // Progressive dispatch produced, and its own writes must not be clobbered
    // when that dispatch later executes.
    if (!SyncForCpuAccess()) {
      return false;
    }
    const int x = left < 0 ? 0 : left;
    const int y = top < 0 ? 0 : top;
    // The band must lie inside the surface; clear_decompress would otherwise
    // clip, but a partly outside band means the stream is not what we model.
    if (x + width > surface->meta.width || y + height > surface->meta.height) {
      return false;
    }
    // Storage order: the surface's own GFX format (wire 0x20 -> BGRX32,
    // 0x21 -> BGRA32), exactly what gdi hands to clear_decompress; RGBA when the
    // swapchain forces the engine storage to RGBA8. Alpha is 0xFF everywhere in
    // this engine, but the format still has to be the one the surface was
    // created with so the band/glyph pixel interpretation matches gdi.
    const uint32_t format = swapRb ? kPixelFormatRgba32 : surface->meta.format;
    if (!clearDecoder->Decode(payload, payloadLen, width, height, format, surface->gpu.mapped,
                              surface->gpu.stride, x, y, surface->meta.width,
                              surface->meta.height)) {
      return false;
    }
    HostWrote();
    MarkSurfaceDirty(*surface, x, y, x + width, y + height);
    clearDecoded++;
    // Dev: prove the CPU write actually landed in the mapping (an A/B that says
    // "never written" has to distinguish "skipped" from "written elsewhere").
    if (clearDecoded <= 4) {
      const uint8_t* p = surface->gpu.mapped + static_cast<size_t>(y) * surface->gpu.stride +
                         static_cast<size_t>(x) * 4;
      HMRDP_LOGW(
          "vk clearcodec applied: sid=%{public}u rect=(%{public}d,%{public}d)+%{public}dx%{public}d first=b%{public}u g%{public}u r%{public}u stride=%{public}d surfW=%{public}d",
          static_cast<unsigned>(surfaceId), x, y, width, height, p[0], p[1], p[2],
          surface->gpu.stride, surface->meta.width);
    }
    return true;
  }

  void DestroyRfxResources() {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev != VK_NULL_HANDLE) {
      if (rfxPool != VK_NULL_HANDLE && vk.DestroyDescriptorPool != nullptr) {
        vk.DestroyDescriptorPool(dev, rfxPool, nullptr);
      }
      if (composePipe != VK_NULL_HANDLE && vk.DestroyPipeline != nullptr) {
        vk.DestroyPipeline(dev, composePipe, nullptr);
      }
      if (decodePipe != VK_NULL_HANDLE && vk.DestroyPipeline != nullptr) {
        vk.DestroyPipeline(dev, decodePipe, nullptr);
      }
      if (composePipeLayout != VK_NULL_HANDLE && vk.DestroyPipelineLayout != nullptr) {
        vk.DestroyPipelineLayout(dev, composePipeLayout, nullptr);
      }
      if (decodePipeLayout != VK_NULL_HANDLE && vk.DestroyPipelineLayout != nullptr) {
        vk.DestroyPipelineLayout(dev, decodePipeLayout, nullptr);
      }
      if (composeSetLayout != VK_NULL_HANDLE && vk.DestroyDescriptorSetLayout != nullptr) {
        vk.DestroyDescriptorSetLayout(dev, composeSetLayout, nullptr);
      }
      if (decodeSetLayout != VK_NULL_HANDLE && vk.DestroyDescriptorSetLayout != nullptr) {
        vk.DestroyDescriptorSetLayout(dev, decodeSetLayout, nullptr);
      }
    }
    rfxPool = VK_NULL_HANDLE;
    composePipe = VK_NULL_HANDLE;
    decodePipe = VK_NULL_HANDLE;
    composePipeLayout = VK_NULL_HANDLE;
    decodePipeLayout = VK_NULL_HANDLE;
    composeSetLayout = VK_NULL_HANDLE;
    decodeSetLayout = VK_NULL_HANDLE;
  }
};

GfxVkDesktop::GfxVkDesktop() = default;

GfxVkDesktop::~GfxVkDesktop() {
  Reset();
}

bool GfxVkDesktop::Init(VkFormat format) {
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM) {
    HMRDP_LOGW("vk desktop: unsupported image format %{public}u, using B8G8R8A8",
               static_cast<unsigned>(format));
    format = VK_FORMAT_B8G8R8A8_UNORM;
  }
  impl_->format = format;
  impl_->swapRb = (format == VK_FORMAT_R8G8B8A8_UNORM);
  VkContext& context = VkContext::Instance();
  // No surface: the offline correctness harness never presents.
  if (!context.EnsureDevice(VK_NULL_HANDLE)) {
    HMRDP_LOGE("vk desktop: device unavailable: %{public}s", context.lastError().c_str());
    return false;
  }
  VkApi& api = GetVkApi();
  const VkDevice device = context.device();

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = context.queueFamily();
  if (api.CreateCommandPool(device, &poolInfo, nullptr, &impl_->commandPool) != VK_SUCCESS) {
    HMRDP_LOGE("vk desktop: vkCreateCommandPool failed");
    return false;
  }
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = impl_->commandPool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  if (api.AllocateCommandBuffers(device, &alloc, &impl_->commandBuffer) != VK_SUCCESS) {
    HMRDP_LOGE("vk desktop: vkAllocateCommandBuffers failed");
    return false;
  }
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (api.CreateFence(device, &fenceInfo, nullptr, &impl_->fence) != VK_SUCCESS) {
    HMRDP_LOGE("vk desktop: vkCreateFence failed");
    return false;
  }
  // V3 Progressive compute. Optional: when it cannot be created the engine still
  // serves every other command and Progressive stays "unsupported" (counted and
  // visible in the stats, never silently wrong).
  if (impl_->CreateRfxPipelines()) {
    const size_t coefBytes = static_cast<size_t>(kRfxChunkTiles) * 3u * 4096u * 4u;
    if (impl_->CreateRawBuffer(&impl_->coef, coefBytes)) {
      impl_->rfxReady = true;
    } else {
      HMRDP_LOGE("vk desktop: progressive scratch allocation failed");
    }
  } else {
    HMRDP_LOGW("vk desktop: progressive compute unavailable (pipeline creation failed)");
  }
  // V4: ClearCodec read-modify-write on the CPU. Independent of the compute
  // pipeline; a failure just leaves ClearCodec in the unsupported path.
  impl_->clearDecoder = CreateFreeRdpClearDecoder();
  if (impl_->clearDecoder == nullptr) {
    HMRDP_LOGW("vk desktop: FreeRDP clear_decompress unavailable");
  }
  ready_ = true;
  HMRDP_LOGI("vk desktop: engine ready (V2 storage: host-visible buffers, rfxCompute=%{public}d)",
             impl_->rfxReady ? 1 : 0);
  return true;
}

void GfxVkDesktop::Reset() {
  if (impl_ == nullptr) {
    return;
  }
  const int64_t resetStart = Impl::NowUs();
  impl_->Flush();
  impl_->ReleasePending();
  const int64_t afterRelease = Impl::NowUs();
  VkApi& api = GetVkApi();
  const VkDevice device = impl_->device();
  for (auto& kv : impl_->surfaces) {
    impl_->DestroyGpuBuffer(&kv.second.gpu);
    impl_->DestroyGpuBuffer(&kv.second.rfx.cur);
    impl_->DestroyGpuBuffer(&kv.second.rfx.sign);
    impl_->DestroyGpuBuffer(&kv.second.rfx.bp);
  }
  impl_->surfaces.clear();
  const size_t cacheCount = impl_->cache.size();
  for (auto& kv : impl_->cache) {
    impl_->DestroyGpuBuffer(&kv.second.gpu);
  }
  impl_->cache.clear();
  const int64_t afterCache = Impl::NowUs();
  impl_->DestroyImage(&impl_->screen);
  impl_->DestroyGpuBuffer(&impl_->coef);
  impl_->DestroyStage();
  impl_->DestroyRfxResources();
  impl_->clearDecoder.reset();
  if (device != VK_NULL_HANDLE) {
    if (impl_->fillBuffer != VK_NULL_HANDLE) {
      api.DestroyBuffer(device, impl_->fillBuffer, nullptr);
      api.FreeMemory(device, impl_->fillMemory, nullptr);
    }
    if (impl_->readBuffer != VK_NULL_HANDLE) {
      api.UnmapMemory(device, impl_->readMemory);
      api.DestroyBuffer(device, impl_->readBuffer, nullptr);
      api.FreeMemory(device, impl_->readMemory, nullptr);
    }
    if (impl_->fence != VK_NULL_HANDLE) {
      api.DestroyFence(device, impl_->fence, nullptr);
    }
    if (impl_->commandPool != VK_NULL_HANDLE) {
      api.DestroyCommandPool(device, impl_->commandPool, nullptr);
    }
  }
  delete impl_;
  impl_ = nullptr;
  ready_ = false;
  screenW_ = 0;
  screenH_ = 0;
  HMRDP_LOGI("vk desktop: reset %{public}llu ms (flush+release=%{public}llu cache[%{public}u]=%{public}llu tail=%{public}llu)",
             static_cast<unsigned long long>((Impl::NowUs() - resetStart) / 1000),
             static_cast<unsigned long long>((afterRelease - resetStart) / 1000),
             static_cast<unsigned>(cacheCount),
             static_cast<unsigned long long>((afterCache - afterRelease) / 1000),
             static_cast<unsigned long long>((Impl::NowUs() - afterCache) / 1000));
}

bool GfxVkDesktop::ready() const {
  return ready_ && impl_ != nullptr;
}

bool GfxVkDesktop::swapRb() const {
  return impl_ != nullptr && impl_->swapRb;
}

bool GfxVkDesktop::Flush() {
  return impl_ != nullptr && impl_->Flush();
}

bool GfxVkDesktop::CreateSurface(uint16_t surfaceId, int width, int height, uint32_t format) {
  if (!ready() || width <= 0 || height <= 0) {
    return false;
  }
  DeleteSurface(surfaceId);

  Impl::Surface surface;
  surface.meta.id = surfaceId;
  surface.meta.width = Align16(width);
  surface.meta.height = Align16(height);
  surface.meta.stride = Align16(surface.meta.width * 4);
  // FreeRDP maps the wire format 0x20 -> BGRX32, 0x21 -> BGRA32; both are BGRA
  // bytes, which is exactly the internal pixel order.
  surface.meta.format = (format == 0x20u) ? kPixelFormatBgrx32 : kPixelFormatBgra32;
  surface.meta.gridW = (surface.meta.width + 63) / 64;
  surface.meta.gridH = (surface.meta.height + 63) / 64;
  surface.meta.mappedWidth = width;
  surface.meta.mappedHeight = height;

  if (!impl_->CreateGpuBuffer(&surface.gpu, surface.meta.width, surface.meta.height)) {
    return false;
  }
  // FreeRDP's CreateSurface contract: every pixel starts as 0xFF, and unpainted
  // pixels are what the pixel comparison observes. 0xFFFFFFFF is channel-order
  // symmetric, so it needs no swap.
  std::memset(surface.gpu.mapped, 0xFF, surface.gpu.capacity);
  impl_->HostWrote();
  // Persistent Progressive tile state. Allocation failure degrades only the
  // Progressive path for this surface (counted as unsupported), not the surface.
  if (impl_->rfxReady &&
      !impl_->CreateRfxState(&surface.rfx, surface.meta.gridW, surface.meta.gridH)) {
    HMRDP_LOGW("vk desktop: surface %{public}u progressive state allocation failed", surfaceId);
  }

  HMRDP_LOGI("vk desktop: surface %{public}u %{public}dx%{public}d stride=%{public}d", surfaceId,
             surface.meta.width, surface.meta.height, surface.meta.stride);
  impl_->surfaces[surfaceId] = surface;
  return true;
}

void GfxVkDesktop::DeleteSurface(uint16_t surfaceId) {
  if (impl_ == nullptr) {
    return;
  }
  const auto it = impl_->surfaces.find(surfaceId);
  if (it == impl_->surfaces.end()) {
    return;
  }
  // The buffer may still be referenced by recorded/in-flight work, so its
  // destruction is deferred to the next fence wait.
  impl_->DeferGpuBuffer(&it->second.gpu);
  impl_->surfaces.erase(it);
}

const GpuSurface* GfxVkDesktop::FindSurface(uint16_t surfaceId) const {
  const Impl::Surface* surface = impl_ != nullptr ? impl_->Find(surfaceId) : nullptr;
  return surface != nullptr ? &surface->meta : nullptr;
}

void GfxVkDesktop::MapSurfaceToOutput(uint16_t surfaceId, uint32_t outputOriginX,
                                      uint32_t outputOriginY) {
  Impl::Surface* surface = impl_ != nullptr ? impl_->Find(surfaceId) : nullptr;
  if (surface != nullptr) {
    surface->meta.mapped = true;
    surface->meta.outputX = outputOriginX;
    surface->meta.outputY = outputOriginY;
    // gdi_MapSurfaceToOutput clears the surface's invalid region.
    surface->meta.dirtyValid = false;
  }
}

bool GfxVkDesktop::ResetGraphics(int width, int height) {
  if (!ready()) {
    return false;
  }
  impl_->Flush();
  // FreeRDP's path is `gdi_ResetGraphics`: it asks the update layer to resize
  // the primary buffer, and `gdi_resize()` is a **no-op when the size is
  // unchanged** (the live harness' Resize() early-returns too), so the picture
  // survives a same-size ResetGraphics. Only an actual size change recreates the
  // primary buffer - and a fresh gdi_CreateCompatibleBitmap is memset to 0xFF
  // ("Initialize with 0xff"). Recreating unconditionally here made the engine
  // blank the screen on every repeated ResetGraphics of the same size, which the
  // gdi screen did not do (observed as a full-screen diff).
  const bool sizeChanged = (width != screenW_ || height != screenH_);
  impl_->screenDirtyValid = false;
  if (sizeChanged) {
    impl_->DestroyImage(&impl_->screen);
    screenW_ = 0;
    screenH_ = 0;
    if (width > 0 && height > 0) {
      if (!impl_->PrepareImage(&impl_->screen, width, height)) {
        return false;
      }
      impl_->FillImage(&impl_->screen, 0xFFFFFFFFu);
      screenW_ = impl_->screen.width;
      screenH_ = impl_->screen.height;
    }
  }
  // The rest of gdi_ResetGraphics is per-surface and applies on every call:
  // wipe every GFX surface to 0xFF and drop its invalid region. The Progressive
  // state is deliberately NOT touched: this FreeRDP's
  // `progressive_context_reset()` is a no-op (its own comment: the codec caches
  // must not be reset by a ResetGraphics PDU), so the per-(tile,component)
  // `current`/`sign`/bit positions survive - exactly like gdi. ClearCodec *is*
  // reset by gdi (freerdp_client_codecs_reset -> clear_context_reset, which
  // restarts its band sequence number), so mirror that too.
  for (auto& kv : impl_->surfaces) {
    Impl::Surface& surface = kv.second;
    if (surface.gpu.valid()) {
      std::memset(surface.gpu.mapped, 0xFF, surface.gpu.capacity);
    }
    surface.meta.dirtyValid = false;
  }
  if (impl_->clearDecoder != nullptr) {
    impl_->clearDecoder->Reset();
  }
  impl_->HostWrote();
  return true;
}

bool GfxVkDesktop::Compose() {
  if (!ready() || impl_->screen.image == VK_NULL_HANDLE) {
    return false;
  }
  const int64_t composeStart = Impl::NowUs();
  const int scrW = impl_->screen.width;
  const int scrH = impl_->screen.height;
  for (auto& kv : impl_->surfaces) {
    Impl::Surface& s = kv.second;
    GpuSurface& m = s.meta;
    if (!m.mapped) {
      impl_->composeSkipUnmapped++;
      continue;
    }
    if (!m.dirtyValid) {
      impl_->composeSkipClean++;
      continue;
    }
    int left = m.dirtyLeft;
    int top = m.dirtyTop;
    int right = m.dirtyRight;
    int bottom = m.dirtyBottom;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > m.mappedWidth) right = m.mappedWidth;
    if (bottom > m.mappedHeight) bottom = m.mappedHeight;
    if (right <= left || bottom <= top) {
      m.dirtyValid = false;
      continue;
    }
    // 1:1 output mapping (scaled PDUs unmap the surface); a rect copy.
    int dstX = static_cast<int>(m.outputX) + left;
    int dstY = static_cast<int>(m.outputY) + top;
    if (dstX < 0) dstX = 0;
    if (dstY < 0) dstY = 0;
    if (dstX >= scrW || dstY >= scrH) {
      m.dirtyValid = false;
      continue;
    }
    int dstW = right - left;
    int dstH = bottom - top;
    if (dstW > scrW - dstX) dstW = scrW - dstX;
    if (dstH > scrH - dstY) dstH = scrH - dstY;
    if (dstW <= 0 || dstH <= 0) {
      m.dirtyValid = false;
      continue;
    }
    if (!impl_->CopyBufferRegionToScreen(s.gpu, left, top, dstX, dstY, dstW, dstH)) {
      return false;
    }
    impl_->MarkScreenDirty(dstX, dstY, dstX + dstW, dstY + dstH);
    m.dirtyValid = false;
    impl_->composeCopies++;
  }
  Impl::AddStat(&impl_->statCompose, composeStart);
  return impl_->screenDirtyValid;
}

void GfxVkDesktop::ClearScreenDirty() {
  if (impl_ != nullptr) {
    impl_->screenDirtyValid = false;
  }
}

bool GfxVkDesktop::screenDirty() const {
  return impl_ != nullptr && impl_->screenDirtyValid;
}

int GfxVkDesktop::screenWidth() const {
  return screenW_;
}

int GfxVkDesktop::screenHeight() const {
  return screenH_;
}

VkImage GfxVkDesktop::screenImage() const {
  return impl_ != nullptr ? impl_->screen.image : VK_NULL_HANDLE;
}

VkFormat GfxVkDesktop::format() const {
  return impl_ != nullptr ? impl_->format : VK_FORMAT_UNDEFINED;
}

bool GfxVkDesktop::ReadScreen(std::vector<uint8_t>* out) {
  if (!ready() || out == nullptr || impl_->screen.image == VK_NULL_HANDLE) {
    return false;
  }
  const int64_t readStart = Impl::NowUs();
  const int width = impl_->screen.width;
  const int height = impl_->screen.height;
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  out->assign(bytes, 0);

  // Prior work must be complete before the copy is recorded, and the copy itself
  // must complete before the CPU reads (this is the only place V2 stalls).
  if (!impl_->Flush()) {
    return false;
  }
  if (!impl_->EnsureReadBuffer(bytes)) {
    return false;
  }
  if (!impl_->EnsureRecording()) {
    return false;
  }
  impl_->BarrierBeforeRead();
  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = static_cast<uint32_t>(width);
  region.bufferImageHeight = static_cast<uint32_t>(height);
  region.imageSubresource = ColorLayers();
  region.imageExtent.width = static_cast<uint32_t>(width);
  region.imageExtent.height = static_cast<uint32_t>(height);
  region.imageExtent.depth = 1;
  GetVkApi().CmdCopyImageToBuffer(impl_->commandBuffer, impl_->screen.image,
                                  VK_IMAGE_LAYOUT_GENERAL, impl_->readBuffer, 1, &region);
  impl_->pendingDeviceWrites = true;
  if (!impl_->Flush()) {
    return false;
  }
  Impl::AddStat(&impl_->statRead, readStart);
  std::memcpy(out->data(), impl_->readMapped, bytes);
  if (impl_->swapRb) {
    // Hand the caller FreeRDP's BGRA bytes whatever the internal pixel order.
    uint32_t* pixels = reinterpret_cast<uint32_t*>(out->data());
    const size_t count = bytes / 4;
    for (size_t i = 0; i < count; ++i) {
      pixels[i] = SwapRb(pixels[i]);
    }
  }
  return true;
}

bool GfxVkDesktop::ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out) {
  if (!ready() || out == nullptr) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->gpu.valid()) {
    return false;
  }
  // A recorded Progressive dispatch has not run yet: submit it before reading
  // the mapping, otherwise the pre-decode pixels are returned.
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  // The stored buffer layout (stride * height, top-down) is exactly what the
  // caller expects, so this is a plain copy out of the mapping: no readback, no
  // staging, no stall.
  const size_t bytes = surface->gpu.capacity;
  out->assign(bytes, 0);
  std::memcpy(out->data(), surface->gpu.mapped, bytes);
  if (impl_->swapRb) {
    uint32_t* pixels = reinterpret_cast<uint32_t*>(out->data());
    const size_t count = bytes / 4;
    for (size_t i = 0; i < count; ++i) {
      pixels[i] = SwapRb(pixels[i]);
    }
  }
  return true;
}

bool GfxVkDesktop::ReadSurfaceRect(uint16_t surfaceId, int x, int y, int width, int height,
                                   std::vector<uint8_t>* out) {
  if (!ready() || out == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->gpu.valid()) {
    return false;
  }
  if (x < 0 || y < 0 || x + width > surface->meta.width || y + height > surface->meta.height) {
    return false;
  }
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  out->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  for (int row = 0; row < height; ++row) {
    const uint8_t* src = surface->gpu.mapped +
                         static_cast<size_t>(y + row) * surface->gpu.stride +
                         static_cast<size_t>(x) * 4;
    std::memcpy(out->data() + static_cast<size_t>(row) * width * 4, src,
                static_cast<size_t>(width) * 4);
  }
  if (impl_->swapRb) {
    uint32_t* pixels = reinterpret_cast<uint32_t*>(out->data());
    const size_t count = out->size() / 4;
    for (size_t i = 0; i < count; ++i) {
      pixels[i] = SwapRb(pixels[i]);
    }
  }
  return true;
}

bool GfxVkDesktop::ReadCacheEntry(uint16_t slot, int* width, int* height,
                                  std::vector<uint8_t>* out) {
  if (!ready() || out == nullptr) {
    return false;
  }
  const auto it = impl_->cache.find(slot);
  if (it == impl_->cache.end() || !it->second.gpu.valid() || it->second.width <= 0 ||
      it->second.height <= 0) {
    return false;
  }
  const int w = it->second.width;
  const int h = it->second.height;
  out->resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  for (int row = 0; row < h; ++row) {
    std::memcpy(out->data() + static_cast<size_t>(row) * w * 4,
                it->second.gpu.mapped + static_cast<size_t>(row) * it->second.gpu.stride,
                static_cast<size_t>(w) * 4);
  }
  if (width != nullptr) {
    *width = w;
  }
  if (height != nullptr) {
    *height = h;
  }
  return true;
}

bool GfxVkDesktop::ReadRfxTileState(uint16_t surfaceId, uint32_t tileIndex, int component,
                                    std::vector<int16_t>* cur, std::vector<int16_t>* sign,
                                    std::vector<uint8_t>* bitPos) {
  // Dev/verification only (see the header): the state is written by the decode
  // compute shader, so the CPU has to observe the recorded work first.
  if (!ready() || component < 0 || component > 2 || cur == nullptr || sign == nullptr ||
      bitPos == nullptr) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->rfx.valid()) {
    return false;
  }
  const uint32_t gridStreams =
      static_cast<uint32_t>(surface->meta.gridW) * static_cast<uint32_t>(surface->meta.gridH) * 3u;
  const uint32_t stream = tileIndex * 3u + static_cast<uint32_t>(component);
  if (stream >= gridStreams) {
    return false;
  }
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  const auto& curBuf = surface->rfx.cur;
  const auto& signBuf = surface->rfx.sign;
  const auto& bpBuf = surface->rfx.bp;
  const size_t curBytes = static_cast<size_t>(stream) * 4096u * sizeof(int16_t);
  if (curBytes + 4096u * sizeof(int16_t) > curBuf.capacity ||
      curBytes + 4096u * sizeof(int16_t) > signBuf.capacity) {
    return false;
  }
  cur->resize(4096);
  sign->resize(4096);
  std::memcpy(cur->data(), curBuf.mapped + curBytes, 4096u * sizeof(int16_t));
  std::memcpy(sign->data(), signBuf.mapped + curBytes, 4096u * sizeof(int16_t));
  // 12 bytes per stream in the engine (kBitPosStride): 10 bytes are used, the
  // rest is padding so adjacent streams never share a 32-bit word.
  const size_t bpBytes = static_cast<size_t>(stream) * 12u;
  if (bpBytes + 10u > bpBuf.capacity) {
    return false;
  }
  bitPos->assign(bpBuf.mapped + bpBytes, bpBuf.mapped + bpBytes + 10u);
  return true;
}

bool GfxVkDesktop::SolidFill(uint16_t surfaceId, uint32_t bgraPixel, const uint16_t* rects,
                             uint32_t rectCount) {
  if (!ready() || rects == nullptr || rectCount == 0) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->gpu.valid()) {
    return false;
  }
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  const int surfaceW = surface->meta.width;
  const int surfaceH = surface->meta.height;
  const uint32_t texel = impl_->swapRb ? SwapRb(bgraPixel) : bgraPixel;
  for (uint32_t i = 0; i < rectCount; ++i) {
    int left = rects[i * 4 + 0];
    int top = rects[i * 4 + 1];
    int right = rects[i * 4 + 2];
    int bottom = rects[i * 4 + 3];
    if (right > surfaceW) right = surfaceW;
    if (bottom > surfaceH) bottom = surfaceH;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0) {
      continue;
    }
    CpuFillRect(surface->gpu.mapped, surface->gpu.stride, left, top, width, height, texel);
    impl_->HostWrote();
    Impl::MarkSurfaceDirty(*surface, left, top, right, bottom);
  }
  return true;
}

bool GfxVkDesktop::UploadBgra(uint16_t surfaceId, int left, int top, int width, int height,
                             const uint8_t* bgra, int srcStride) {
  if (!ready() || bgra == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->gpu.valid()) {
    return false;
  }
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  int sx = left < 0 ? 0 : left;
  int sy = top < 0 ? 0 : top;
  int ex = left + width;
  int ey = top + height;
  if (ex > surface->meta.width) ex = surface->meta.width;
  if (ey > surface->meta.height) ey = surface->meta.height;
  if (ex <= sx || ey <= sy) {
    return true;
  }
  const int srcCol = sx - left;
  const int srcRow0 = sy - top;
  const int rows = ey - sy;
  const int cols = ex - sx;
  const int dstStride = surface->gpu.stride;
  for (int row = 0; row < rows; ++row) {
    const uint8_t* srcRow =
        bgra + static_cast<size_t>(srcRow0 + row) * srcStride + static_cast<size_t>(srcCol) * 4;
    uint8_t* dstRow = surface->gpu.mapped + static_cast<size_t>(sy + row) * dstStride +
                      static_cast<size_t>(sx) * 4;
    if (impl_->swapRb) {
      for (int col = 0; col < cols; ++col) {
        dstRow[col * 4 + 0] = srcRow[col * 4 + 2];
        dstRow[col * 4 + 1] = srcRow[col * 4 + 1];
        dstRow[col * 4 + 2] = srcRow[col * 4 + 0];
        dstRow[col * 4 + 3] = srcRow[col * 4 + 3];
      }
    } else {
      std::memcpy(dstRow, srcRow, static_cast<size_t>(cols) * 4);
    }
  }
  impl_->HostWrote();
  Impl::MarkSurfaceDirty(*surface, sx, sy, ex, ey);
  return true;
}

bool GfxVkDesktop::SurfaceToCache(uint16_t surfaceId, uint16_t slot, int x, int y, int width,
                                 int height) {
  if (!ready() || width <= 0 || height <= 0) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->gpu.valid()) {
    return false;
  }
  if (x < 0 || y < 0 || x + width > surface->meta.width || y + height > surface->meta.height) {
    return false;
  }
  // The source pixels may still be sitting in a recorded compute dispatch.
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  Impl::CacheEntry& entry = impl_->cache[slot];
  if (entry.gpu.width < width || entry.gpu.height < height || !entry.gpu.valid()) {
    impl_->DeferGpuBuffer(&entry.gpu);
    if (!impl_->CreateGpuBuffer(&entry.gpu, width, height)) {
      return false;
    }
  }
  entry.width = width;
  entry.height = height;
  CpuCopyRows(surface->gpu.mapped, surface->gpu.stride, x, y, entry.gpu.mapped, entry.gpu.stride, 0,
              0, width, height);
  impl_->HostWrote();
  return true;
}

bool GfxVkDesktop::CacheToSurface(uint16_t surfaceId, uint16_t slot, int dstX, int dstY) {
  if (!ready()) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->gpu.valid()) {
    return false;
  }
  // The write must land after, not before, any recorded compute dispatch that
  // targets this surface (it would otherwise overwrite the restored pixels).
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  const auto it = impl_->cache.find(slot);
  if (it == impl_->cache.end() || !it->second.gpu.valid()) {
    return false;
  }
  const Impl::CacheEntry& entry = it->second;
  // gdi_CacheToSurface validates `{destPt, destPt + entrySize}` against the
  // surface and, when it does not fit, fails the whole command *without*
  // clipping or copying (the caller then skips the remaining destPts). Clipping
  // here instead would paint part of a bitmap where gdi paints nothing.
  if (dstX < 0 || dstY < 0 || dstX + entry.width > surface->meta.width ||
      dstY + entry.height > surface->meta.height) {
    return false;
  }
  CpuCopyRows(entry.gpu.mapped, entry.gpu.stride, 0, 0, surface->gpu.mapped, surface->gpu.stride,
              dstX, dstY, entry.width, entry.height);
  impl_->HostWrote();
  Impl::MarkSurfaceDirty(*surface, dstX, dstY, dstX + entry.width, dstY + entry.height);
  return true;
}

void GfxVkDesktop::EvictCache(uint16_t slot) {
  if (impl_ == nullptr) {
    return;
  }
  const auto it = impl_->cache.find(slot);
  if (it == impl_->cache.end()) {
    return;
  }
  impl_->DeferGpuBuffer(&it->second.gpu);
  impl_->cache.erase(it);
}

bool GfxVkDesktop::SurfaceToSurface(uint16_t srcSurfaceId, int srcX, int srcY, int width,
                                   int height, uint16_t dstSurfaceId, int dstX, int dstY) {
  if (!ready() || width <= 0 || height <= 0) {
    return false;
  }
  const Impl::Surface* src = impl_->Find(srcSurfaceId);
  Impl::Surface* dst = impl_->Find(dstSurfaceId);
  if (src == nullptr || dst == nullptr || !src->gpu.valid() || !dst->gpu.valid()) {
    return false;
  }
  // Both sides are CPU-accessed: the source pixels may still be in a recorded
  // compute dispatch, and the destination write must follow it.
  if (!impl_->SyncForCpuAccess()) {
    return false;
  }
  // gdi_SurfaceToSurface validates the source rect and every destination rect
  // against their surfaces and fails the whole command on the first mismatch -
  // it never clips. Clipping here painted pixels gdi leaves untouched.
  if (srcX < 0 || srcY < 0 || srcX + width > src->meta.width || srcY + height > src->meta.height) {
    return false;
  }
  if (dstX < 0 || dstY < 0 || dstX + width > dst->meta.width || dstY + height > dst->meta.height) {
    return false;
  }
  const int dx = dstX;
  const int dy = dstY;
  const int w = width;
  const int h = height;
  const int sx = srcX;
  const int sy = srcY;
  // Stage through a scratch vector so overlapping same-surface copies are safe
  // (a plain row copy could read rows already overwritten by the destination).
  impl_->copyScratch.resize(static_cast<size_t>(w) * h * 4);
  uint8_t* staged = impl_->copyScratch.data();
  CpuCopyRows(src->gpu.mapped, src->gpu.stride, sx, sy, staged, w * 4, 0, 0, w, h);
  CpuCopyRows(staged, w * 4, 0, 0, dst->gpu.mapped, dst->gpu.stride, dx, dy, w, h);
  impl_->HostWrote();
  Impl::MarkSurfaceDirty(*dst, dx, dy, dx + w, dy + h);
  return true;
}

void GfxVkDesktop::ApplyCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                               const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                               uint32_t payloadLen) {
  const int64_t opStart = impl_ != nullptr ? Impl::NowUs() : 0;
  switch (cmdId) {
    case kGpuCmdCreateSurface:
      if (scalars != nullptr) {
        CreateSurface(static_cast<uint16_t>(surfaceId), static_cast<int>(scalars[0]),
                      static_cast<int>(scalars[1]), scalars[2]);
      }
      break;
    case kGpuCmdDeleteSurface:
      DeleteSurface(static_cast<uint16_t>(surfaceId));
      break;
    case kGpuCmdSolidFill:
      if (scalars != nullptr && params != nullptr) {
        // FreeRDP always uses alpha 0xFF regardless of the PDU's XA byte.
        SolidFill(static_cast<uint16_t>(surfaceId), (scalars[0] & 0x00FFFFFFu) | 0xFF000000u,
                  reinterpret_cast<const uint16_t*>(params), scalars[1]);
      }
      break;
    case kGpuCmdSurfaceToSurface: {
      if (scalars == nullptr || params == nullptr || paramsLen < 8) {
        break;
      }
      const int sx = GpuRd16(params);
      const int sy = GpuRd16(params + 2);
      const int w = GpuRd16(params + 4) - sx;
      const int h = GpuRd16(params + 6) - sy;
      const uint32_t count = scalars[1];
      const uint32_t avail = (paramsLen - 8) / 4;
      const uint32_t n = count < avail ? count : avail;
      for (uint32_t i = 0; i < n; ++i) {
        const int px = GpuRd16(params + 8 + static_cast<size_t>(i) * 4);
        const int py = GpuRd16(params + 8 + static_cast<size_t>(i) * 4 + 2);
        // gdi fails the whole command on the first invalid destination rect.
        if (!SurfaceToSurface(static_cast<uint16_t>(scalars[0]), sx, sy, w, h,
                              static_cast<uint16_t>(surfaceId), px, py)) {
          break;
        }
      }
      break;
    }
    case kGpuCmdSurfaceToCache: {
      if (scalars == nullptr || params == nullptr || paramsLen < 16) {
        break;
      }
      const int sx = GpuRd16(params + 8);
      const int sy = GpuRd16(params + 10);
      const int w = GpuRd16(params + 12) - sx;
      const int h = GpuRd16(params + 14) - sy;
      if (!SurfaceToCache(static_cast<uint16_t>(surfaceId), static_cast<uint16_t>(scalars[0]), sx,
                          sy, w, h)) {
        // Dev diagnosis: a failed store leaves the slot empty, so a following
        // CacheToSurface silently keeps the old surface content.
        HMRDP_LOGW("vk cache store FAILED slot=%{public}u rect=(%{public}d,%{public}d)+%{public}dx%{public}d sid=%{public}u",
                   static_cast<unsigned>(scalars[0]), sx, sy, w, h, surfaceId);
      }
      break;
    }
    case kGpuCmdCacheToSurface: {
      if (scalars == nullptr || params == nullptr) {
        break;
      }
      const uint32_t count = scalars[1];
      const uint32_t avail = paramsLen / 4;
      const uint32_t n = count < avail ? count : avail;
      for (uint32_t i = 0; i < n; ++i) {
        const int px = GpuRd16(params + static_cast<size_t>(i) * 4);
        const int py = GpuRd16(params + static_cast<size_t>(i) * 4 + 2);
        // gdi fails the whole command on the first invalid destination rect.
        if (!CacheToSurface(static_cast<uint16_t>(surfaceId), static_cast<uint16_t>(scalars[0]), px,
                            py)) {
          HMRDP_LOGW("vk cache restore FAILED slot=%{public}u dst=(%{public}d,%{public}d) sid=%{public}u",
                     static_cast<unsigned>(scalars[0]), px, py, surfaceId);
          break;
        }
      }
      break;
    }
    case kGpuCmdEvictCacheEntry:
      if (scalars != nullptr) {
        EvictCache(static_cast<uint16_t>(scalars[0]));
      }
      break;
    case kGpuCmdWireToSurface: {
      const uint16_t sid = static_cast<uint16_t>(surfaceId);
      if (params == nullptr || paramsLen < 32 || scalars == nullptr || impl_ == nullptr) {
        break;
      }
      Impl::Surface* surface = impl_->Find(sid);
      if (surface == nullptr) {
        // Dev: a command for a surface the engine does not have is dropped
        // silently otherwise (the A/B then reports the pixels as never written).
        if (impl_->missingSurfaceLog++ < 8) {
          HMRDP_LOGW(
              "vk surface command NOT applied: sid=%{public}u codec=0x%{public}x (no such surface)",
              static_cast<unsigned>(sid),
              static_cast<unsigned>(scalars != nullptr ? scalars[0] : 0u));
        }
        break;
      }
      const uint32_t codecId = scalars[0];
      const uint32_t format = GpuRd32(params + 4);
      const int left = static_cast<int>(GpuRd32(params + 8));
      const int top = static_cast<int>(GpuRd32(params + 12));
      const int width = static_cast<int>(GpuRd32(params + 24));
      const int height = static_cast<int>(GpuRd32(params + 28));
      if (codecId == kGpuCodecUncompressed) {
        const uint32_t bpp = format >> 24;
        if (width > 0 && height > 0 && payload != nullptr && (bpp == 24 || bpp == 32) &&
            static_cast<uint64_t>(bpp / 8) * width * height <= payloadLen) {
          if (bpp == 32) {
            UploadBgra(sid, left, top, width, height, payload, width * 4);
          } else {
            std::vector<uint8_t> tmp(static_cast<size_t>(width) * height * 4);
            for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
              tmp[i * 4] = payload[i * 3];
              tmp[i * 4 + 1] = payload[i * 3 + 1];
              tmp[i * 4 + 2] = payload[i * 3 + 2];
              tmp[i * 4 + 3] = 0xFF;
            }
            UploadBgra(sid, left, top, width, height, tmp.data(), width * 4);
          }
        }
      } else if (codecId == kGpuCodecCaprogressive || codecId == kGpuCodecCaprogressiveV2) {
        // V3: RemoteFX Progressive, decoded by the compute pipeline straight
        // into the surface buffer. The PDU destRect origin offsets every tile
        // and region rect (gdi does the same).
        if (!impl_->DecodeProgressive(sid, payload, payloadLen, left, top)) {
          // No compute support / malformed container: leave the surface
          // untouched, but count it (never silent).
          impl_->unsupportedCount++;
          impl_->progressiveFailed++;
        }
      } else if (codecId == kGpuCodecClearCodec) {
        // V4: CPU clear_decompress read-modify-write on the mapped surface.
        if (!impl_->ClearCodecDecode(sid, payload, payloadLen, left, top, width, height)) {
          impl_->unsupportedCount++;
          impl_->clearUnsupported++;
          // Dev: name the reason (a silent bail-out here is invisible otherwise).
          if (impl_->clearUnsupported <= 8) {
            const Impl::Surface* s = impl_->Find(sid);
            std::string info = "missing/not-ready";
            if (s != nullptr && s->gpu.valid()) {
              info = std::to_string(s->meta.width) + "x" + std::to_string(s->meta.height);
            }
            HMRDP_LOGW(
                "vk clearcodec NOT applied: sid=%{public}u rect=(%{public}d,%{public}d)+%{public}dx%{public}d payload=%{public}u surface=%{public}s",
                static_cast<unsigned>(sid), left, top, width, height,
                static_cast<unsigned>(payloadLen), info.c_str());
          }
        }
      }
      break;
    }
    case kGpuCmdMapSurfaceToOutput:
      if (scalars != nullptr) {
        MapSurfaceToOutput(static_cast<uint16_t>(surfaceId), scalars[0], scalars[1]);
      }
      break;
    case kGpuCmdMapSurfaceToScaledOutput:
      // Unsupported (server-side scaling), so unmap like FreeRDP/gdi does.
      if (impl_ != nullptr) {
        if (Impl::Surface* s = impl_->Find(static_cast<uint16_t>(surfaceId))) {
          s->meta.mapped = false;
          s->meta.dirtyValid = false;
        }
      }
      break;
    case kGpuCmdResetGraphics:
      if (scalars != nullptr) {
        ResetGraphics(static_cast<int>(scalars[0]), static_cast<int>(scalars[1]));
      }
      break;
    case kGpuCmdStartFrame:
      // Frame boundary: FreeRDP resets the Progressive "updated tiles" list when
      // the RDPGFX frame id *changes* (PROGRESSIVE_SURFACE_CONTEXT::frameId is
      // compared in progressive_decompress), and the wire frame id can repeat.
      // Apply exactly that rule, otherwise the engine drops tiles gdi still
      // re-composites (see Surface::frameTiles).
      if (impl_ != nullptr && scalars != nullptr) {
        for (auto& entry : impl_->surfaces) {
          if (entry.second.frameId == scalars[0]) {
            impl_->frameIdRepeats++;
            continue;
          }
          entry.second.frameId = scalars[0];
          entry.second.frameTiles.clear();
          if (!entry.second.frameTileSeen.empty()) {
            std::fill(entry.second.frameTileSeen.begin(), entry.second.frameTileSeen.end(), 0);
          }
        }
      }
      break;
    default:
      break;
  }
  if (impl_ != nullptr) {
    Impl::OpStat* stat = &impl_->statLifecycle;
    switch (cmdId) {
      case kGpuCmdSolidFill:
        stat = &impl_->statFill;
        break;
      case kGpuCmdSurfaceToSurface:
        stat = &impl_->statCopy;
        break;
      case kGpuCmdSurfaceToCache:
      case kGpuCmdCacheToSurface:
      case kGpuCmdEvictCacheEntry:
        stat = &impl_->statCache;
        break;
      case kGpuCmdWireToSurface:
        if (scalars != nullptr && (scalars[0] == kGpuCodecCaprogressive ||
                                   scalars[0] == kGpuCodecCaprogressiveV2 ||
                                   scalars[0] == kGpuCodecClearCodec)) {
          stat = &impl_->statProgressive;
        } else {
          stat = &impl_->statUpload;
        }
        break;
      default:
        break;
    }
    Impl::AddStat(stat, opStart);
  }
}

std::string GfxVkDesktop::Stats() const {
  if (impl_ == nullptr) {
    return "vk desktop: not initialised";
  }
  char buf[640];
  std::snprintf(
      buf, sizeof(buf),
      "vk desktop: surfaces=%zu cache=%zu submits=%llu unsupported=%llu screen=%dx%d\n"
      "  us/count fill=%llu/%llu upload=%llu/%llu cache=%llu/%llu copy=%llu/%llu life=%llu/%llu "
      "prog=%llu/%llu compose=%llu/%llu read=%llu/%llu barriers=%llu flushWait=%llums",
      impl_->surfaces.size(), impl_->cache.size(),
      static_cast<unsigned long long>(impl_->submits),
      static_cast<unsigned long long>(impl_->unsupportedCount), screenW_, screenH_,
      static_cast<unsigned long long>(impl_->statFill.us),
      static_cast<unsigned long long>(impl_->statFill.count),
      static_cast<unsigned long long>(impl_->statUpload.us),
      static_cast<unsigned long long>(impl_->statUpload.count),
      static_cast<unsigned long long>(impl_->statCache.us),
      static_cast<unsigned long long>(impl_->statCache.count),
      static_cast<unsigned long long>(impl_->statCopy.us),
      static_cast<unsigned long long>(impl_->statCopy.count),
      static_cast<unsigned long long>(impl_->statLifecycle.us),
      static_cast<unsigned long long>(impl_->statLifecycle.count),
      static_cast<unsigned long long>(impl_->statProgressive.us),
      static_cast<unsigned long long>(impl_->statProgressive.count),
      static_cast<unsigned long long>(impl_->statCompose.us),
      static_cast<unsigned long long>(impl_->statCompose.count),
      static_cast<unsigned long long>(impl_->statRead.us),
      static_cast<unsigned long long>(impl_->statRead.count),
      static_cast<unsigned long long>(impl_->barriers),
      static_cast<unsigned long long>(impl_->flushWaitUs / 1000));
  char buf2[768];
  std::snprintf(buf2, sizeof(buf2),
                "\n  compose copies=%llu skipUnmapped=%llu skipClean=%llu"
                "\n  rfxCompute=%d chunks=%llu first=%llu upgrade=%llu clearDec=%llu "
                "clearUnsup=%llu progFail=%llu progComposeSkip=%llu"
                "\n  rfxParse regions=%llu simple=%llu diff=%llu nonExtrap=%llu skipTiles=%llu "
                "errors=%llu originNonZero=%llu restamped=%llu multiRegion=%llu frameIdRepeats=%llu",
                static_cast<unsigned long long>(impl_->composeCopies),
                static_cast<unsigned long long>(impl_->composeSkipUnmapped),
                static_cast<unsigned long long>(impl_->composeSkipClean), impl_->rfxReady ? 1 : 0,
                static_cast<unsigned long long>(impl_->rfxChunks),
                static_cast<unsigned long long>(impl_->rfxFirstTiles),
                static_cast<unsigned long long>(impl_->rfxUpgradeTiles),
                static_cast<unsigned long long>(impl_->clearDecoded),
                static_cast<unsigned long long>(impl_->clearUnsupported),
                static_cast<unsigned long long>(impl_->progressiveFailed),
                static_cast<unsigned long long>(impl_->progressiveComposeSkipped),
                static_cast<unsigned long long>(impl_->rfxRegions),
                static_cast<unsigned long long>(impl_->rfxSimpleTiles),
                static_cast<unsigned long long>(impl_->rfxDiffTiles),
                static_cast<unsigned long long>(impl_->rfxNonExtrapolate),
                static_cast<unsigned long long>(impl_->rfxSkippedTiles),
                static_cast<unsigned long long>(impl_->rfxParseErrors),
                static_cast<unsigned long long>(impl_->rfxOriginNonZero),
                static_cast<unsigned long long>(impl_->restamped),
                static_cast<unsigned long long>(impl_->rfxMultiRegion),
                static_cast<unsigned long long>(impl_->frameIdRepeats));
  return std::string(buf) + buf2;
}

std::string GfxVkDesktop::lastError() const {
  return ready() ? std::string() : std::string("vk desktop not ready");
}

bool GpuVkPresentComposed(GfxVkDesktop* engine, VkRenderer* renderer) {
  if (engine == nullptr || renderer == nullptr) {
    return false;
  }
  if (!engine->Compose()) {
    return false;  // static frame: nothing dirty, no present (FPS stays 0)
  }
  // Compose() only *records* the buffer->image copies; the screen image is not
  // updated until they are submitted. Flush() before blitting it, otherwise the
  // presenter blits the previous (or initial) screen and, worse, every frame's
  // commands pile up into one giant submission that only runs at teardown.
  if (!engine->Flush()) {
    return false;
  }
  const bool presented = renderer->PresentImage(engine->screenImage(), engine->format(),
                                               engine->screenWidth(), engine->screenHeight());
  if (presented) {
    engine->ClearScreenDirty();
  }
  return presented;
}

}  // namespace hmrdp
