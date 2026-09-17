/*
 * HmRdp - Vulkan GFX surface engine (V2). See hmrdp_vk_desktop.h.
 */
#include "hmrdp_vk_desktop.h"

#include <freerdp/codec/region.h>

// WinPR's synch.h maps CreateSemaphore -> CreateSemaphoreA, which rewrites the
// *Vulkan* entry points of the same name reached through VkApi (`api.CreateSemaphore`)
// in this translation unit. The WinPR headers are already parsed at this point, so
// dropping the macro here only affects the Vulkan calls below.
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "hmrdp_log.h"
#include "hmrdp_vk_renderer.h"

// Generated at build time by cmake/EmbedSpirv.cmake (see CMakeLists.txt).
#include "rfx_compose.comp.h"
#include "rfx_decode.comp.h"
#include "rfx_idwt.comp.h"

namespace hmrdp {
namespace {

// The Progressive decode scratch is sized by GfxVkDesktop::Impl::kMaxBatchStreams:
// one Progressive message is decoded per dispatch, against a fixed-size coefficient
// plane (no per-message allocation).
constexpr uint32_t kMetaStride = 64;  // bytes per (tile,component) stream job
// One tile is 64x64 pixels; the compositor dispatches one lane per tile pixel.
constexpr uint32_t kTilePixels = 4096;

// Both dev probes below measure a property of the *device*, not of the engine
// instance: the replay page re-creates the engine on every route switch, and
// re-probing each time only slows the start of a run down and adds noise to the
// numbers. The first engine instance probes, the rest reuse the result.
std::once_flag g_probeOnce;
std::string g_hostMemoryProbe;
uint64_t g_emptySubmitUs = 0;

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

// Dev bisect: 1 = re-composite the frame's earlier tiles (FreeRDP's update_tiles
// re-stamps the whole frame tile list on every message), 0 = decode only. Turning
// it off is a *diagnostic*: gdi's re-stamp writes pixels the surface already holds
// (it copies the tile's own last-decode pixels again), so a divergence that
// disappears without it means the re-stamp is writing something else.
constexpr bool kRestampEnabled = true;

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
// persistent-mapped host-visible buffers (V2, doc_agent/gfx-engine.md §1). It needs
// to be a transfer destination (fill / compose), a transfer source (readback) and
// SAMPLED: the presenter samples it with the same letterbox quad it uses for CPU
// frames, so the composed screen image is no longer blitted
// (doc_agent/gfx-engine.md §2.3).
//
// STORAGE is deliberately absent: with it set, the platform's Vulkan layer
// silently dropped every transfer to and from images (the 0xFF initialisation
// read back as all zeros).
constexpr VkImageUsageFlags kImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT;

// Surfaces / cache need to be a transfer source and destination. STORAGE_BUFFER
// is included already because V3's Progressive compute shader writes tiles
// straight into these buffers (doc_agent/gfx-engine.md §1), and adding it later
// would mean re-allocating every live surface.
constexpr VkBufferUsageFlags kSurfaceBufferUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

inline int Align16(int value) {
  return (value + 15) & ~15;
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
    // Changed rects since the last Compose, collected as the commands touch the
    // surface (one per decoded Progressive tile, one per cache restore, ...).
    // They are merged into exact rectangles at Compose time (CoalesceRects) and
    // composed individually - never as one bounding box - which is the same policy
    // the CPU (gdi) present path follows; gdi gets the merging for free from
    // region16 (doc_agent/gfx-engine.md §2.3). `dirtyOverflow` means even the
    // merged list was too long, so the union box in `meta` is composed instead.
    struct DirtyRect {
      int left = 0;
      int top = 0;
      int right = 0;
      int bottom = 0;
    };
    std::vector<DirtyRect> dirtyRects;
    bool dirtyOverflow = false;
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

  // The engine's one storage/picture format: FreeRDP's own BGRA byte order. The
  // presenter samples it and lets the image format do the channel conversion
  // (doc_agent/gfx-engine.md §2.3), so nothing in here has to swap R/B.
  static constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;
  VkFormat format = kFormat;
  // Host-visible memory selection (perf-critical). The CPU addresses surface and
  // cache pixels directly, so those allocations must use the device's *cached*
  // host-visible type when it has one: on the target device the
  // DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT type is uncached and reads at
  // ~120 MB/s (measured: vk desktop host memory probe), which made every CPU-side
  // pixel command - ClearCodec, cache store/restore, SurfaceToSurface - the
  // bottleneck (95 MB/s end to end) rather than the GPU.
  //
  // A cached type is not HOST_COHERENT, so the CPU/GPU handoff needs explicit
  // range-scoped cache maintenance: host writes are flushed before the GPU reads
  // them, device writes are invalidated before the CPU reads them.
  bool hostCoherent = true;
  VkDeviceSize atomSize = 64;
  std::string hostMemoryType;
  // Device memories the CPU wrote since the last submit. Identified by handle, not
  // by buffer address: a surface/cache buffer is built in a local and *copied*
  // into its container, so a stored pointer would dangle.
  std::vector<VkDeviceMemory> dirtyHostMemories;
  std::vector<VkMappedMemoryRange> invalidateScratch;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  // Frames in flight. Each slot owns a command buffer and a fence so one submission
  // can stay in flight while the next frame is recorded - the same shape the
  // presenter already uses (`VkRenderer::kFramesInFlight`), and what lets the
  // present stop waiting for the frame's own decode (doc_agent/gfx-engine.md §2.3).
  // `commandBuffer`/`fence`/`recording` are a *view* of the current slot, kept in
  // sync by SelectSlot()/StoreSlot(), so every recording site keeps addressing one
  // command buffer.
  static constexpr int kSlots = 2;
  int slot = 0;
  VkCommandBuffer commandBuffers[kSlots] = {};
  VkFence fences[kSlots] = {};
  VkSemaphore frameSemaphores[kSlots] = {};
  bool slotRecording[kSlots] = {};
  // A slot whose submission has been sent but whose fence has not been waited yet.
  // Re-recording it (or rewinding the arena/pool it references) requires that wait.
  bool slotInFlight[kSlots] = {};
  // The slot whose frame semaphore the presenter must wait (it stays valid even after
  // the recording slot advances to the next frame).
  int lastSubmittedSlot = 0;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool recording = false;
  // Device-side hand-off (doc_agent/gfx-engine.md §2.3), all on the GPU so the CPU
  // never waits for a frame:
  //  * `frameSemaphores[slot]`: signaled by the frame's submission, waited by the
  //    presenter's blit for that frame;
  //  * `engineChain[slot]`: signaled by every submission, waited by the *next* one -
  //    the surfaces and the decode scratch are read-modify-written across frames, so
  //    engine frames must stay ordered on the device (this used to be implied by
  //    waiting the previous blit, which also serialized compose against blit);
  //  * `blitDone[slot]`: handed to the presenter to signal when its blit finishes;
  //    waited when this slot's picture is written again (two frames later), so a
  //    picture is never overwritten while the presenter still reads it.
  VkSemaphore engineChain[kSlots] = {};
  bool engineChainPending[kSlots] = {};
  VkSemaphore blitDone[kSlots] = {};
  bool blitDonePending[kSlots] = {};

  void SelectSlot(int next) {
    slot = next;
    commandBuffer = commandBuffers[next];
    fence = fences[next];
    recording = slotRecording[next];
  }

  void StoreSlot() { slotRecording[slot] = recording; }
  // Writes that later reads in the recorded command buffer must be ordered
  // after. Kept apart because the source stage differs: a CPU write to a mapped
  // buffer is HOST_WRITE, a vkCmdFillBuffer/vkCmdCopyBufferToImage write is
  // TRANSFER_WRITE (doc_agent/gfx-engine.md §3 "CPU 写的表面 -> GPU 读").
  bool pendingHostWrites = false;
  bool pendingDeviceWrites = false;
  // A compute dispatch is recorded but has not been submitted yet. The surface
  // buffers are both GPU-written (Progressive compute) and CPU-accessed
  // (ClearCodec read-modify-write, bitmap cache, ReadSurface), so a CPU access
  // while a dispatch is only *recorded* would read the pre-dispatch pixels and
  // then be overwritten when the dispatch finally runs at the next Flush - the
  // exact ordering gdi does not have. SyncForCpuAccess() submits and waits first.
  // Per slot: a slot may have compute recorded/submitted while another slot's
  // submission is already done (any of them forces a CPU readback to drain first).
  bool slotComputeInFlight[kSlots] = {};

  bool AnyComputeInFlight() const {
    for (int i = 0; i < kSlots; ++i) {
      if (slotComputeInFlight[i]) {
        return true;
      }
    }
    return false;
  }

  std::map<uint16_t, Surface> surfaces;
  std::map<uint16_t, CacheEntry> cache;
  // Two pictures (ping-pong), one per frame slot: the presenter samples the picture the
  // last submitted frame composed into while the next frame composes the other one.
  // Each picture carries the deltas it is *missing* - the rects the other picture got
  // since this one was last written - because the picture is *accumulated* content
  // (only this frame's dirty rects are copied into it), so its untouched areas would
  // otherwise still show the desktop as it was two frames ago
  // (doc_agent/gfx-engine.md §2.3).
  GpuImage pictures[kSlots];
  std::map<uint16_t, std::vector<Surface::DirtyRect>> pictureMissing[kSlots];
  int pictureW = 0;
  int pictureH = 0;

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
  // Second half of the tile decode: the inverse DWT, one workgroup per
  // (tile,component) stream with the coefficient plane in shared memory. Split
  // out of the decode kernel because the transform's global-SSBO round trips were
  // the decode bottleneck (rfx_idwt.comp).
  VkDescriptorSetLayout idwtSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout idwtPipeLayout = VK_NULL_HANDLE;
  VkPipeline idwtPipe = VK_NULL_HANDLE;
  // One descriptor pool per frame slot: the sets are allocated while recording and
  // released by resetting the pool, which must not touch a slot still in flight.
  VkDescriptorPool rfxPools[kSlots] = {};


  // Decode scratch: kMaxBatchStreams component planes of 4096 int16 each (the
  // batch's streams are laid out consecutively, so a message's compose addresses
  // its own slice by its first stream index).
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
  // One staging arena per frame slot: the decode/compose dispatches read it, so a
  // slot that is still in flight must keep its arena intact (doc_agent/gfx-engine.md
  // §2.3 - the requirement for letting the CPU record a frame ahead).
  struct StageSlot {
    std::vector<StageBuffer> buffers;
    size_t index = 0;
  };
  StageSlot stageSlots[kSlots];
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
  // Tiles FreeRDP rejects before decoding because the tile's `quality` index is
  // outside the region's progressive quant table (its pixels and persistent state
  // are left alone); the engine must skip them the same way.
  uint64_t rfxRejectedTiles = 0;
  // Tiles whose `quality` is 0xFF, i.e. "full quality": the progressive quant table
  // is the all-zero one (see DecodeProgressive).
  uint64_t rfxFullQualityTiles = 0;
  // Progressive messages whose stream count exceeds the decode scratch (see
  // DecodeProgressive); must stay 0.
  uint64_t rfxBatchOverflow = 0;
  // Progressive tiles re-composited because FreeRDP's update_tiles re-stamps the
  // whole frame's tile list on every message (see DecodeProgressive).
  uint64_t restamped = 0;
  uint64_t rfxMultiRegion = 0;
  // StartFrame PDUs whose frame id repeated (FreeRDP keeps its tile list then).
  uint64_t frameIdRepeats = 0;
  uint64_t rfxOriginNonZero = 0;

  // --- Progressive decode batching ------------------------------------------
  // One Progressive message carries only a few hundred (tile,component) streams -
  // a handful of workgroups - which leaves the GPU starved: measured on the video
  // capture the decode's per-stream cost drops 3.3x when the stream count per
  // dispatch goes 278 -> 813. So the *decode* (and the inverse DWT that follows it)
  // of every message of a frame is merged into one dispatch pair, while the
  // composes stay per message and in message order.
  //
  // The rule that makes the merge safe: **a batch's messages must have pairwise
  // disjoint tile sets**.
  //  * a tile's decode refines persistent per-tile state (`cur`/`sign`/bit
  //    positions), so two decodes of the same tile inside one batch would race;
  //  * a message's compose re-composites the tiles decoded earlier *in its frame*
  //    (FreeRDP's update_tiles / our restamp jobs) from the persistent `cur`, so a
  //    tile decoded again later in the batch would feed that restamp "future"
  //    coefficients.
  // A message whose tiles intersect the batch, targets another surface, or would
  // overflow the coefficient scratch flushes the batch first. Frame changes and
  // any drain/present flush it too (see Flush/Compose/StartFrame), so a batch never
  // spans a frame boundary.
  struct BatchMessage {
    bool composeRegion = false;
    uint32_t tileCount = 0;
    uint32_t firstStream = 0;     // first stream index inside the merged coef
    uint32_t tileMetaOffset = 0;  // element offset into batchTileMeta
    uint32_t rectOffset = 0;      // element offset into batchRects
  };
  // The coefficient scratch is sized for this many streams (comp planes only; the
  // inverse DWT no longer needs a global temp).
  static constexpr uint32_t kMaxBatchStreams = 6144;
  // Spec-guaranteed minimum for maxComputeWorkGroupCount[x] (the device reports
  // exactly this): a linear dispatch may use at most this many workgroups per
  // axis, so anything larger has to be split over a 2-D/3-D grid.
  static constexpr uint32_t kMaxDispatchGroups = 65535;
  // Compose dispatches that had to be split (with the largest linear group count
  // they would have needed) - the evidence that a message really did exceed the
  // per-axis limit, kept visible instead of silently "working".
  uint64_t composeGridSplits = 0;
  uint64_t composeGroupsMax = 0;
  std::vector<BatchMessage> batchMessages;
  std::unordered_set<uint32_t> batchTiles;
  std::vector<uint8_t> batchMeta;      // host-side stream records (kMetaStride each)
  std::vector<uint32_t> batchTileMeta;
  std::vector<uint32_t> batchRects;
  std::vector<uint8_t> batchPayload;
  uint32_t batchStreams = 0;
  uint16_t batchSurfaceId = 0;
  bool batchOpen = false;
  uint64_t batchesFlushed = 0;
  uint64_t batchesSkipped = 0;
  uint64_t batchMessagesTotal = 0;
  uint64_t batchStreamsMax = 0;
  // Dev (perf): how many Progressive messages a frame actually carries. Batching
  // messages into one bigger decode dispatch only pays when frames carry several
  // of them (a frame's decodes are the maximum safe batch, see DecodeProgressive).
  uint32_t messagesThisFrame = 0;
  uint32_t messagesThisFrameMax = 0;
  static constexpr uint32_t kMessageBuckets = 6;  // 0,1,2,3,4-7,8+
  uint64_t messagesPerFrame[kMessageBuckets] = {0};
  void RecordFrameMessages() {
    const uint32_t n = messagesThisFrame;
    uint32_t bucket = 0;
    if (n == 0) {
      bucket = 0;
    } else if (n == 1) {
      bucket = 1;
    } else if (n == 2) {
      bucket = 2;
    } else if (n == 3) {
      bucket = 3;
    } else if (n < 8) {
      bucket = 4;
    } else {
      bucket = 5;
    }
    messagesPerFrame[bucket]++;
    if (n > messagesThisFrameMax) {
      messagesThisFrameMax = n;
    }
    messagesThisFrame = 0;
  }

  // Screen dirty rectangle (0xFF/0 initialised).
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
    // The part of `us` that was a SyncForCpuAccess() GPU drain: the class' own
    // CPU cost is `us - drainUs`.
    uint64_t drains = 0;
    uint64_t drainUs = 0;
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
  // Perf attribution (dev): driver-allocation cost, CPU copy volume and the
  // sync points the CPU-access path forces.
  uint64_t allocCalls = 0;
  uint64_t allocUs = 0;
  uint64_t cacheAllocCalls = 0;
  uint64_t cacheBytes = 0;
  uint64_t copyBytes = 0;
  uint64_t copyOverlapBytes = 0;
  uint64_t cacheStoreCalls = 0;
  uint64_t cacheRestoreCalls = 0;
  uint64_t syncDrains = 0;
  uint64_t syncDrainUs = 0;
  uint64_t releaseUs = 0;

  // --- Per-dispatch GPU time (dev perf) ------------------------------------
  // A timestamp pair around each compute dispatch, read back after the fence
  // wait. This is the honest way to attribute the GPU time: it changes nothing
  // about what is executed (unlike a "skip this dispatch" switch, whose side
  // effects also shift the work that follows it). Reported per kernel in Stats().
  static constexpr uint32_t kTimestampsPerDispatch = 2;
  static constexpr uint32_t kTimestampCapacity = 16384;  // queries (8192 brackets)
  // Per slot: one submission's queries are read back after *its* fence wait, so the
  // slots cannot share a pool (a reset while another slot's queries are pending would
  // discard them).
  VkQueryPool timestampPools[kSlots] = {};
  double timestampNsPerTick = 0.0;
  uint32_t timestampWrites[kSlots] = {};
  struct TimestampBracket {
    uint32_t start = 0;
    uint32_t slot = 0;  // 0 = RLGR decode, 1 = inverse DWT, 2 = compose, 3 = screen
  };
  std::vector<TimestampBracket> pendingTimestamps[kSlots];
  uint64_t gpuTicks[4] = {0, 0, 0, 0};
  uint64_t gpuSamples[4] = {0, 0, 0, 0};
  // Per-bracket extremes: a kernel whose per-dispatch time is constant while its
  // input size varies is dominated by waiting, not by arithmetic - the totals alone
  // cannot tell those apart (doc_agent/gfx-engine.md §6).
  uint64_t gpuTicksMin[4] = {UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
  uint64_t gpuTicksMax[4] = {0, 0, 0, 0};
  // Input size per chunk, so the decode can be expressed per payload byte / stream.
  uint64_t chunkBytes = 0;
  uint64_t chunkBytesMin = UINT64_MAX;
  uint64_t chunkBytesMax = 0;
  uint64_t streamCount = 0;
  uint64_t gpuTimestampDrops = 0;

  // Opens a GPU-time bracket around the dispatch that follows; returns the token
  // to close (UINT32_MAX when timestamps are unavailable / the pool is full).
  uint32_t TimestampOpen(uint32_t kind) {
    const int s = slot;
    if (timestampPools[s] == VK_NULL_HANDLE || api().CmdWriteTimestamp == nullptr ||
        timestampWrites[s] + kTimestampsPerDispatch > kTimestampCapacity) {
      if (timestampPools[s] != VK_NULL_HANDLE) {
        gpuTimestampDrops++;
      }
      return UINT32_MAX;
    }
    const uint32_t start = timestampWrites[s];
    timestampWrites[s] += kTimestampsPerDispatch;
    // Both ends at the compute stage: TOP_OF_PIPE -> BOTTOM_OF_PIPE brackets the
    // *whole* queue up to that point (including waiting for earlier work), which
    // made the per-kernel numbers overlap and overshoot the wall clock.
    api().CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestampPools[s],
                            start);
    TimestampBracket bracket;
    bracket.start = start;
    bracket.slot = kind;
    pendingTimestamps[s].push_back(bracket);
    return start;
  }

  void TimestampClose(uint32_t token) {
    if (token == UINT32_MAX) {
      return;
    }
    api().CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestampPools[slot],
                            token + 1);
  }

