/*
 * HmRdp - Vulkan GFX surface engine (V2). See hmrdp_vk_desktop.h.
 */
#include "hmrdp_vk_desktop.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "hmrdp_log.h"
#include "hmrdp_vk_renderer.h"

namespace hmrdp {
namespace {

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

// Clips [x,x+w) x [y,y+h) to [0,limitW) x [0,limitH); false when empty.
bool ClipRect(int* x, int* y, int* w, int* h, int limitW, int limitH) {
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  if (*x + *w > limitW) {
    *w = limitW - *x;
  }
  if (*y + *h > limitH) {
    *h = limitH - *y;
  }
  return *w > 0 && *h > 0;
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

  struct Surface {
    GpuSurface meta;
    GpuBuffer gpu;
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

  // Screen dirty rectangle (0xFF/0 initialised, mirrors GfxGpuDesktop).
  bool screenDirtyValid = false;
  int screenDirtyL = 0;
  int screenDirtyT = 0;
  int screenDirtyR = 0;
  int screenDirtyB = 0;

  // V1 coverage: Progressive / ClearCodec are not implemented yet. `unsupported`
  // means "since the last reset"; the harness keeps its own ever-seen flag,
  // because once one is applied the surfaces diverge from gdi for good.
  bool unsupported = false;
  uint64_t unsupportedCount = 0;
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
    if (!pendingHostWrites && !pendingDeviceWrites) {
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
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    api().CmdPipelineBarrier(commandBuffer, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
                             0, nullptr, 0, nullptr);
    pendingHostWrites = false;
    pendingDeviceWrites = false;
    barriers++;
  }

  void HostWrote() { pendingHostWrites = true; }

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
    if (pendingDeviceWrites) {
      // Make device writes visible to the *host* before the command buffer ends.
      // This is the spec-mandated dependency for a CPU read of device-written
      // memory (dstStage HOST / dstAccess HOST_READ); most drivers do not insist
      // on it, the platform layer here does.
      VkMemoryBarrier toHost{};
      toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      vk.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
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
    // emitted before the next command buffer's first read.
    pendingDeviceWrites = true;
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
  ready_ = true;
  HMRDP_LOGI("vk desktop: engine ready (V2 storage: host-visible buffers)");
  return true;
}

void GfxVkDesktop::Reset() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->Flush();
  impl_->ReleasePending();
  VkApi& api = GetVkApi();
  const VkDevice device = impl_->device();
  for (auto& kv : impl_->surfaces) {
    impl_->DestroyGpuBuffer(&kv.second.gpu);
  }
  impl_->surfaces.clear();
  for (auto& kv : impl_->cache) {
    impl_->DestroyGpuBuffer(&kv.second.gpu);
  }
  impl_->cache.clear();
  impl_->DestroyImage(&impl_->screen);
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
  impl_->DestroyImage(&impl_->screen);
  impl_->screenDirtyValid = false;
  screenW_ = 0;
  screenH_ = 0;
  if (width <= 0 || height <= 0) {
    return true;
  }
  if (!impl_->PrepareImage(&impl_->screen, width, height)) {
    return false;
  }
  // The screen starts fully transparent black (FreeRDP's gdi behaviour).
  impl_->FillImage(&impl_->screen, 0x00000000u);
  screenW_ = impl_->screen.width;
  screenH_ = impl_->screen.height;
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

bool GfxVkDesktop::SolidFill(uint16_t surfaceId, uint32_t bgraPixel, const uint16_t* rects,
                            uint32_t rectCount) {
  if (!ready() || rects == nullptr || rectCount == 0) {
    return false;
  }
  Impl::Surface* surface = impl_->Find(surfaceId);
  if (surface == nullptr || !surface->gpu.valid()) {
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
  const auto it = impl_->cache.find(slot);
  if (it == impl_->cache.end() || !it->second.gpu.valid()) {
    return false;
  }
  const Impl::CacheEntry& entry = it->second;
  int dx = dstX;
  int dy = dstY;
  int w = entry.width;
  int h = entry.height;
  if (!ClipRect(&dx, &dy, &w, &h, surface->meta.width, surface->meta.height)) {
    return true;  // fully outside the surface
  }
  const int srcX = dx - dstX;
  const int srcY = dy - dstY;
  CpuCopyRows(entry.gpu.mapped, entry.gpu.stride, srcX, srcY, surface->gpu.mapped,
              surface->gpu.stride, dx, dy, w, h);
  impl_->HostWrote();
  Impl::MarkSurfaceDirty(*surface, dx, dy, dx + w, dy + h);
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
  int dx = dstX;
  int dy = dstY;
  int w = width;
  int h = height;
  if (!ClipRect(&dx, &dy, &w, &h, dst->meta.width, dst->meta.height)) {
    return true;
  }
  const int sx = srcX + (dx - dstX);
  const int sy = srcY + (dy - dstY);
  if (sx < 0 || sy < 0 || sx + w > src->meta.width || sy + h > src->meta.height) {
    return false;
  }
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
        SurfaceToSurface(static_cast<uint16_t>(scalars[0]), sx, sy, w, h,
                         static_cast<uint16_t>(surfaceId), px, py);
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
      SurfaceToCache(static_cast<uint16_t>(surfaceId), static_cast<uint16_t>(scalars[0]), sx, sy, w,
                     h);
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
        CacheToSurface(static_cast<uint16_t>(surfaceId), static_cast<uint16_t>(scalars[0]), px, py);
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
      } else if (codecId == kGpuCodecCaprogressive || codecId == kGpuCodecCaprogressiveV2 ||
                 codecId == kGpuCodecClearCodec) {
        // Not implemented yet (VULKAN-TODO §5 V3/V4). The surface keeps
        // its previous pixels, and the frame is flagged so a correctness run
        // excludes it.
        impl_->unsupported = true;
        impl_->unsupportedCount++;
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

bool GfxVkDesktop::unsupportedSeen() const {
  return impl_ != nullptr && impl_->unsupported;
}

void GfxVkDesktop::resetUnsupportedSeen() {
  if (impl_ != nullptr) {
    impl_->unsupported = false;
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
  char buf2[192];
  std::snprintf(buf2, sizeof(buf2),
                "\n  compose copies=%llu skipUnmapped=%llu skipClean=%llu",
                static_cast<unsigned long long>(impl_->composeCopies),
                static_cast<unsigned long long>(impl_->composeSkipUnmapped),
                static_cast<unsigned long long>(impl_->composeSkipClean));
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
  const bool presented = renderer->PresentImage(engine->screenImage(), engine->format(),
                                               engine->screenWidth(), engine->screenHeight());
  if (presented) {
    engine->ClearScreenDirty();
  }
  return presented;
}

}  // namespace hmrdp