  // Called right after the fence wait: every query of the window has completed,
  // so the results need no wait bit.
  void CollectTimestamps(int s) {
    if (pendingTimestamps[s].empty()) {
      timestampWrites[s] = 0;
      return;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || vk.GetQueryPoolResults == nullptr) {
      pendingTimestamps[s].clear();
      timestampWrites[s] = 0;
      return;
    }
    const uint32_t first = pendingTimestamps[s].front().start;
    const uint32_t count = timestampWrites[s] - first;
    std::vector<uint64_t> ticks(count, 0);
    if (vk.GetQueryPoolResults(dev, timestampPools[s], first, count, ticks.size() * sizeof(uint64_t),
                               ticks.data(), sizeof(uint64_t),
                               VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (const TimestampBracket& bracket : pendingTimestamps[s]) {
        const uint32_t local = bracket.start - first;
        if (local + 1 >= count) {
          continue;
        }
        const uint64_t begin = ticks[local];
        const uint64_t end = ticks[local + 1];
        if (end >= begin) {
          const uint64_t delta = end - begin;
          gpuTicks[bracket.slot] += delta;
          gpuSamples[bracket.slot]++;
          if (delta < gpuTicksMin[bracket.slot]) {
            gpuTicksMin[bracket.slot] = delta;
          }
          if (delta > gpuTicksMax[bracket.slot]) {
            gpuTicksMax[bracket.slot] = delta;
          }
        }
      }
    }
    pendingTimestamps[s].clear();
    timestampWrites[s] = 0;
  }

  double GpuMs(uint32_t slot) const {
    return static_cast<double>(gpuTicks[slot]) * timestampNsPerTick / 1000000.0;
  }

  double GpuMsMin(uint32_t slot) const {
    return gpuTicksMin[slot] == UINT64_MAX
               ? 0.0
               : static_cast<double>(gpuTicksMin[slot]) * timestampNsPerTick / 1000000.0;
  }

  double GpuMsMax(uint32_t slot) const {
    return static_cast<double>(gpuTicksMax[slot]) * timestampNsPerTick / 1000000.0;
  }
  // Compose decisions: distinguishes "nothing mapped" from "nothing dirty".
  // `composeCopies` counts surface composes (one per surface per frame), while
  // `composeRects`/`composeMaxRects`/`composeRectOverflow` describe the merged
  // dirty rect list - the engine-side counterpart of the CPU present path's
  // `rectlist=`/`maxRects=`/`truncated=`. They are collected regardless of
  // kComposeRects so the list is quantified while the engine is still composing
  // the box.
  uint64_t composeCopies = 0;
  uint64_t composeSkipUnmapped = 0;
  uint64_t composeSkipClean = 0;
  uint64_t composeRects = 0;
  uint64_t composeMaxRects = 0;
  uint64_t composeRectOverflow = 0;
  // Perf accounting for the present strategy (doc_agent/gfx-engine.md §2.3/§3): the
  // host-time split of a present into "record the dirty compose", "submit it and
  // wait" and "blit the whole screen to the swapchain + present". The last bucket is
  // the per-frame cost that does *not* scale with the dirty area, so it decides which
  // strategy can win.
  uint64_t presentFrames = 0;
  uint64_t presentComposeUs = 0;
  uint64_t presentFlushUs = 0;
  uint64_t presentBlitUs = 0;

  // Dev (perf): what the CPU can actually do against each host-visible memory type
  // this device exposes. Every CPU-side pixel command (ClearCodec, cache store /
  // restore, SurfaceToSurface, SolidFill, uncompressed upload) runs on a mapped
  // buffer, so these numbers are the ceiling for the whole CPU-side model and
  // decide whether the bulk pixel moves belong on the device instead.
  std::string hostMemoryProbe;

  static double MbPerSec(size_t bytes, int64_t micros) {
    return micros > 0 ? static_cast<double>(bytes) / static_cast<double>(micros) : 0.0;
  }

  // Dev (perf): the fixed cost of one submit + fence wait with nothing recorded.
  // Every CPU-access command drains through this path, so the result separates
  // "the GPU is really executing" from "each synchronization point costs this
  // much" - the two need opposite fixes (less GPU work vs fewer sync points).
  uint64_t emptySubmitUs = 0;

  void ProbeSubmitCost() {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || vk.BeginCommandBuffer == nullptr ||
        vk.EndCommandBuffer == nullptr || vk.ResetCommandBuffer == nullptr) {
      return;
    }
    constexpr int kIterations = 20;
    int64_t total = 0;
    int done = 0;
    for (int i = 0; i < kIterations; ++i) {
      if (vk.ResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
        break;
      }
      VkCommandBufferBeginInfo begin{};
      begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (vk.BeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) {
        break;
      }
      if (vk.EndCommandBuffer(commandBuffer) != VK_SUCCESS) {
        break;
      }
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &commandBuffer;
      if (vk.ResetFences(dev, 1, &fence) != VK_SUCCESS) {
        break;
      }
      const int64_t start = NowUs();
      if (vk.QueueSubmit(VkContext::Instance().queue(), 1, &submit, fence) != VK_SUCCESS) {
        break;
      }
      if (vk.WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        break;
      }
      total += NowUs() - start;
      ++done;
    }
    if (done > 0) {
      emptySubmitUs = static_cast<uint64_t>(total) / static_cast<uint64_t>(done);
    }
    // Leave the fence signaled: Flush() resets it before its own submit.
    HMRDP_LOGI("vk submit probe: empty submit+wait = %{public}llu us (%{public}d samples)",
               static_cast<unsigned long long>(emptySubmitUs), done);
  }

  void RunDeviceProbes() {
    std::call_once(g_probeOnce, [this]() {
      ProbeHostMemory();
      ProbeSubmitCost();
      g_hostMemoryProbe = hostMemoryProbe;
      g_emptySubmitUs = emptySubmitUs;
    });
    hostMemoryProbe = g_hostMemoryProbe;
    emptySubmitUs = g_emptySubmitUs;
  }

  void ProbeHostMemory() {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || vk.GetPhysicalDeviceMemoryProperties == nullptr) {
      return;
    }
    constexpr size_t kBytes = 16u << 20;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kBytes;
    info.usage = kSurfaceBufferUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer probeBuffer = VK_NULL_HANDLE;
    if (vk.CreateBuffer(dev, &info, nullptr, &probeBuffer) != VK_SUCCESS) {
      return;
    }
    VkMemoryRequirements req{};
    vk.GetBufferMemoryRequirements(dev, probeBuffer, &req);
    VkPhysicalDeviceMemoryProperties memProps{};
    vk.GetPhysicalDeviceMemoryProperties(VkContext::Instance().physicalDevice(), &memProps);
    char entry[192];
    for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
      const VkMemoryPropertyFlags flags = memProps.memoryTypes[t].propertyFlags;
      if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
          (req.memoryTypeBits & (1u << t)) == 0) {
        continue;
      }
      VkMemoryAllocateInfo alloc{};
      alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      alloc.allocationSize = req.size;
      alloc.memoryTypeIndex = t;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      if (vk.AllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
        continue;
      }
      if (vk.BindBufferMemory(dev, probeBuffer, memory, 0) != VK_SUCCESS) {
        vk.FreeMemory(dev, memory, nullptr);
        continue;
      }
      void* mapped = nullptr;
      if (vk.MapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vk.FreeMemory(dev, memory, nullptr);
        continue;
      }
      uint8_t* base = static_cast<uint8_t*>(mapped);
      int64_t start = NowUs();
      std::memset(base, 0xA5, kBytes);
      const double writeMb = MbPerSec(kBytes, NowUs() - start);
      // Contiguous read+write, i.e. the best case any CPU copy can hope for.
      start = NowUs();
      std::memcpy(base + (kBytes / 4), base, kBytes / 4);
      const double copyMb = MbPerSec(kBytes / 4, NowUs() - start);
      // The engine's actual access shape: `rows` rows of `rowBytes`, stepped by a
      // desktop surface stride - how CpuCopyRows reads and writes.
      constexpr int kRows = 512;
      constexpr int kRowBytes = 64;
      constexpr int kStride = 12480;
      start = NowUs();
      for (int row = 0; row < kRows; ++row) {
        std::memcpy(base + kStride * 64 + row * kStride, base + row * kStride, kRowBytes);
      }
      const double stridedMb = MbPerSec(static_cast<size_t>(kRows) * kRowBytes, NowUs() - start);
      std::snprintf(entry, sizeof(entry), "t%u%s%s%s w=%.0f c=%.0f s=%.0f MB/s", t,
                    (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "|DL" : "",
                    (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "|CO" : "",
                    (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "|CA" : "", writeMb, copyMb,
                    stridedMb);
      if (!hostMemoryProbe.empty()) {
        hostMemoryProbe += "; ";
      }
      hostMemoryProbe += entry;
      vk.UnmapMemory(dev, memory);
      vk.FreeMemory(dev, memory, nullptr);
    }
    vk.DestroyBuffer(dev, probeBuffer, nullptr);
    HMRDP_LOGI("vk host memory probe: %{public}s", hostMemoryProbe.c_str());
  }

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
  // use-after-free or force a submit+wait per command (doc_agent/gfx-engine.md §3 "never
  // destroy in-flight resources", §7.3 "never wait per command").
  struct DeferredResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    bool mapped = false;
  };
  // Resources a recorded command buffer may still reference, released only when the
  // slot that recorded them has been waited: with two slots in flight, releasing on
  // *any* fence wait would destroy a buffer the other slot still reads.
  std::vector<DeferredResource> pendingDestroy[kSlots];

  void DeferGpuBuffer(GpuBuffer* buffer) {
    if (buffer == nullptr) {
      return;
    }
    if (buffer->buffer != VK_NULL_HANDLE || buffer->memory != VK_NULL_HANDLE) {
      DeferredResource deferred;
      deferred.buffer = buffer->buffer;
      deferred.bufferMemory = buffer->memory;
      deferred.mapped = (buffer->mapped != nullptr);
      pendingDestroy[slot].push_back(deferred);
    }
    *buffer = GpuBuffer{};
  }

  void ReleasePending(int s) {
    if (pendingDestroy[s].empty()) {
      return;
    }
    const int64_t releaseStart = NowUs();
    VkApi& vk = api();
    const VkDevice dev = device();
    for (const DeferredResource& deferred : pendingDestroy[s]) {
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
    pendingDestroy[s].clear();
    releaseUs += static_cast<uint64_t>(NowUs() - releaseStart);
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

  // Raw marks a surface may collect before the union box takes over. Well above
  // any measured frame (a full-screen Progressive I-frame marked ~2900 tiles), so
  // it only bounds a pathological stream; the *composed* count is bounded by
  // kMaxComposeRects after merging.
  static constexpr int kMaxRawDirtyRects = 8192;
  // Merged rects one Compose call will turn into copy regions (the same cap the
  // CPU present path uses for its upload; the box is the fallback above it).
  static constexpr int kMaxComposeRects = 256;

  // Whether Compose copies the merged dirty rects individually, the way the CPU
  // present path uploads its rects, instead of copying their union box.
  //
  // **Off: the rect path exposes a transfer-visibility defect of this platform.**
  // With the rect list one frame of the scrolling sample differs deterministically
  // by 156 px at (992,1728)-(1007,1791) - exactly one 64-byte cache line wide (the
  // device's nonCoherentAtomSize, 16 px) across 64 rows. The pixels are inside the
  // frame's rect list, and both the CPU and the *device* read the correct surface
  // bytes back there, so the decoded/composed content is right: the copy that
  // covers them reads the source stale. The box hides it because it re-composes the
  // same pixels on later frames (once the bytes are visible); the rect list
  // composes each pixel once, so a stale read becomes permanent. Measured on the
  // scrolling sample (doc_agent/gfx-engine.md §2.3, §7): box `bad=0`, rect
  // `bad=1 rgbPx=156`; an unconditional all-commands barrier before the copy cuts
  // it to 60 px but not to zero, and neither greedy host flushes, one copy call per
  // region, nor a coherent host-visible memory type change it. The box is itself a
  // dirty-region compose (it copies the marks' hull, not the screen), so leaving it
  // on satisfies the "the GPU presents the dirty region like the CPU does" rule at
  // no cost. Do not flip this on without moving the compose off the transfer path.
  static constexpr bool kComposeRects = false;

  // Marks one rect as changed. The rects themselves are kept (Compose composes
  // exactly those, like the CPU present path uploads its dirty rects); the union
  // box is maintained alongside as the bounded fallback.
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
    } else {
      if (left < m.dirtyLeft) m.dirtyLeft = left;
      if (top < m.dirtyTop) m.dirtyTop = top;
      if (right > m.dirtyRight) m.dirtyRight = right;
      if (bottom > m.dirtyBottom) m.dirtyBottom = bottom;
    }
    if (s.dirtyOverflow) {
      return;
    }
    if (s.dirtyRects.size() >= kMaxRawDirtyRects) {
      // Bounded: keep the box and stop collecting.
      s.dirtyOverflow = true;
      return;
    }
    s.dirtyRects.push_back(Surface::DirtyRect{left, top, right, bottom});
  }

  // Merges touching rects into the fewest exact rectangles: horizontal runs first
  // (same top/bottom, touching or overlapping), then vertical ones (identical
  // left/right, touching). The union is preserved exactly - no gap is ever
  // included - so the composed pixels are unchanged, while a grid of 64x64
  // Progressive tiles collapses into per-row strips and the region count drops far
  // below the cap. gdi gets the same effect from region16's band merging.
  static int CoalesceRects(Surface::DirtyRect* rects, int count) {
    if (count <= 1) {
      return count;
    }
    std::sort(rects, rects + count,
              [](const Surface::DirtyRect& a, const Surface::DirtyRect& b) {
                if (a.top != b.top) {
                  return a.top < b.top;
                }
                return a.left < b.left;
              });
    // Horizontal runs.
    int out = 0;
    for (int i = 0; i < count; ++i) {
      if (out > 0 && rects[out - 1].top == rects[i].top &&
          rects[out - 1].bottom == rects[i].bottom && rects[i].left <= rects[out - 1].right) {
        if (rects[i].right > rects[out - 1].right) {
          rects[out - 1].right = rects[i].right;
        }
        continue;
      }
      rects[out++] = rects[i];
    }
    count = out;
    // Vertical runs (the list is still ordered by top).
    std::sort(rects, rects + count,
              [](const Surface::DirtyRect& a, const Surface::DirtyRect& b) {
                if (a.left != b.left) {
                  return a.left < b.left;
                }
                return a.top < b.top;
              });
    out = 0;
    for (int i = 0; i < count; ++i) {
      if (out > 0 && rects[out - 1].left == rects[i].left &&
          rects[out - 1].right == rects[i].right && rects[i].top <= rects[out - 1].bottom) {
        if (rects[i].bottom > rects[out - 1].bottom) {
          rects[out - 1].bottom = rects[i].bottom;
        }
        continue;
      }
      rects[out++] = rects[i];
    }
    return out;
  }

  static void ClearSurfaceDirty(Surface& s) {
    s.meta.dirtyValid = false;
    s.dirtyOverflow = false;
    s.dirtyRects.clear();
  }

  void MarkScreenDirty(int left, int top, int right, int bottom) {
    if (right <= left || bottom <= top) {
      return;
    }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > pictureW) right = pictureW;
    if (bottom > pictureH) bottom = pictureH;
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

  // Prefer a DEVICE_LOCAL|HOST_VISIBLE type the CPU can actually stream through:
  // HOST_CACHED first (the CPU-side pixel paths are read-heavy), then COHERENT
  // (no explicit cache maintenance), then any host-visible type. Never assume a
  // pure DEVICE_LOCAL type exists (doc_agent/gfx-engine.md §3).
  uint32_t FindHostVisibleType(uint32_t typeBits) const {
    VkApi& vk = api();
    VkPhysicalDeviceMemoryProperties props{};
    if (vk.GetPhysicalDeviceMemoryProperties == nullptr) {
      return UINT32_MAX;
    }
    vk.GetPhysicalDeviceMemoryProperties(VkContext::Instance().physicalDevice(), &props);
    uint32_t best = UINT32_MAX;
    int bestScore = -1;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((typeBits & (1u << i)) == 0) {
        continue;
      }
      const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
      if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        continue;
      }
      int score = 0;
      // Temporary experiment (doc_agent/gfx-engine.md §7): skip the cached type so
      // the coherent one wins, to test whether the rect-mode compose staleness comes
      // from the cached (non-coherent) memory path. Correctness first; the CPU-side
      // pixel paths get measurably slower with the coherent type, so if this is the
      // answer the fix has to be scoped to the shadow compare, not the present path.
      if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
        score += 8;
      }
      if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
        score += 4;
      }
      if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
        score += 2;
      }
      if (score > bestScore) {
        bestScore = score;
        best = i;
      }
    }
    return best;
  }

  // Records the type actually chosen (and whether it needs explicit cache
  // maintenance). Called once at Init; the findings are also logged there.
  void NoteHostMemoryType(uint32_t typeIndex) {
    VkApi& vk = api();
    VkPhysicalDeviceMemoryProperties props{};
    if (typeIndex == UINT32_MAX || vk.GetPhysicalDeviceMemoryProperties == nullptr) {
      return;
    }
    vk.GetPhysicalDeviceMemoryProperties(VkContext::Instance().physicalDevice(), &props);
    const VkMemoryPropertyFlags flags = props.memoryTypes[typeIndex].propertyFlags;
    hostCoherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    std::string name = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "";
    name += name.empty() ? "" : "|";
    name += "HOST_VISIBLE";
    if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
      name += "|HOST_COHERENT";
    }
    if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
      name += "|HOST_CACHED";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), " (%u, %s)", typeIndex, name.c_str());
    hostMemoryType = buf;
  }

  // Host write -> GPU read. Coarse (one whole-memory range per buffer touched):
  // the driver writes back only the dirty lines, and the list stays tiny because
  // it is emptied on every submit.
  void FlushHostWrites() {
    if (hostCoherent) {
      dirtyHostMemories.clear();
      return;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || vk.FlushMappedMemoryRanges == nullptr) {
      dirtyHostMemories.clear();
      return;
    }
    invalidateScratch.clear();
    for (VkDeviceMemory memory : dirtyHostMemories) {
      if (memory == VK_NULL_HANDLE) {
        continue;
      }
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      invalidateScratch.push_back(range);
    }
    dirtyHostMemories.clear();
    // The Progressive staging arenas are read by compute too; flushing every arena
    // buffer (all slots) is cheaper than tracking them (there are only a handful),
    // and the driver itself only writes back dirty lines.
    for (StageSlot& arena : stageSlots) {
      for (StageBuffer& sb : arena.buffers) {
        if (sb.memory == VK_NULL_HANDLE || !sb.used) {
          continue;
        }
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = sb.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        invalidateScratch.push_back(range);
      }
    }
    if (!invalidateScratch.empty()) {
      vk.FlushMappedMemoryRanges(dev, static_cast<uint32_t>(invalidateScratch.size()),
                                 invalidateScratch.data());
    }
  }

  void MarkHostWrite(GpuBuffer* buffer) {
    if (buffer == nullptr || hostCoherent || buffer->memory == VK_NULL_HANDLE) {
      return;
    }
    // The list is emptied on every submit and a submit window only touches a
    // handful of distinct surface / state memories (cache entries are CPU-only),
    // so a linear duplicate check is cheaper than any bookkeeping flag.
    for (VkDeviceMemory memory : dirtyHostMemories) {
      if (memory == buffer->memory) {
        return;
      }
    }
    dirtyHostMemories.push_back(buffer->memory);
  }

  // Device write -> CPU read, restricted to the rect the CPU is about to touch
  // (invalidating the whole surface would move 4x the bytes of a typical band).
  void InvalidateRect(const GpuBuffer& buffer, int x, int y, int width, int height) {
    if (hostCoherent || buffer.memory == VK_NULL_HANDLE || width <= 0 || height <= 0) {
      return;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || vk.InvalidateMappedMemoryRanges == nullptr) {
      return;
    }
    const VkDeviceSize atom = atomSize != 0 ? atomSize : 1;
    const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(width) * 4;
    const VkDeviceSize rowStart = static_cast<VkDeviceSize>(x) * 4;
    invalidateScratch.clear();
    for (int row = 0; row < height; ++row) {
      const VkDeviceSize begin =
          static_cast<VkDeviceSize>(y + row) * static_cast<VkDeviceSize>(buffer.stride) + rowStart;
      VkDeviceSize end = begin + rowBytes;
      const VkDeviceSize alignedBegin = begin & ~(atom - 1);
      const VkDeviceSize alignedEnd = (end + atom - 1) & ~(atom - 1);
      if (!invalidateScratch.empty()) {
        VkMappedMemoryRange& last = invalidateScratch.back();
        if (last.offset + last.size >= alignedBegin) {
          last.size = (alignedEnd > last.offset + last.size)
                          ? (alignedEnd - last.offset)
                          : last.size;
          continue;
        }
      }
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = buffer.memory;
      range.offset = alignedBegin;
      // A range may not run past the allocation; VK_WHOLE_SIZE from an aligned
      // offset is the spec-blessed way to say "to the end".
      range.size = (alignedEnd > buffer.capacity) ? VK_WHOLE_SIZE : (alignedEnd - alignedBegin);
      invalidateScratch.push_back(range);
      if (range.size == VK_WHOLE_SIZE) {
        break;
      }
    }
    if (!invalidateScratch.empty()) {
      vk.InvalidateMappedMemoryRanges(dev, static_cast<uint32_t>(invalidateScratch.size()),
                                      invalidateScratch.data());
    }
  }

  void InvalidateWhole(const GpuBuffer& buffer) {
    if (hostCoherent || buffer.memory == VK_NULL_HANDLE) {
      return;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || vk.InvalidateMappedMemoryRanges == nullptr) {
      return;
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer.memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    vk.InvalidateMappedMemoryRanges(dev, 1, &range);
  }

  // Allocates, binds and persistently maps a linear buffer of `bytes`.
  bool AllocateMappedBuffer(size_t bytes, VkBufferUsageFlags usage, VkBuffer* outBuffer,
                            VkDeviceMemory* outMemory, void** outMapped) {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev == VK_NULL_HANDLE || bytes == 0) {
      return false;
    }
    const int64_t allocStart = NowUs();
    allocCalls++;
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
      allocUs += static_cast<uint64_t>(NowUs() - allocStart);
      return false;
    }
    if (hostMemoryType.empty()) {
      NoteHostMemoryType(type);
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vk.AllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
      vk.DestroyBuffer(dev, buffer, nullptr);
      allocUs += static_cast<uint64_t>(NowUs() - allocStart);
      return false;
    }
    if (vk.BindBufferMemory(dev, buffer, memory, 0) != VK_SUCCESS) {
      vk.FreeMemory(dev, memory, nullptr);
      vk.DestroyBuffer(dev, buffer, nullptr);
      allocUs += static_cast<uint64_t>(NowUs() - allocStart);
      return false;
    }
    void* mapped = nullptr;
    if (vk.MapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
      vk.FreeMemory(dev, memory, nullptr);
      vk.DestroyBuffer(dev, buffer, nullptr);
      allocUs += static_cast<uint64_t>(NowUs() - allocStart);
      return false;
    }
    allocUs += static_cast<uint64_t>(NowUs() - allocStart);
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
      pendingDestroy[slot].push_back(deferred);
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
      pendingDestroy[slot].push_back(deferred);
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
    // This slot's previous submission must be complete before its command buffer is
    // reset and the arena / descriptor pool it references are rewound: the wait that
    // used to happen at the present now happens here, when the slot is reused.
    if (slotInFlight[slot] && !WaitSlot()) {
      return false;
    }
    if (vk.ResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
      return false;
    }
    // A new command buffer only starts after the previous submission's fence wait
    // (Flush() waits; step 2 of the pipeline work makes these per slot), so the
    // staging arena can be rewound and the descriptor pool reset: neither is
    // referenced by any in-flight command buffer.
    ResetStage(slot);
    if (rfxPools[slot] != VK_NULL_HANDLE && vk.ResetDescriptorPool != nullptr) {
      vk.ResetDescriptorPool(device(), rfxPools[slot], 0);
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.BeginCommandBuffer(commandBuffer, &begin) != VK_SUCCESS) {
      return false;
    }
    recording = true;
    StoreSlot();
    // The timestamp queries are read back right after the fence wait, so this
    // window can start from a clean pool (doc_agent/gfx-engine.md §6; per slot once
    // submissions can overlap).
    pendingTimestamps[slot].clear();
    timestampWrites[slot] = 0;
    if (timestampPools[slot] != VK_NULL_HANDLE && vk.CmdResetQueryPool != nullptr) {
      vk.CmdResetQueryPool(commandBuffer, timestampPools[slot], 0, kTimestampCapacity);
    }
    return true;
  }

  // Makes every prior write visible to every later read. Deliberately coarse:
  // correctness over barrier count (doc_agent/gfx-engine.md §3). The host leg is required
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
    // sync points - see doc_agent/gfx-engine.md §3).
    if (!AnyComputeInFlight()) {
      return true;
    }
    const int64_t drainStart = NowUs();
    // Wait *every* in-flight slot: another slot's dispatch may be writing the
    // surface this CPU command is about to read-modify-write.
    const bool ok = FlushAll();
    syncDrains++;
    syncDrainUs += static_cast<uint64_t>(NowUs() - drainStart);
    return ok;
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

  // Ends and submits the current slot. Deliberately does NOT wait: the CPU-readback
  // paths call Flush() (which waits), while the present path only has to hand the
  // slot to the presenter - that is what keeps the decode off the present's
  // critical path (doc_agent/gfx-engine.md §2.3, §7).
  bool SubmitSlot(bool signalFrame) {
    // A pending decode batch must be recorded (its composes included) before this
    // window is closed: the batch's staged data lives in the arena, which the next
    // command buffer rewinds.
    if (!FinishDecodeBatch()) {
      return false;
    }
    if (!recording) {
      return true;
    }
    VkApi& vk = api();
    const VkDevice dev = device();
    recording = false;
    StoreSlot();
    if (pendingDeviceWrites || slotComputeInFlight[slot]) {
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
      if (slotComputeInFlight[slot]) {
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
    // Device-side chain (doc_agent/gfx-engine.md §2.3). Two dependencies, both on
    // the GPU:
    //   * the *engine's own* previous submission: the surfaces and the decode
    //     scratch are read-modify-written across frames, so frames must stay
    //     ordered on the device. (Ordering via the presenter's blit - as it used
    //     to be - also serialized compose(N) against blit(N-1), which this removes.)
    //   * the blit that last sampled the picture *this* submission reuses (two
    //     frames ago, same slot): a picture must not be overwritten while the
    //     presenter is still reading it.
    const int prevSlot = (slot + kSlots - 1) % kSlots;
    VkSemaphore waits[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t waitCount = 0;
    if (engineChainPending[prevSlot]) {
      waits[waitCount++] = engineChain[prevSlot];
      engineChainPending[prevSlot] = false;
    }
    if (blitDonePending[slot]) {
      waits[waitCount++] = blitDone[slot];
      blitDonePending[slot] = false;
    }
    VkPipelineStageFlags waitStages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT |
                                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT |
                                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    if (waitCount > 0) {
      submit.waitSemaphoreCount = waitCount;
      submit.pWaitSemaphores = waits;
      submit.pWaitDstStageMask = waitStages;
    }
    // Signals: the chain token for the next engine submission (always - even a
    // Flush() submission has to order the next frame after itself) and, for a frame
    // the presenter is going to blit, the token that blit waits.
    VkSemaphore signals[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint32_t signalCount = 0;
    if (engineChain[slot] != VK_NULL_HANDLE) {
      signals[signalCount++] = engineChain[slot];
      engineChainPending[slot] = true;
    }
    if (signalFrame && frameSemaphores[slot] != VK_NULL_HANDLE) {
      signals[signalCount++] = frameSemaphores[slot];
    }
    if (signalCount > 0) {
      submit.signalSemaphoreCount = signalCount;
      submit.pSignalSemaphores = signals;
    }
    // Host writes the GPU is about to read must be in the memory domain first: a
    // cached (non-coherent) host-visible type needs an explicit flush, the
    // pipeline barrier alone does not write the CPU cache lines back.
    FlushHostWrites();
    vk.ResetFences(dev, 1, &fence);
    if (vk.QueueSubmit(VkContext::Instance().queue(), 1, &submit, fence) != VK_SUCCESS) {
      return false;
    }
    ++submits;
    slotInFlight[slot] = true;
    lastSubmittedSlot = slot;
    return true;
  }

  // Waits the current slot's fence and does everything that needs the submission to
  // be complete: make the device writes visible to the host, read back the
  // timestamp queries and release the deferred resources.
  bool WaitSlot(int s) {
    VkApi& vk = api();
    const VkDevice dev = device();
    const int64_t waitStart = NowUs();
    if (vk.WaitForFences(dev, 1, &fences[s], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
    // Conservative: whatever was written is now complete, but a barrier is still
    // emitted before the next command buffer's first read. The compute writes
    // have executed too, so a following CPU access sees the real pixels.
    slotInFlight[s] = false;
    slotComputeInFlight[s] = false;
    pendingDeviceWrites = true;
    flushWaitUs += static_cast<uint64_t>(NowUs() - waitStart);
    // The queries of this submission have completed, so their GPU times can be
    // accumulated without a wait bit (doc_agent/gfx-engine.md §6).
    CollectTimestamps(s);
    // The submission is complete, so everything this slot deferred is safe to
    // destroy (the other slot's list stays untouched).
    ReleasePending(s);
    return true;
  }

  bool WaitSlot() { return WaitSlot(slot); }

  // Every in-flight slot must be waited before the CPU touches memory any of them
  // may still write (a CPU readback / read-modify-write).
  bool WaitAllInFlight() {
    for (int s = 0; s < kSlots; ++s) {
      if (slotInFlight[s] && !WaitSlot(s)) {
        return false;
      }
    }
    return true;
  }

  // Submit + wait: the semantics every CPU-readback path needs (surface pixels the
  // CPU is about to read, the screen readback, teardown). The next recording then
  // uses the other slot, whose fence is already signaled.
  bool Flush() {
    if (!SubmitSlot(false)) {
      return false;
    }
    if (!WaitSlot()) {
      return false;
    }
    SelectSlot((slot + 1) % kSlots);
    return true;
  }

  // CPU readback: everything recorded, by any slot, must be complete.
  bool FlushAll() {
    if (!SubmitSlot(false)) {
      return false;
    }
    if (!WaitAllInFlight()) {
      return false;
    }
    SelectSlot((slot + 1) % kSlots);
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

  // Fills one copy region for a surface rect -> screen position pair. The surface
  // stride goes into `bufferRowLength`, so the desktop size does not have to be a
  // multiple of anything. Returns false when the rect is degenerate.
  bool MakeScreenCopyRegion(const GpuSurface& m, const GpuBuffer& src, int left, int top,
                            int right, int bottom, int scrW, int scrH,
                            VkBufferImageCopy* out) {
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > m.mappedWidth) right = m.mappedWidth;
    if (bottom > m.mappedHeight) bottom = m.mappedHeight;
    if (right <= left || bottom <= top) {
      return false;
    }
    // 1:1 output mapping (scaled PDUs unmap the surface); a rect copy.
    int dstX = static_cast<int>(m.outputX) + left;
    int dstY = static_cast<int>(m.outputY) + top;
    if (dstX < 0) dstX = 0;
    if (dstY < 0) dstY = 0;
    if (dstX >= scrW || dstY >= scrH) {
      return false;
    }
    int dstW = right - left;
    int dstH = bottom - top;
    if (dstW > scrW - dstX) dstW = scrW - dstX;
    if (dstH > scrH - dstY) dstH = scrH - dstY;
    if (dstW <= 0 || dstH <= 0) {
      return false;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = static_cast<VkDeviceSize>(top) * static_cast<VkDeviceSize>(src.stride) +
                          static_cast<VkDeviceSize>(left) * 4;
    region.bufferRowLength = static_cast<uint32_t>(src.stride / 4);
    region.bufferImageHeight = 0;
    region.imageSubresource = ColorLayers();
    region.imageOffset = {dstX, dstY, 0};
    region.imageExtent.width = static_cast<uint32_t>(dstW);
    region.imageExtent.height = static_cast<uint32_t>(dstH);
    region.imageExtent.depth = 1;
    *out = region;
    MarkScreenDirty(dstX, dstY, dstX + dstW, dstY + dstH);
    return true;
  }

  // Compose: copies the clipped dirty rects out of a persistently mapped surface
  // buffer into the screen image. One command buffer, one call per surface: all
  // regions share the same source buffer and destination image.
  bool CopyBufferRegionsToScreen(const GpuBuffer& src, VkImage target,
                                 const VkBufferImageCopy* regions, int count) {
    if (count <= 0) {
      return true;
    }
    if (!src.valid() || target == VK_NULL_HANDLE) {
      return false;
    }
    if (!EnsureRecording()) {
      return false;
    }
    BarrierBeforeRead();
    api().CmdCopyBufferToImage(commandBuffer, src.buffer, target, VK_IMAGE_LAYOUT_GENERAL,
                               static_cast<uint32_t>(count), regions);
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

  bool GrowStage(int s, size_t minBytes) {
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
    stageSlots[s].buffers.push_back(sb);
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
    StageSlot& arena = stageSlots[slot];
    for (;;) {
      if (arena.index >= arena.buffers.size()) {
        if (!GrowStage(slot, bytes)) {
          return false;
        }
      }
      StageBuffer& sb = arena.buffers[arena.index];
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
      arena.index++;
    }
  }

  void ResetStage(int s) {
    stageSlots[s].index = 0;
    for (StageBuffer& sb : stageSlots[s].buffers) {
      sb.used = 0;
    }
  }

  void DestroyStage() {
    VkApi& vk = api();
    const VkDevice dev = device();
    for (StageSlot& arena : stageSlots) {
    for (StageBuffer& sb : arena.buffers) {
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
    arena.buffers.clear();
    arena.index = 0;
    }
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

    // 0 tileMeta, 1 coef, 2 surface, 3 clip rects, 4 the per-stream bit-position
    // buffer (its spare bytes carry the UPGRADE length verdict, see
    // rfx_decode.comp: a tile FreeRDP rejects must not be composited at all).
    VkDescriptorSetLayoutBinding composeBindings[5] = {};
    for (int i = 0; i < 5; ++i) {
      composeBindings[i].binding = static_cast<uint32_t>(i);
      composeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      composeBindings[i].descriptorCount = 1;
      composeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo composeLayoutInfo{};
    composeLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    composeLayoutInfo.bindingCount = 5;
    composeLayoutInfo.pBindings = composeBindings;
    if (vk.CreateDescriptorSetLayout(dev, &composeLayoutInfo, nullptr, &composeSetLayout) !=
        VK_SUCCESS) {
      return false;
    }

    VkDescriptorSetLayoutBinding idwtBindings[1] = {};
    idwtBindings[0].binding = 0;
    idwtBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    idwtBindings[0].descriptorCount = 1;
    idwtBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo idwtLayoutInfo{};
    idwtLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    idwtLayoutInfo.bindingCount = 1;
    idwtLayoutInfo.pBindings = idwtBindings;
    if (vk.CreateDescriptorSetLayout(dev, &idwtLayoutInfo, nullptr, &idwtSetLayout) != VK_SUCCESS) {
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
    // numTiles, compBase, surfaceW/H, the four colour coefficients, the (unused)
    // uSwapRb the shader still declares, gridW.
    composeRange.size = 10 * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo composePipeInfo{};
    composePipeInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    composePipeInfo.setLayoutCount = 1;
    composePipeInfo.pSetLayouts = &composeSetLayout;
    composePipeInfo.pushConstantRangeCount = 1;
    composePipeInfo.pPushConstantRanges = &composeRange;
    if (vk.CreatePipelineLayout(dev, &composePipeInfo, nullptr, &composePipeLayout) != VK_SUCCESS) {
      return false;
    }

    VkPushConstantRange idwtRange{};
    idwtRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    idwtRange.offset = 0;
    idwtRange.size = 2 * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo idwtPipeInfo{};
    idwtPipeInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    idwtPipeInfo.setLayoutCount = 1;
    idwtPipeInfo.pSetLayouts = &idwtSetLayout;
    idwtPipeInfo.pushConstantRangeCount = 1;
    idwtPipeInfo.pPushConstantRanges = &idwtRange;
    if (vk.CreatePipelineLayout(dev, &idwtPipeInfo, nullptr, &idwtPipeLayout) != VK_SUCCESS) {
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
    if (!makePipeline(kRfxIdwtSpv, kRfxIdwtSpvWords, idwtPipeLayout, &idwtPipe)) {
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
    for (int s = 0; s < kSlots; ++s) {
      if (vk.CreateDescriptorPool(dev, &poolInfo, nullptr, &rfxPools[s]) != VK_SUCCESS) {
        return false;
      }
    }
    return true;
  }

  bool AllocSet(VkDescriptorSetLayout layout, VkDescriptorSet* out) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = rfxPools[slot];
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
  MarkHostWrite(&created.cur);
  MarkHostWrite(&created.sign);
  MarkHostWrite(&created.bp);
  HostWrote();
    *state = created;
    return true;
  }

  // Decodes one Progressive ("WBT") message into the surface buffer: parse the
  // container on the CPU, then for each chunk of <=512 tiles run the decode then
  // the YCbCr compose compute dispatch.
  // Appends one message's prepared decode work (its stream records, tile meta and
  // tile-local clip rects, plus the raw payload they point into) to the frame's
  // batch. `tileKeys` are the surface tile indices this message advances; they are
  // what enforces the disjointness rule above. The payload offsets inside `meta`
  // are shifted here to the merged payload base.
  bool AppendToDecodeBatch(uint16_t surfaceId, bool composeRegion,
                           const std::vector<uint32_t>& tileKeys, std::vector<uint8_t> meta,
                           std::vector<uint32_t> tileMeta, std::vector<uint32_t> rects,
                           const uint8_t* payload, size_t payloadSize) {
    const uint32_t streams = static_cast<uint32_t>(meta.size() / kMetaStride);
    bool intersects = false;
    for (uint32_t key : tileKeys) {
      if (batchTiles.find(key) != batchTiles.end()) {
        intersects = true;
        break;
      }
    }
    if (batchOpen && (surfaceId != batchSurfaceId || intersects ||
                      batchStreams + streams > kMaxBatchStreams)) {
      if (!FinishDecodeBatch()) {
        return false;
      }
    }
    if (!batchOpen) {
      batchOpen = true;
      batchSurfaceId = surfaceId;
      batchStreams = 0;
    }

    const uint32_t payloadBase = static_cast<uint32_t>(batchPayload.size());
    if (payloadSize > 0) {
      batchPayload.insert(batchPayload.end(), payload, payload + payloadSize);
    }
    // Shift the three payload-relative byte offsets onto the merged payload base.
    for (uint32_t s = 0; s < streams; ++s) {
      uint8_t* rec = &meta[static_cast<size_t>(s) * kMetaStride];
      const size_t fields[3] = {8u, 16u, 24u};
      for (size_t f = 0; f < 3; ++f) {
        uint32_t value = 0;
        std::memcpy(&value, rec + fields[f], 4);
        value += payloadBase;
        std::memcpy(rec + fields[f], &value, 4);
      }
    }

    BatchMessage message;
    message.composeRegion = composeRegion;
    message.tileCount = static_cast<uint32_t>(tileMeta.size() / 4);
    message.firstStream = batchStreams;
    message.tileMetaOffset = static_cast<uint32_t>(batchTileMeta.size());
    message.rectOffset = static_cast<uint32_t>(batchRects.size());

    batchMeta.insert(batchMeta.end(), meta.begin(), meta.end());
    batchTileMeta.insert(batchTileMeta.end(), tileMeta.begin(), tileMeta.end());
    batchRects.insert(batchRects.end(), rects.begin(), rects.end());
    // The compose reads tile-local rect *offsets*, so shift them by the pool base.
    for (uint32_t t = 0; t < message.tileCount; ++t) {
      batchTileMeta[message.tileMetaOffset + t * 4 + 2] += message.rectOffset;
    }
    batchMessages.push_back(message);
    for (uint32_t key : tileKeys) {
      batchTiles.insert(key);
    }
    batchStreams += streams;

    // Input-volume accounting: the decode time means nothing without its denominator.
    for (uint32_t s = 0; s < streams; ++s) {
      const uint8_t* rec = &meta[static_cast<size_t>(s) * kMetaStride];
      uint32_t payloadLen = 0;
      uint32_t srlLen = 0;
      uint32_t rawLen = 0;
      std::memcpy(&payloadLen, rec + 12, 4);
      std::memcpy(&srlLen, rec + 20, 4);
      std::memcpy(&rawLen, rec + 28, 4);
      chunkBytes += payloadLen + srlLen + rawLen;
    }
    streamCount += streams;
    if (batchStreams > batchStreamsMax) {
      batchStreamsMax = batchStreams;
    }
    // One message per dispatch: record it now (the decode is recorded, and the
    // matching inverse DWT plus the composes follow in FinishDecodeBatch).
    return FinishDecodeBatch();
  }

  // Records one Progressive message's decode dispatch and its inverse DWT, then the
  // message's compose (which borrows its coefficients from the decode plane).
  bool FinishDecodeBatch() {
    if (!batchOpen || batchMessages.empty()) {
      return true;
    }
    VkApi& vk = api();
    Surface* surface = Find(batchSurfaceId);
    const uint32_t streams = batchStreams;
    // "Nothing to record" is not a failure: returning false here would make the
    // caller's Flush() bail out *without submitting*, which surfaces as a present
    // failure and a starved pipeline (measured: only 3 of 44 decodes completed).
    if (surface == nullptr || !surface->gpu.valid() || !surface->rfx.valid() || streams == 0 ||
        !EnsureRecording()) {
      batchMessages.clear();
      batchTiles.clear();
      batchMeta.clear();
      batchTileMeta.clear();
      batchRects.clear();
      batchPayload.clear();
      batchStreams = 0;
      batchOpen = false;
      batchesSkipped++;
      return true;
    }
    const bool ok = true;
    if (ok) {
      VkBuffer payloadBuffer = VK_NULL_HANDLE;
      VkDeviceSize payloadOffset = 0;
      VkBuffer metaBuffer = VK_NULL_HANDLE;
      VkDeviceSize metaOffset = 0;
      VkBuffer tileMetaBuffer = VK_NULL_HANDLE;
      VkDeviceSize tileMetaOffset = 0;
      VkBuffer rectBuffer = VK_NULL_HANDLE;
      VkDeviceSize rectOffset = 0;
      const bool staged =
          StageAppend(batchPayload.data(), batchPayload.size(), &payloadBuffer, &payloadOffset) &&
          StageAppend(batchMeta.data(), batchMeta.size(), &metaBuffer, &metaOffset) &&
          StageAppend(batchTileMeta.data(), batchTileMeta.size() * sizeof(uint32_t), &tileMetaBuffer,
                      &tileMetaOffset) &&
          (batchRects.empty() ||
           StageAppend(batchRects.data(), batchRects.size() * sizeof(uint32_t), &rectBuffer,
                       &rectOffset));
      VkDescriptorSet decodeSet = VK_NULL_HANDLE;
      VkDescriptorSet idwtSet = VK_NULL_HANDLE;
      if (staged && AllocSet(decodeSetLayout, &decodeSet) && AllocSet(idwtSetLayout, &idwtSet)) {
        WriteBuffer(decodeSet, 0, payloadBuffer, payloadOffset, batchPayload.size());
        WriteBuffer(decodeSet, 1, metaBuffer, metaOffset, batchMeta.size());
        WriteBuffer(decodeSet, 2, coef.buffer, 0, VK_WHOLE_SIZE);
        WriteBuffer(decodeSet, 3, surface->rfx.cur.buffer, 0, VK_WHOLE_SIZE);
        WriteBuffer(decodeSet, 4, surface->rfx.sign.buffer, 0, VK_WHOLE_SIZE);
        WriteBuffer(decodeSet, 5, surface->rfx.bp.buffer, 0, VK_WHOLE_SIZE);
        BarrierBeforeCompute();
        vk.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipe);
        vk.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipeLayout, 0,
                                 1, &decodeSet, 0, nullptr);
        struct DecodePush {
          uint32_t numStreams;
          uint32_t compBase;
          uint32_t tempBase;
        } dpush{streams, 0u, streams * 4096u};
        vk.CmdPushConstants(commandBuffer, decodePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof(dpush), &dpush);
        const uint32_t decodeToken = TimestampOpen(0);
        vk.CmdDispatch(commandBuffer, (streams + 63u) / 64u, 1, 1);
        TimestampClose(decodeToken);

        WriteBuffer(idwtSet, 0, coef.buffer, 0, VK_WHOLE_SIZE);
        BarrierBeforeCompute();
        vk.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, idwtPipe);
        vk.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, idwtPipeLayout, 0, 1,
                                 &idwtSet, 0, nullptr);
        struct IdwtPush {
          uint32_t numStreams;
          uint32_t compBase;
        } ipush{streams, 0u};
        vk.CmdPushConstants(commandBuffer, idwtPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof(ipush), &ipush);
        const uint32_t idwtToken = TimestampOpen(1);
        vk.CmdDispatch(commandBuffer, streams, 1, 1);
        TimestampClose(idwtToken);
        pendingComputeWrites = true;
        slotComputeInFlight[slot] = true;
        for (const BatchMessage& message : batchMessages) {
          if (!message.composeRegion || message.tileCount == 0) {
            continue;
          }
          VkDescriptorSet composeSet = VK_NULL_HANDLE;
          if (!AllocSet(composeSetLayout, &composeSet)) {
            break;
          }
          const uint32_t rectBytes =
              static_cast<uint32_t>(batchRects.size() * sizeof(uint32_t));
          WriteBuffer(composeSet, 0, tileMetaBuffer,
                      tileMetaOffset + static_cast<VkDeviceSize>(message.tileMetaOffset) * 4u,
                      static_cast<VkDeviceSize>(message.tileCount) * 4u * sizeof(uint32_t));
          WriteBuffer(composeSet, 1, coef.buffer, 0, VK_WHOLE_SIZE);
          WriteBuffer(composeSet, 2, surface->gpu.buffer, 0, VK_WHOLE_SIZE);
          WriteBuffer(composeSet, 3, rectBuffer != VK_NULL_HANDLE ? rectBuffer : metaBuffer,
                      rectBuffer != VK_NULL_HANDLE ? rectOffset : metaOffset,
                      rectBytes == 0 ? 4u : rectBytes);
          // The UPGRADE length verdict of each of the tile's three streams lives in
          // the spare bytes of its bit-position entry; a tile with a rejected stream
          // is not composited (FreeRDP never writes that tile's pixels).
          WriteBuffer(composeSet, 4, surface->rfx.bp.buffer, 0, VK_WHOLE_SIZE);
          BarrierBeforeCompute();
          vk.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, composePipe);
          vk.CmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, composePipeLayout,
                                   0, 1, &composeSet, 0, nullptr);
          struct ComposePush {
            uint32_t numTiles;
            uint32_t compBase;
            int32_t surfaceW;
            int32_t surfaceH;
            int32_t kr;
            int32_t kcrG;
            int32_t kcbG;
            int32_t kcbB;
            uint32_t swapRb;  // uSwapRb: must stay in the layout; always 0
            uint32_t gridW;  // tiles per row: tile index from the tile's pixel origin
          } cpush{message.tileCount, message.firstStream * 4096u, surface->meta.width,
                  surface->meta.height, kKr, kKcrG, kKcbG, kKcbB, 0u /* uSwapRb: storage is BGRA */,
                  static_cast<uint32_t>(surface->meta.gridW)};
          vk.CmdPushConstants(commandBuffer, composePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                              sizeof(cpush), &cpush);
          // One invocation per pixel (local_size_x = 64), so a *linear* dispatch is
          // `tileCount * 64` workgroups. A full-screen Progressive message carries
          // 1000+ tiles and thus needs > 65535 workgroups, which is past the
          // spec-guaranteed `maxComputeWorkGroupCount` (65535 on this device): the
          // dispatch is invalid and the platform silently ran only part of it, so
          // the tail of the message - the desktop's lower tile rows - was never
          // composited and kept the previous frame's pixels. Split the grid in 2-D
          // and let the shader rebuild the linear invocation index from
          // gl_NumWorkGroups (a browse-scale capture never exceeds the limit, which
          // is why this stayed hidden).
          const uint32_t groups = (message.tileCount * kTilePixels + 63u) / 64u;
          uint32_t gx = groups;
          uint32_t gy = 1;
          if (gx > kMaxDispatchGroups) {
            gx = kMaxDispatchGroups;
            gy = (groups + gx - 1u) / gx;
            composeGridSplits++;
            if (groups > composeGroupsMax) {
              composeGroupsMax = groups;
            }
          }
          const uint32_t composeToken = TimestampOpen(2);
          vk.CmdDispatch(commandBuffer, gx, gy, 1);
          TimestampClose(composeToken);
          pendingComputeWrites = true;
          slotComputeInFlight[slot] = true;
        }
        batchesFlushed++;
        batchMessagesTotal += batchMessages.size();
      }
    }
    batchMessages.clear();
    batchTiles.clear();
    batchMeta.clear();
    batchTileMeta.clear();
    batchRects.clear();
    batchPayload.clear();
    batchStreams = 0;
    batchOpen = false;
    return ok;
  }

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
    messagesThisFrame++;
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
    // The compose clip is FreeRDP's `clippingRects`: update_tiles unions the
    // region's rects with region16_union_rect(), which is a *band/coalescing*
    // union (it merges items overlapping a band into one bounding-box rect, so it
    // also covers the gaps between them). It is built from the REGION header, not
    // from the tiles - a Progressive message can carry a region with zero tiles,
    // and FreeRDP still re-composites its whole frame tile list with that clip.
    auto buildMsgClip = [&](const RfxRegionRef& region) {
      if (msgClipReady || region.rects == nullptr || region.numRects == 0) {
        return;
      }
      REGION16 clip;
      region16_init(&clip);
      for (uint16_t ri = 0; ri < region.numRects; ++ri) {
        const RfxRect& r = region.rects[ri];
        // Rects are relative to the command's destRect origin, exactly like
        // FreeRDP's gdi (update_tiles: clippingRect.left = nXDst + rect->x).
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
        const uint32_t rx = merged[i].left;
        const uint32_t ry = merged[i].top;
        rectPool.push_back(rx | (ry << 16));
        rectPool.push_back(
            (static_cast<uint32_t>(merged[i].right - merged[i].left)) |
            (static_cast<uint32_t>(merged[i].bottom - merged[i].top) << 16));
      }
      region16_uninit(&clip);
      msgClipCount = static_cast<uint32_t>(msgRects.size() / 4);
      msgClipReady = true;
    };
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
          job.rectOffset = 0;
          job.rectCount = msgClipCount;
          const RfxQuant* qv[3] = {&t.quants[t.quantIdxY], &t.quants[t.quantIdxCb],
                                   &t.quants[t.quantIdxCr]};
          const uint32_t tileIndex = static_cast<uint32_t>(t.yIdx) * static_cast<uint32_t>(gridW) +
                                     t.xIdx;
          // FreeRDP registers a tile in the frame's updated-tile list while *reading*
          // the block and only decodes it afterwards (progressive_process_tiles), so a
          // tile whose decode is rejected below still joins that list and is
          // re-composited - from its old state - by a later message of the frame.
          msgTiles.push_back(tileIndex);
          // FreeRDP's per-tile progressive-quant lookup (progressive.c,
          // progressive_decompress_tile_first / _tile_upgrade):
          //   * quality == 0xFF selects `quantProgValFull`, a fixed all-zero table
          //     (only its `quality` field is set), i.e. "no progressive offset";
          //   * a quality index outside the region's table makes FreeRDP **reject the
          //     tile**: it returns before touching `current` / `sign` / the bit
          //     positions, so the tile keeps its previous pixels *and* its persistent
          //     state - while the tile is still part of the frame's updated-tile list,
          //     so a later message of the same frame re-composites it from that old
          //     state.
          // Decoding such a tile anyway (with a zero table) is a silent divergence:
          // the tile still gets plausible pixels, but every later refinement of it
          // starts from the wrong `sign` / bit positions - which is exactly what a
          // sparse, tile-sized colour drift looks like.
          RfxQuant prog[3];
          if (t.quality != 0xFF) {
            if (t.progQuants == nullptr || t.numProgQuant == 0 || t.quality >= t.numProgQuant) {
              rfxRejectedTiles++;
              return;  // registered in msgTiles only: no decode, no compose, no state
            }
            prog[0] = t.progQuants[t.quality].y;
            prog[1] = t.progQuants[t.quality].cb;
            prog[2] = t.progQuants[t.quality].cr;
          } else {
            rfxFullQualityTiles++;
          }
          const uint8_t* data[3] = {t.yData, t.cbData, t.crData};
          const uint16_t len[3] = {t.yLen, t.cbLen, t.crLen};
          const uint8_t* srl[3] = {t.ySrlData, t.cbSrlData, t.crSrlData};
          const uint16_t srlLen[3] = {t.ySrlLen, t.cbSrlLen, t.crSrlLen};
          const uint8_t* raw[3] = {t.yRawData, t.cbRawData, t.crRawData};
          const uint16_t rawLen[3] = {t.yRawLen, t.cbRawLen, t.crRawLen};
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
        &stats, &rfxState, buildMsgClip);
    if (!parsed) {
      // FreeRDP rejects the whole message on a malformed / invalid region header
      // (tileSize, numRects < 1, numQuant > 7, quant nibbles outside [6,15], ...)
      // and decodes nothing at all - including the per-tile state. Decoding the
      // tiles that happened to parse would make the surface diverge from gdi.
      rfxParseErrors += stats.errors;
      // Dev: name the stage (see RfxParseStats::errorStage). A region *header*
      // rejection is nothing-but-reject on both sides, while a failure during the
      // tile walk happens in FreeRDP only *after* the tiles read so far were
      // registered in the surface's frame tile list.
      HMRDP_LOGW("vk progressive parse FAIL stage=%{public}s tiles=%{public}u regions=%{public}u "
                 "bytes=%{public}zu",
                 stats.errorStage != nullptr ? stats.errorStage : "?",
                 static_cast<unsigned>(stats.tiles), static_cast<unsigned>(stats.regions), size);
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
    if (kRestampEnabled && msgRectCount > 0 && !surface->frameTiles.empty()) {
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

    // Build this message's decode work once and hand it to the frame's batch: the
    // batch is what sets the dispatch size now (one Progressive message alone only
    // carries a few hundred streams, which leaves the GPU starved).
    const uint32_t tileCount = static_cast<uint32_t>(tiles.size());
    const uint32_t streams = tileCount * 3;
    // A message with more tiles than the decode scratch holds would write past the
    // end of the coefficient planes (the shader's clamped writes then corrupt the
    // tail tiles *silently*). No capture has come near it - the largest message seen
    // is 1422 tiles / 4266 streams against 6144 - but a 4K desktop with a full-screen
    // region (~2074 tiles / 6222 streams) would. Counted loudly so it can never be
    // silent; the fix is to append the message's tiles in chunks (the batching rule
    // already flushes before an overflow, and the per-tile clip rects are
    // independent of the chunking).
    if (streams > kMaxBatchStreams) {
      rfxBatchOverflow++;
      HMRDP_LOGE("vk progressive: message needs %{public}u streams, scratch holds %{public}u",
                 streams, kMaxBatchStreams);
    }
    std::vector<uint8_t> meta(static_cast<size_t>(streams) * kMetaStride, 0);
    std::vector<uint32_t> tileMeta(static_cast<size_t>(tileCount) * 4, 0);
    // The compositor's clip rects, per tile, in *tile-local* pixel coordinates
    // (x0 | y0 << 8 | x1 << 16 | y1 << 24). Only the rects that actually intersect
    // the tile are kept, so the kernel's per-pixel probe is one byte extract per
    // rect instead of a scan of the whole message clip list.
    std::vector<uint32_t> rects;
    std::vector<uint32_t> tileKeys;
    tileKeys.reserve(tileCount);
    for (uint32_t t = 0; t < tileCount; ++t) {
      const TileJob& job = tiles[t];
      // Tile pixel origin = destRect origin + 64 * tile index (gdi's
      // updateRect = nXDst + tile->x).
      const int tilePx = originX + static_cast<int>(job.x) * 64;
      const int tilePy = originY + static_cast<int>(job.y) * 64;
      const uint32_t rectOff = static_cast<uint32_t>(rects.size());
      uint32_t rectCnt = 0;
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
        const int l = rx > tilePx ? rx : tilePx;
        const int tp = ry > tilePy ? ry : tilePy;
        const int r = (rx + rw) < (tilePx + 64) ? (rx + rw) : (tilePx + 64);
        const int b = (ry + rh) < (tilePy + 64) ? (ry + rh) : (tilePy + 64);
        if (r <= l || b <= tp) {
          continue;
        }
        const uint32_t lx0 = static_cast<uint32_t>(l - tilePx);
        const uint32_t ly0 = static_cast<uint32_t>(tp - tilePy);
        const uint32_t lx1 = static_cast<uint32_t>(r - tilePx);
        const uint32_t ly1 = static_cast<uint32_t>(b - tilePy);
        rects.push_back(lx0 | (ly0 << 8) | (lx1 << 16) | (ly1 << 24));
        rectCnt++;
      }
      tileMeta[t * 4] = static_cast<uint32_t>(tilePx);
      tileMeta[t * 4 + 1] = static_cast<uint32_t>(tilePy);
      tileMeta[t * 4 + 2] = rectOff;
      tileMeta[t * 4 + 3] = rectCnt;
      MarkSurfaceDirty(*surface, tilePx, tilePy, tilePx + 64, tilePy + 64);
      // Only the tiles this message actually *decodes* take part in the batch's
      // disjointness rule: a restamp (type 3) job just rebuilds a tile from the
      // persistent state an earlier message of this frame produced, and it is that
      // earlier message's tile that must not be decoded again later in the batch.
      if (job.streams[0].type != 3u) {
        tileKeys.push_back(static_cast<uint32_t>(job.y) * static_cast<uint32_t>(gridW) +
                           static_cast<uint32_t>(job.x));
      }
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
    if (!AppendToDecodeBatch(surfaceId, composeRegion, tileKeys, std::move(meta),
                             std::move(tileMeta), std::move(rects), payload, size)) {
      return false;
    }
    rfxChunks++;
    return true;
  }

  // V4: ClearCodec is not self-contained (band pixels it does not cover keep the
  // current surface value), so it is a CPU read-modify-write. Since V2 the
  // surface IS the mapping, so FreeRDP's clear_decompress runs straight on it -
  // no staging, no GPU round trip (doc_agent/gfx-engine.md §1).
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
    // 0x21 -> BGRA32), exactly what gdi hands to clear_decompress. Alpha is 0xFF
    // everywhere in this engine, but the format still has to be the one the surface
    // was created with so the band/glyph pixel interpretation matches gdi.
    // Do NOT pass Impl::format (a VkFormat) here: clear_decompress takes FreeRDP's
    // *packed* format, and the two are different vocabularies.
    const uint32_t packedFormat = surface->meta.format;
    InvalidateRect(surface->gpu, x, y, width, height);
    if (!clearDecoder->Decode(payload, payloadLen, width, height, packedFormat, surface->gpu.mapped,
                              surface->gpu.stride, x, y, surface->meta.width,
                              surface->meta.height)) {
      return false;
    }
    MarkHostWrite(&surface->gpu);
    HostWrote();
    MarkSurfaceDirty(*surface, x, y, x + width, y + height);
    clearDecoded++;
    return true;
  }

  void DestroyRfxResources() {
    VkApi& vk = api();
    const VkDevice dev = device();
    if (dev != VK_NULL_HANDLE) {
      for (int s = 0; s < kSlots; ++s) {
        if (rfxPools[s] != VK_NULL_HANDLE && vk.DestroyDescriptorPool != nullptr) {
          vk.DestroyDescriptorPool(dev, rfxPools[s], nullptr);
          rfxPools[s] = VK_NULL_HANDLE;
        }
      }
      if (composePipe != VK_NULL_HANDLE && vk.DestroyPipeline != nullptr) {
        vk.DestroyPipeline(dev, composePipe, nullptr);
      }
      if (idwtPipe != VK_NULL_HANDLE && vk.DestroyPipeline != nullptr) {
        vk.DestroyPipeline(dev, idwtPipe, nullptr);
      }
      if (idwtPipeLayout != VK_NULL_HANDLE && vk.DestroyPipelineLayout != nullptr) {
        vk.DestroyPipelineLayout(dev, idwtPipeLayout, nullptr);
      }
      if (idwtSetLayout != VK_NULL_HANDLE && vk.DestroyDescriptorSetLayout != nullptr) {
        vk.DestroyDescriptorSetLayout(dev, idwtSetLayout, nullptr);
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

    composePipe = VK_NULL_HANDLE;
    idwtPipe = VK_NULL_HANDLE;
    idwtPipeLayout = VK_NULL_HANDLE;
    idwtSetLayout = VK_NULL_HANDLE;
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

bool GfxVkDesktop::Init() {
  if (impl_ == nullptr) {
    impl_ = new Impl();
  }
  // Storage/picture format is fixed (Impl::kFormat): every producer hands over
  // FreeRDP's BGRA order and the presenter converts at present time, so the engine
  // never follows the swapchain's format (doc_agent/gfx-engine.md §2.3).
  impl_->format = Impl::kFormat;
  VkContext& context = VkContext::Instance();
  // No surface: the offline correctness harness never presents.
  if (!context.EnsureDevice(VK_NULL_HANDLE)) {
    HMRDP_LOGE("vk desktop: device unavailable: %{public}s", context.lastError().c_str());
    return false;
  }
  VkApi& api = GetVkApi();
  const VkDevice device = context.device();

  VkPhysicalDeviceProperties physProps{};
  if (api.GetPhysicalDeviceProperties != nullptr) {
    api.GetPhysicalDeviceProperties(context.physicalDevice(), &physProps);
    impl_->atomSize = physProps.limits.nonCoherentAtomSize != 0
                          ? physProps.limits.nonCoherentAtomSize
                          : 64;
    HMRDP_LOGI("vk desktop: limits sharedMem=%{public}u maxInvocations=%{public}u "
               "maxWgCount=%{public}u,%{public}u,%{public}u",
               static_cast<unsigned>(physProps.limits.maxComputeSharedMemorySize),
               static_cast<unsigned>(physProps.limits.maxComputeWorkGroupInvocations),
               static_cast<unsigned>(physProps.limits.maxComputeWorkGroupCount[0]),
               static_cast<unsigned>(physProps.limits.maxComputeWorkGroupCount[1]),
               static_cast<unsigned>(physProps.limits.maxComputeWorkGroupCount[2]));
  }

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
  alloc.commandBufferCount = Impl::kSlots;
  if (api.AllocateCommandBuffers(device, &alloc, impl_->commandBuffers) != VK_SUCCESS) {
    HMRDP_LOGE("vk desktop: vkAllocateCommandBuffers failed");
    return false;
  }
  // Created signaled: a slot that has never been submitted can be waited without
  // blocking, and an already-waited slot keeps its fence signaled.
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  for (int i = 0; i < Impl::kSlots; ++i) {
    if (api.CreateFence(device, &fenceInfo, nullptr, &impl_->fences[i]) != VK_SUCCESS ||
        api.CreateSemaphore(device, &semaphoreInfo, nullptr, &impl_->frameSemaphores[i]) !=
            VK_SUCCESS ||
        api.CreateSemaphore(device, &semaphoreInfo, nullptr, &impl_->engineChain[i]) !=
            VK_SUCCESS ||
        api.CreateSemaphore(device, &semaphoreInfo, nullptr, &impl_->blitDone[i]) != VK_SUCCESS) {
      HMRDP_LOGE("vk desktop: vkCreateFence/vkCreateSemaphore failed");
      return false;
    }
  }
  impl_->SelectSlot(0);
  // V3 Progressive compute. Optional: when it cannot be created the engine still
  // serves every other command and Progressive stays "unsupported" (counted and
  // visible in the stats, never silently wrong).
  if (impl_->CreateRfxPipelines()) {
    const size_t coefBytes = static_cast<size_t>(Impl::kMaxBatchStreams) * 4096u * 2u;
    if (impl_->CreateRawBuffer(&impl_->coef, coefBytes)) {
      impl_->rfxReady = true;
    } else {
      HMRDP_LOGE("vk desktop: progressive scratch allocation failed");
    }
  } else {
    HMRDP_LOGW("vk desktop: progressive compute unavailable (pipeline creation failed)");
  }
  // GPU timestamps (dev perf): per-dispatch GPU time with nothing skipped, so the
  // workload is exactly the one that renders. Optional: without a timestamp period
  // or the entry points the engine simply reports no per-kernel numbers.
  if (api.CmdWriteTimestamp != nullptr && api.CreateQueryPool != nullptr &&
      api.GetQueryPoolResults != nullptr && api.CmdResetQueryPool != nullptr &&
      physProps.limits.timestampPeriod != 0.0f) {
    VkQueryPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = impl_->kTimestampCapacity;
    bool poolsOk = true;
    for (int s = 0; s < Impl::kSlots; ++s) {
      if (api.CreateQueryPool(device, &poolInfo, nullptr, &impl_->timestampPools[s]) != VK_SUCCESS) {
        poolsOk = false;
        break;
      }
    }
    if (poolsOk) {
      impl_->timestampNsPerTick = static_cast<double>(physProps.limits.timestampPeriod);
    } else {
      // A partial failure leaves the pools that were created; CollectTimestamps()
      // simply skips the slots whose pool is missing.
      impl_->timestampNsPerTick = 0.0;
    }
  }
  if (impl_->timestampPools[0] == VK_NULL_HANDLE) {
    HMRDP_LOGW("vk desktop: GPU timestamps unavailable; per-kernel timings disabled");
  }
  // V4: ClearCodec read-modify-write on the CPU. Independent of the compute
  // pipeline; a failure just leaves ClearCodec in the unsupported path.
  impl_->clearDecoder = CreateFreeRdpClearDecoder();
  if (impl_->clearDecoder == nullptr) {
    HMRDP_LOGW("vk desktop: FreeRDP clear_decompress unavailable");
  }
  impl_->RunDeviceProbes();
  ready_ = true;
  HMRDP_LOGI("vk desktop: engine ready (V2 storage: host-visible buffers, rfxCompute=%{public}d, "
             "hostMem=%{public}s coherent=%{public}d atom=%{public}u)",
             impl_->rfxReady ? 1 : 0, impl_->hostMemoryType.c_str(), impl_->hostCoherent ? 1 : 0,
             static_cast<unsigned>(impl_->atomSize));
  return true;
}

void GfxVkDesktop::Reset() {
  if (impl_ == nullptr) {
    return;
  }
  const int64_t resetStart = Impl::NowUs();
  impl_->FlushAll();
  for (int s = 0; s < Impl::kSlots; ++s) {
    impl_->ReleasePending(s);
  }
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
  for (int s = 0; s < Impl::kSlots; ++s) {
    impl_->DestroyImage(&impl_->pictures[s]);
    impl_->pictureMissing[s].clear();
  }
  impl_->DestroyGpuBuffer(&impl_->coef);
  if (device != VK_NULL_HANDLE && api.DestroyQueryPool != nullptr) {
    for (int s = 0; s < Impl::kSlots; ++s) {
      if (impl_->timestampPools[s] != VK_NULL_HANDLE) {
        api.DestroyQueryPool(device, impl_->timestampPools[s], nullptr);
        impl_->timestampPools[s] = VK_NULL_HANDLE;
      }
    }
  }
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
    for (int i = 0; i < Impl::kSlots; ++i) {
      if (impl_->fences[i] != VK_NULL_HANDLE) {
        api.DestroyFence(device, impl_->fences[i], nullptr);
        impl_->fences[i] = VK_NULL_HANDLE;
      }
      if (impl_->frameSemaphores[i] != VK_NULL_HANDLE) {
        api.DestroySemaphore(device, impl_->frameSemaphores[i], nullptr);
        impl_->frameSemaphores[i] = VK_NULL_HANDLE;
      }
      if (impl_->engineChain[i] != VK_NULL_HANDLE) {
        api.DestroySemaphore(device, impl_->engineChain[i], nullptr);
        impl_->engineChain[i] = VK_NULL_HANDLE;
      }
      if (impl_->blitDone[i] != VK_NULL_HANDLE) {
        api.DestroySemaphore(device, impl_->blitDone[i], nullptr);
        impl_->blitDone[i] = VK_NULL_HANDLE;
      }
    }
    if (impl_->commandPool != VK_NULL_HANDLE) {
      // Frees the slot command buffers too.
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

bool GfxVkDesktop::Flush() {
  return impl_ != nullptr && impl_->Flush();
}

// Submits the frame and keeps the slot in flight: the presenter waits this frame's
// semaphore on the device, so the CPU does not wait for the frame's decode here
// (doc_agent/gfx-engine.md §2.3). CPU-readback callers keep using Flush().
bool GfxVkDesktop::SubmitFrame() {
  if (impl_ == nullptr || !impl_->SubmitSlot(true)) {
    return false;
  }
  // The presenter will signal this slot's blit token when its blit finishes; the
  // next time this slot's picture is written (two frames later) it is waited.
  impl_->blitDonePending[impl_->lastSubmittedSlot] = true;
  // Move to the other slot so the next frame is recorded into resources that are not
  // referenced by this submission: the CPU then records it without waiting (the wait
  // happens here only when the slot comes around again, two frames later, and by then
  // the chain has long completed it).
  impl_->SelectSlot((impl_->slot + 1) % Impl::kSlots);
  return true;
}

VkSemaphore GfxVkDesktop::frameSemaphore() const {
  return impl_ != nullptr ? impl_->frameSemaphores[impl_->lastSubmittedSlot] : VK_NULL_HANDLE;
}

// The token the presenter must signal when it has finished reading this frame's
// picture; it is waited before that picture is written again (two frames later).
VkSemaphore GfxVkDesktop::blitDoneSemaphore() const {
  return impl_ != nullptr ? impl_->blitDone[impl_->lastSubmittedSlot] : VK_NULL_HANDLE;
}

// Present failed: the token handed out by blitDoneSemaphore() will never be
// signalled, so stop treating it as pending (the picture stays unsynchronized for one
// round rather than dead-locking the engine on a signal that never comes).
void GfxVkDesktop::AbandonBlitDoneHandoff() {
  if (impl_ != nullptr) {
    impl_->blitDonePending[impl_->lastSubmittedSlot] = false;
  }
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
  // Grid formula copied from FreeRDP's progressive_surface_context_new():
  //   gridWidth = (width + (64 - width % 64)) / 64
  // It is deliberately NOT the minimal ceil(width/64): for a surface whose
  // 16-aligned width is a multiple of 64 it yields one extra column (and row),
  // and the engine must accept exactly the same tiles as gdi - a tile the engine
  // rejects but FreeRDP decodes (or vice versa) is a silent divergence in what
  // the two implementations consider "the same tile grid". Those extra tiles lie
  // entirely outside the surface, so they cannot change a visible pixel either
  // way (verified: the region intersection is empty), but the acceptance set has
  // to match.
  surface.meta.gridW = (surface.meta.width + (64 - surface.meta.width % 64)) / 64;
  surface.meta.gridH = (surface.meta.height + (64 - surface.meta.height % 64)) / 64;
  surface.meta.mappedWidth = width;
  surface.meta.mappedHeight = height;

  if (!impl_->CreateGpuBuffer(&surface.gpu, surface.meta.width, surface.meta.height)) {
    return false;
  }
  // FreeRDP's CreateSurface contract: every pixel starts as 0xFF, and unpainted
  // pixels are what the pixel comparison observes. 0xFFFFFFFF is channel-order
  // symmetric, so it needs no swap.
  std::memset(surface.gpu.mapped, 0xFF, surface.gpu.capacity);
  impl_->MarkHostWrite(&surface.gpu);
  impl_->HostWrote();
  // Persistent Progressive tile state. Allocation failure degrades only the
  // Progressive path for this surface (counted as unsupported), not the surface.
  if (impl_->rfxReady &&
      !impl_->CreateRfxState(&surface.rfx, surface.meta.gridW, surface.meta.gridH)) {
    HMRDP_LOGW("vk desktop: surface %{public}u progressive state allocation failed", surfaceId);
  }

  // The wire pixel format decides more than the swap: 0x21 (ARGB_8888) maps to
  // BGRA32, which is exactly gdi's desktop format, so a desktop-sized surface can
  // share the primary buffer (doc_agent/cpu-path.md §6.1 ②); 0x20 (XRGB_8888)
  // maps to BGRX32 and cannot.
  HMRDP_LOGI("vk desktop: surface %{public}u %{public}dx%{public}d stride=%{public}d"
             " wireFormat=0x%{public}x",
             surfaceId, surface.meta.width, surface.meta.height, surface.meta.stride, format);
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

void GfxVkDesktop::MapSurfaceToOutput(uint16_t surfaceId, uint32_t outputOriginX,
                                      uint32_t outputOriginY) {
  Impl::Surface* surface = impl_ != nullptr ? impl_->Find(surfaceId) : nullptr;
  if (surface != nullptr) {
    surface->meta.mapped = true;
    surface->meta.outputX = outputOriginX;
    surface->meta.outputY = outputOriginY;
    // gdi_MapSurfaceToOutput clears the surface's invalid region.
    Impl::ClearSurfaceDirty(*surface);
  }
}

bool GfxVkDesktop::ResetGraphics(int width, int height) {
  if (!ready()) {
    return false;
  }
  impl_->FlushAll();
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
    for (int s = 0; s < Impl::kSlots; ++s) {
      impl_->DestroyImage(&impl_->pictures[s]);
      impl_->pictureMissing[s].clear();
    }
    impl_->pictureW = 0;
    impl_->pictureH = 0;
    screenW_ = 0;
    screenH_ = 0;
    if (width > 0 && height > 0) {
      // A fresh picture carries no history: both start from the same 0xFF fill, so
      // neither owes the other anything.
      for (int s = 0; s < Impl::kSlots; ++s) {
        if (!impl_->PrepareImage(&impl_->pictures[s], width, height)) {
          return false;
        }
        impl_->FillImage(&impl_->pictures[s], 0xFFFFFFFFu);
      }
      impl_->pictureW = impl_->pictures[0].width;
      impl_->pictureH = impl_->pictures[0].height;
      screenW_ = impl_->pictureW;
      screenH_ = impl_->pictureH;
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
      impl_->MarkHostWrite(&surface.gpu);
    }
    Impl::ClearSurfaceDirty(surface);
  }
  if (impl_->clearDecoder != nullptr) {
    impl_->clearDecoder->Reset();
  }
  impl_->HostWrote();
  return true;
}

bool GfxVkDesktop::Compose() {
  // Compose into this slot's picture; the other one is what the presenter samples.
  const int pic = impl_->slot;
  Impl::GpuImage& target = impl_->pictures[pic];
  if (!ready() || target.image == VK_NULL_HANDLE) {
    return false;
  }
  // The screen compose reads the surfaces *after* the pending decode batch has
  // written them, so the batch (decode, inverse DWT and the surface composes it
  // carries) has to be recorded first.
  if (!impl_->FinishDecodeBatch()) {
    return false;
  }
  // Nothing marked anywhere: do not compose and let the caller skip the present.
  // A picture that is behind keeps its ledger, so the next frame that *does* have
  // marks brings it up to date before it is sampled - that is what keeps a static
  // desktop at FPS 0 instead of re-presenting a catch-up every frame.
  bool anyDirty = false;
  for (const auto& kv : impl_->surfaces) {
    if (kv.second.meta.mapped && kv.second.meta.dirtyValid) {
      anyDirty = true;
      break;
    }
  }
  if (!anyDirty) {
    return false;
  }
  const int64_t composeStart = Impl::NowUs();
  const int scrW = impl_->pictureW;
  const int scrH = impl_->pictureH;
  const int other = (pic + 1) % Impl::kSlots;
  for (auto& kv : impl_->surfaces) {
    Impl::Surface& s = kv.second;
    GpuSurface& m = s.meta;
    if (!m.mapped) {
      impl_->composeSkipUnmapped++;
      continue;
    }
    // This picture is missing whatever the *other* picture received since it was
    // last written: those rects have to be copied again (they live in surface
    // coordinates, so they belong to this surface's list) or the picture's
    // untouched areas would still show the older desktop. The last `frameRects`
    // entries are this frame's own marks and are what the other picture will owe
    // us next time.
    std::vector<Impl::Surface::DirtyRect>& missing = impl_->pictureMissing[pic][kv.first];
    // Copied *before* appending (and before CoalesceRects sorts the list in place):
    // this is exactly what the other picture owes a re-copy of.
    const std::vector<Impl::Surface::DirtyRect> frameMarks(s.dirtyRects.begin(),
                                                           s.dirtyRects.end());
    if (!missing.empty()) {
      s.dirtyRects.insert(s.dirtyRects.end(), missing.begin(), missing.end());
    }
    if (!m.dirtyValid && s.dirtyRects.empty()) {
      impl_->composeSkipClean++;
      continue;
    }
    // Merge the marked rects into exact rectangles and compose them individually;
    // only when even the merged list is too long does the union box take over
    // (bounded fallback - the same one the CPU present path has). All of a
    // surface's regions go into one command: the source buffer and stride are
    // shared, so the only per-rect cost is another region entry.
    const int merged =
        Impl::CoalesceRects(s.dirtyRects.data(), static_cast<int>(s.dirtyRects.size()));
    const bool rectListTooLong =
        s.dirtyOverflow || merged > Impl::kMaxComposeRects;
    const bool overflowed = rectListTooLong || !Impl::kComposeRects;
    // The box fallback has to cover the missing deltas as well, not just this
    // frame's marks (they are appended to `s.dirtyRects` above, so union their
    // hull with the marked hull).
    int boxLeft = m.dirtyLeft;
    int boxTop = m.dirtyTop;
    int boxRight = m.dirtyRight;
    int boxBottom = m.dirtyBottom;
    for (const Impl::Surface::DirtyRect& r : missing) {
      if (r.left < boxLeft) boxLeft = r.left;
      if (r.top < boxTop) boxTop = r.top;
      if (r.right > boxRight) boxRight = r.right;
      if (r.bottom > boxBottom) boxBottom = r.bottom;
    }
    VkBufferImageCopy regions[Impl::kMaxComposeRects];
    int regionCount = 0;
    const int listCount = overflowed ? 1 : merged;
    for (int i = 0; i < listCount; ++i) {
      const int left = overflowed ? boxLeft : s.dirtyRects[i].left;
      const int top = overflowed ? boxTop : s.dirtyRects[i].top;
      const int right = overflowed ? boxRight : s.dirtyRects[i].right;
      const int bottom = overflowed ? boxBottom : s.dirtyRects[i].bottom;
      if (impl_->MakeScreenCopyRegion(m, s.gpu, left, top, right, bottom, scrW, scrH,
                                      &regions[regionCount])) {
        regionCount++;
      }
    }
    if (regionCount == 0) {
      // Every rect was outside the surface/output: nothing to copy, nothing to
      // retry (same as the old "degenerate box" path).
      Impl::ClearSurfaceDirty(s);
      continue;
    }
    if (!impl_->CopyBufferRegionsToScreen(s.gpu, target.image, regions, regionCount)) {
      // Keep the dirty state so a later Compose can retry this surface.
      return false;
    }
    // This picture caught up: drop its ledger, and hand *this frame's* marks to the
    // other picture so it re-copies them (it missed them) the next time it composes.
    missing.clear();
    std::vector<Impl::Surface::DirtyRect>& otherMissing = impl_->pictureMissing[other][kv.first];
    otherMissing.insert(otherMissing.end(), frameMarks.begin(), frameMarks.end());
    // The rect figures describe the *merged dirty rect list* (what a rect-mode
    // compose copies), independently of which mode actually composed, so they stay
    // comparable with the CPU present path's `rectlist=`/`maxRects=`/`truncated=`.
    impl_->composeRects += static_cast<uint64_t>(merged);
    if (static_cast<uint64_t>(merged) > impl_->composeMaxRects) {
      impl_->composeMaxRects = static_cast<uint64_t>(merged);
    }
    if (rectListTooLong) {
      impl_->composeRectOverflow++;
    }
    impl_->composeCopies++;
    Impl::ClearSurfaceDirty(s);
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
  // The picture the presenter must sample: the one the last submitted frame composed
  // into (the pictures ping-pong, so this is *not* the frame currently being recorded).
  return impl_ != nullptr ? impl_->pictures[impl_->lastSubmittedSlot].image : VK_NULL_HANDLE;
}

VkFormat GfxVkDesktop::format() const {
  return impl_ != nullptr ? impl_->format : VK_FORMAT_UNDEFINED;
}

bool GfxVkDesktop::ReadScreen(std::vector<uint8_t>* out) {
  // The presented picture (the last submitted frame's) - the compare reads exactly
  // what the presenter sampled.
  const Impl::GpuImage& picture = impl_->pictures[impl_->lastSubmittedSlot];
  if (!ready() || out == nullptr || picture.image == VK_NULL_HANDLE) {
    return false;
  }
  const int64_t readStart = Impl::NowUs();
  const int width = picture.width;
  const int height = picture.height;
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  out->assign(bytes, 0);

  // Prior work must be complete before the copy is recorded, and the copy itself
  // must complete before the CPU reads (this is the only place V2 stalls). Every
  // in-flight slot has to be waited, not just this one: the picture accumulates
  // composes from several submissions.
  if (!impl_->FlushAll()) {
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
  GetVkApi().CmdCopyImageToBuffer(impl_->commandBuffer, picture.image,
                                  VK_IMAGE_LAYOUT_GENERAL, impl_->readBuffer, 1, &region);
  impl_->pendingDeviceWrites = true;
  if (!impl_->Flush()) {
    return false;
  }
  Impl::AddStat(&impl_->statRead, readStart);
  VkMappedMemoryRange readRange{};
  readRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  readRange.memory = impl_->readMemory;
  readRange.offset = 0;
  readRange.size = VK_WHOLE_SIZE;
  if (!impl_->hostCoherent && GetVkApi().InvalidateMappedMemoryRanges != nullptr) {
    GetVkApi().InvalidateMappedMemoryRanges(impl_->device(), 1, &readRange);
  }
  std::memcpy(out->data(), impl_->readMapped, bytes);
  return true;
}

void GfxVkDesktop::NotePresentSplitUs(uint64_t composeUs, uint64_t flushUs, uint64_t blitUs) {
  if (impl_ == nullptr) {
    return;
  }
  impl_->presentFrames++;
  impl_->presentComposeUs += composeUs;
  impl_->presentFlushUs += flushUs;
  impl_->presentBlitUs += blitUs;
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
  const uint32_t texel = bgraPixel;
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
    impl_->InvalidateRect(surface->gpu, left, top, width, height);
    CpuFillRect(surface->gpu.mapped, surface->gpu.stride, left, top, width, height, texel);
    impl_->MarkHostWrite(&surface->gpu);
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
  impl_->InvalidateRect(surface->gpu, sx, sy, cols, rows);
  for (int row = 0; row < rows; ++row) {
    const uint8_t* srcRow =
        bgra + static_cast<size_t>(srcRow0 + row) * srcStride + static_cast<size_t>(srcCol) * 4;
    uint8_t* dstRow = surface->gpu.mapped + static_cast<size_t>(sy + row) * dstStride +
                      static_cast<size_t>(sx) * 4;
    std::memcpy(dstRow, srcRow, static_cast<size_t>(cols) * 4);
  }
  impl_->MarkHostWrite(&surface->gpu);
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
    impl_->cacheAllocCalls++;
    impl_->DeferGpuBuffer(&entry.gpu);
    if (!impl_->CreateGpuBuffer(&entry.gpu, width, height)) {
      return false;
    }
  }
  entry.width = width;
  entry.height = height;
  impl_->cacheStoreCalls++;
  impl_->cacheBytes += static_cast<uint64_t>(width) * height * 4;
  // The source is device-written (Progressive compute), the destination is
  // CPU-only (a cache entry is never referenced by a command buffer, so it needs
  // no cache maintenance - only the surface read below does).
  impl_->InvalidateRect(surface->gpu, x, y, width, height);
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
  impl_->InvalidateRect(surface->gpu, dstX, dstY, entry.width, entry.height);
  CpuCopyRows(entry.gpu.mapped, entry.gpu.stride, 0, 0, surface->gpu.mapped, surface->gpu.stride,
              dstX, dstY, entry.width, entry.height);
  impl_->cacheRestoreCalls++;
  impl_->cacheBytes += static_cast<uint64_t>(entry.width) * entry.height * 4;
  impl_->MarkHostWrite(&surface->gpu);
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
  // Overlapping same-surface copies are copied in the order that never clobbers a
  // source row before it is read (bottom-up when the destination is further down,
  // otherwise top-down), with a per-row memmove for the horizontal overlap. This
  // is the memmove row-order argument: rows map monotonically, so the destination
  // row of step i only coincides with a source row already handled.
  impl_->copyBytes += static_cast<uint64_t>(w) * h * 4;
  const bool sameSurface = (src == dst);
  const bool overlaps = sameSurface && sx < dx + w && dx < sx + w && sy < dy + h && dy < sy + h;
  if (overlaps) {
    impl_->copyOverlapBytes += static_cast<uint64_t>(w) * h * 4;
  }
  const size_t rowBytes = static_cast<size_t>(w) * 4;
  // Both rects are invalidated before the CPU touches them: the source may hold
  // device writes the CPU has not seen, and the destination may hold stale cache
  // lines whose untouched neighbours must not be written back.
  impl_->InvalidateRect(src->gpu, sx, sy, w, h);
  if (!sameSurface) {
    impl_->InvalidateRect(dst->gpu, dx, dy, w, h);
  }
  const int step = (overlaps && dy > sy) ? -1 : 1;
  const int first = (step < 0) ? h - 1 : 0;
  for (int i = 0; i < h; ++i) {
    const int row = first + step * i;
    const uint8_t* srcRow = src->gpu.mapped + static_cast<size_t>(sy + row) * src->gpu.stride +
                            static_cast<size_t>(sx) * 4;
    uint8_t* dstRow = dst->gpu.mapped + static_cast<size_t>(dy + row) * dst->gpu.stride +
                      static_cast<size_t>(dx) * 4;
    std::memmove(dstRow, srcRow, rowBytes);
  }
  impl_->MarkHostWrite(&dst->gpu);
  impl_->HostWrote();
  Impl::MarkSurfaceDirty(*dst, dx, dy, dx + w, dy + h);
  return true;
}

void GfxVkDesktop::ApplyCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                               const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                               uint32_t payloadLen) {
  const int64_t opStart = impl_ != nullptr ? Impl::NowUs() : 0;
  const uint64_t drainsBefore = impl_ != nullptr ? impl_->syncDrains : 0;
  const uint64_t drainUsBefore = impl_ != nullptr ? impl_->syncDrainUs : 0;
  // Surface lifecycle commands, and anything that changes what a pending decode
  // batch would composite, must not overtake it. Only a *Progressive* surface
  // command feeds the batch: ClearCodec and uncompressed uploads are CPU
  // read-modify-writes of surface pixels, so they have to be ordered after the
  // batch's composes like any other pixel command.
  bool isProgressive = false;
  if (impl_ != nullptr && cmdId == kGpuCmdWireToSurface && scalars != nullptr) {
    isProgressive = (scalars[0] == kGpuCodecCaprogressive);
  }
  if (impl_ != nullptr && cmdId != kGpuCmdStartFrame && !isProgressive) {
    if (!impl_->FinishDecodeBatch()) {
      return;
    }
  }
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
        break;  // no such surface: nothing to draw on
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
          Impl::ClearSurfaceDirty(*s);
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
        bool frameChanged = false;
        for (auto& entry : impl_->surfaces) {
          if (entry.second.frameId == scalars[0]) {
            impl_->frameIdRepeats++;
            continue;
          }
          // A *new* frame id starts a new frame: the messages of the previous one
          // are complete (their decode batch could not grow past the boundary).
          frameChanged = true;
          entry.second.frameId = scalars[0];
          entry.second.frameTiles.clear();
          if (!entry.second.frameTileSeen.empty()) {
            std::fill(entry.second.frameTileSeen.begin(), entry.second.frameTileSeen.end(), 0);
          }
        }
        if (frameChanged) {
          // A batch never spans a frame boundary: the frame's tile list resets and
          // a message's compose semantics are defined relative to its own frame.
          if (!impl_->FinishDecodeBatch()) {
            return;
          }
          impl_->RecordFrameMessages();
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
    stat->drains += impl_->syncDrains - drainsBefore;
    stat->drainUs += impl_->syncDrainUs - drainUsBefore;
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
      "prog=%llu/%llu compose=%llu/%llu read=%llu/%llu barriers=%llu flushWait=%llums"
      "\n  drainUs/drains fill=%llu/%llu cache=%llu/%llu copy=%llu/%llu prog=%llu/%llu"
      " life=%llu/%llu release=%llums",
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
      static_cast<unsigned long long>(impl_->flushWaitUs / 1000),
      static_cast<unsigned long long>(impl_->statFill.drainUs),
      static_cast<unsigned long long>(impl_->statFill.drains),
      static_cast<unsigned long long>(impl_->statCache.drainUs),
      static_cast<unsigned long long>(impl_->statCache.drains),
      static_cast<unsigned long long>(impl_->statCopy.drainUs),
      static_cast<unsigned long long>(impl_->statCopy.drains),
      static_cast<unsigned long long>(impl_->statProgressive.drainUs),
      static_cast<unsigned long long>(impl_->statProgressive.drains),
      static_cast<unsigned long long>(impl_->statLifecycle.drainUs),
      static_cast<unsigned long long>(impl_->statLifecycle.drains),
      static_cast<unsigned long long>(impl_->releaseUs / 1000));
  char buf2[2048];
  std::snprintf(buf2, sizeof(buf2),
                "\n  gpuMs rlgr=%.1f idwt=%.1f compose=%.1f (samples %llu/%llu/%llu drops=%llu)"
                "\n  perChunkMs rlgr min=%.2f max=%.2f | idwt min=%.2f max=%.2f"
                "\n  decode input %.1f MB / %llu streams (%.0f B per stream), perChunk KB min=%.1f max=%.1f"
                "\n  batches flushed=%llu skipped=%llu maxStreams=%llu composeGridSplits=%llu "
                "composeGroupsMax=%llu"
                "\n  msgPerFrame 0=%llu 1=%llu 2=%llu 3=%llu 4-7=%llu 8+=%llu max=%u"
                "\n  hostMemType=%s coherent=%d atom=%llu emptySubmit=%lluus"
                "\n  compose copies=%llu rects=%llu maxRects=%llu overflow=%llu"
                " skipUnmapped=%llu skipClean=%llu"
                "\n  presentSplit frames=%llu avgPerFrameUs compose=%.0f flush=%.0f blit=%.0f"
                "\n  alloc calls=%llu us=%llu (cache allocs=%llu)"
                "\n  cpu bytes cache=%llu copy=%llu (overlap=%llu) cacheOps store=%llu restore=%llu"
                "\n  syncDrains=%llu syncDrain=%llums"
                "\n  rfxCompute=%d chunks=%llu first=%llu upgrade=%llu clearDec=%llu "
                "clearUnsup=%llu progFail=%llu progComposeSkip=%llu"
                "\n  rfxParse regions=%llu simple=%llu diff=%llu nonExtrap=%llu skipTiles=%llu "
                "errors=%llu skippedRegions=%llu rejectedTiles=%llu fullQualityTiles=%llu "
                "batchOverflow=%llu originNonZero=%llu restamped=%llu multiRegion=%llu "
                "frameIdRepeats=%llu",
                impl_->GpuMs(0), impl_->GpuMs(1), impl_->GpuMs(2),
                static_cast<unsigned long long>(impl_->gpuSamples[0]),
                static_cast<unsigned long long>(impl_->gpuSamples[1]),
                static_cast<unsigned long long>(impl_->gpuSamples[2]),
                static_cast<unsigned long long>(impl_->gpuTimestampDrops),
                impl_->GpuMsMin(0), impl_->GpuMsMax(0), impl_->GpuMsMin(1), impl_->GpuMsMax(1),
                static_cast<double>(impl_->chunkBytes) / 1048576.0,
                static_cast<unsigned long long>(impl_->streamCount),
                impl_->streamCount > 0
                    ? static_cast<double>(impl_->chunkBytes) /
                          static_cast<double>(impl_->streamCount)
                    : 0.0,
                impl_->chunkBytesMin == UINT64_MAX ? 0.0
                                                   : static_cast<double>(impl_->chunkBytesMin) / 1024.0,
                static_cast<double>(impl_->chunkBytesMax) / 1024.0,
                static_cast<unsigned long long>(impl_->batchesFlushed),
                static_cast<unsigned long long>(impl_->batchesSkipped),
                static_cast<unsigned long long>(impl_->batchStreamsMax),
                static_cast<unsigned long long>(impl_->composeGridSplits),
                static_cast<unsigned long long>(impl_->composeGroupsMax),
                static_cast<unsigned long long>(impl_->messagesPerFrame[0]),
                static_cast<unsigned long long>(impl_->messagesPerFrame[1]),
                static_cast<unsigned long long>(impl_->messagesPerFrame[2]),
                static_cast<unsigned long long>(impl_->messagesPerFrame[3]),
                static_cast<unsigned long long>(impl_->messagesPerFrame[4]),
                static_cast<unsigned long long>(impl_->messagesPerFrame[5]),
                static_cast<unsigned>(impl_->messagesThisFrameMax),
                impl_->hostMemoryType.c_str(), impl_->hostCoherent ? 1 : 0,
                static_cast<unsigned long long>(impl_->atomSize),
                static_cast<unsigned long long>(impl_->emptySubmitUs),
                static_cast<unsigned long long>(impl_->composeCopies),
                static_cast<unsigned long long>(impl_->composeRects),
                static_cast<unsigned long long>(impl_->composeMaxRects),
                static_cast<unsigned long long>(impl_->composeRectOverflow),
                static_cast<unsigned long long>(impl_->composeSkipUnmapped),
                static_cast<unsigned long long>(impl_->composeSkipClean),
                static_cast<unsigned long long>(impl_->presentFrames),
                impl_->presentFrames != 0
                    ? static_cast<double>(impl_->presentComposeUs) /
                          static_cast<double>(impl_->presentFrames)
                    : 0.0,
                impl_->presentFrames != 0
                    ? static_cast<double>(impl_->presentFlushUs) /
                          static_cast<double>(impl_->presentFrames)
                    : 0.0,
                impl_->presentFrames != 0
                    ? static_cast<double>(impl_->presentBlitUs) /
                          static_cast<double>(impl_->presentFrames)
                    : 0.0,
                static_cast<unsigned long long>(impl_->allocCalls),
                static_cast<unsigned long long>(impl_->allocUs),
                static_cast<unsigned long long>(impl_->cacheAllocCalls),
                static_cast<unsigned long long>(impl_->cacheBytes),
                static_cast<unsigned long long>(impl_->copyBytes),
                static_cast<unsigned long long>(impl_->copyOverlapBytes),
                static_cast<unsigned long long>(impl_->cacheStoreCalls),
                static_cast<unsigned long long>(impl_->cacheRestoreCalls),
                static_cast<unsigned long long>(impl_->syncDrains),
                static_cast<unsigned long long>(impl_->syncDrainUs / 1000),
                impl_->rfxReady ? 1 : 0,
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
                static_cast<unsigned long long>(impl_->rfxState.skippedRegions),
                static_cast<unsigned long long>(impl_->rfxRejectedTiles),
                static_cast<unsigned long long>(impl_->rfxFullQualityTiles),
                static_cast<unsigned long long>(impl_->rfxBatchOverflow),
                static_cast<unsigned long long>(impl_->rfxOriginNonZero),
                static_cast<unsigned long long>(impl_->restamped),
                static_cast<unsigned long long>(impl_->rfxMultiRegion),
                static_cast<unsigned long long>(impl_->frameIdRepeats));
  return std::string(buf) + buf2 +
         (impl_->hostMemoryProbe.empty() ? std::string()
                                         : "\n  hostMem " + impl_->hostMemoryProbe);
}

std::string GfxVkDesktop::lastError() const {
  return ready() ? std::string() : std::string("vk desktop not ready");
}


bool GpuVkPresentComposed(GfxVkDesktop* engine, VkRenderer* renderer) {
  if (engine == nullptr || renderer == nullptr) {
    return false;
  }
  // Perf accounting (doc_agent/gfx-engine.md §2.3): split a present into the dirty
  // compose, the submit+wait, and the full-screen blit + present, so the strategy
  // question ("how much of the frame cost scales with the dirty area?") is answered
  // with numbers instead of assumed. Host-time on purpose: every GPU step here is
  // bounded by a fence wait, so host time tracks the device cost.
  const auto nowUs = []() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  };
  const uint64_t tCompose0 = nowUs();
  if (!engine->Compose()) {
    return false;  // static frame: nothing dirty, no present (FPS stays 0)
  }
  // Compose() only *records* the buffer->image copies; the screen image is not
  // updated until they are submitted. Flush() before blitting it, otherwise the
  // presenter blits the previous (or initial) screen and, worse, every frame's
  // commands pile up into one giant submission that only runs at teardown.
  const uint64_t tCompose1 = nowUs();
  if (!engine->SubmitFrame()) {
    return false;
  }
  const uint64_t tFlush1 = nowUs();
  const bool presented = renderer->PresentImage(engine->screenImage(), engine->format(),
                                               engine->screenWidth(), engine->screenHeight(),
                                               engine->frameSemaphore(),
                                               engine->blitDoneSemaphore());
  const uint64_t tBlit1 = nowUs();
  if (!presented) {
    // No blit was submitted, so the token handed over above will never be signalled.
    engine->AbandonBlitDoneHandoff();
  }
  engine->NotePresentSplitUs(tCompose1 - tCompose0, tFlush1 - tCompose1, tBlit1 - tFlush1);
  if (presented) {
    engine->ClearScreenDirty();
  }
  return presented;
}

}  // namespace hmrdp