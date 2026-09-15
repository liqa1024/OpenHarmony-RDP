/*
 * HmRdp - dev-only recorded-RDP replay (doc_agent/gfx-engine.md §6).
 *
 * Present-on-screen consumer of the shared replay driver: the capture is read,
 * decompressed and parsed by hmrdp_gfx_driver.cpp (FreeRDP's own ZGX + RDPGFX
 * parsing), the resulting commands go to the Vulkan desktop engine, and this
 * file only presents the composed screen on every EndFrame through
 * GpuVkPresentComposed(). The compare route can additionally run an offline gdi
 * desktop on the same bytes and compare the two screens pixel by pixel.
 */
#include "hmrdp_replay.h"

#include <native_window/external_window.h>


#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_cpu.h"
#include "hmrdp_gfx_driver.h"
#include "hmrdp_log.h"
#include "hmrdp_rfx.h"  // kGpuCmd / kGpuCodec / ParseRfxProgressive
#include "hmrdp_vk_desktop.h"
#include "hmrdp_vk_renderer.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

// The replay's desktop engine: the Vulkan GFX engine plus its swapchain
// presenter. It exposes exactly what the pump/stats/compare code needs, so that
// code stays free of the engine's own types.
class ReplayDesktop {
 public:
  // Brings up the engine + presenter on the given XComponent surface; returns
  // false and fills `error` on failure.
  bool Init(void* window, int width, int height, std::string* error);
  void Resize(int width, int height);
  void Apply(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
             const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
             uint32_t payloadLen);
  // Composes and presents the engine screen. Returns false when nothing was
  // dirty (static frame) or the present failed.
  bool Present();
  bool screenDirty() const { return engine_ != nullptr && engine_->screenDirty(); }
  bool ReadScreen(std::vector<uint8_t>* out) { return engine_->ReadScreen(out); }
  // Ends recording/submits: required before a CPU read of the surfaces when the
  // dev A/B is run per command (the pending Progressive decode batch must have
  // executed).
  bool Flush() { return engine_->Flush(); }
  // Engine-side surface geometry (16-aligned width/height and the row pitch), so
  // the dev A/B can walk two buffers with their own strides.
  // Dev: the engine's own Progressive predictor state for one tile (see
  // GfxVkDesktop::ReadTileState), so it can be diffed against gdi's.
  bool TileState(uint16_t surfaceId, uint16_t xIdx, uint16_t yIdx, int16_t* cur, int16_t* sign,
                 uint8_t bitPos[30]) {
    return engine_->ReadTileState(surfaceId, xIdx, yIdx, cur, sign, bitPos);
  }
  bool SurfaceSize(uint16_t surfaceId, int* width, int* height, int* stride) {
    const GpuSurface* s = engine_->FindSurface(surfaceId);
    if (s == nullptr) {
      return false;
    }
    if (width != nullptr) {
      *width = s->width;
    }
    if (height != nullptr) {
      *height = s->height;
    }
    if (stride != nullptr) {
      *stride = s->stride;
    }
    return true;
  }
  // Full surface (top-down, `stride` bytes) as BGRA; dev diagnostics only.
  bool ReadSurface(uint16_t surfaceId, std::vector<uint8_t>* out) {
    return engine_->ReadSurface(surfaceId, out);
  }
  // One surface rect, tightly packed BGRA (`width * 4` per row). Dev only: the
  // per-command A/B reads a command's rect instead of the whole surface.
  bool ReadSurfaceRect(uint16_t surfaceId, int x, int y, int width, int height,
                       std::vector<uint8_t>* out) {
    return engine_->ReadSurfaceRect(surfaceId, x, y, width, height, out);
  }
  int screenWidth() const { return engine_->screenWidth(); }
  int screenHeight() const { return engine_->screenHeight(); }
  // One-line engine summary for the dev panel.
  std::string Summary() const { return engine_->Stats(); }

 private:
  std::unique_ptr<GfxVkDesktop> engine_;
  std::unique_ptr<VkRenderer> renderer_;
};


bool ReplayDesktop::Init(void* window, int width, int height, std::string* error) {
  renderer_ = std::make_unique<VkRenderer>();
  renderer_->SetSurface(window, width, height);
  // Prepare() creates the swapchain and presents a black frame; it also pins the
  // image format the engine must be created with (a blit cannot convert channel
  // order, so engine and swapchain have to agree).
  if (!renderer_->Prepare()) {
    if (error != nullptr) {
      *error = "vulkan surface/swapchain failed: " + renderer_->lastError();
    }
    renderer_.reset();
    return false;
  }
  engine_ = std::make_unique<GfxVkDesktop>();
  if (!engine_->Init(renderer_->format())) {
    if (error != nullptr) {
      *error = "vulkan engine init failed";
    }
    engine_.reset();
    renderer_.reset();
    return false;
  }
  return true;
}


void ReplayDesktop::Resize(int width, int height) {
  renderer_->ResizeSurface(width, height);
}

void ReplayDesktop::Apply(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                          const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                          uint32_t payloadLen) {
  engine_->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
}

bool ReplayDesktop::Present() {
  return GpuVkPresentComposed(engine_.get(), renderer_.get());
}

namespace {

constexpr int kFrameMs = 16;                      // ~60 Hz playback target
constexpr int kLogEvery = 120;
constexpr int64_t kMaxRunUs = 120ll * 1000000ll;  // safety cap
constexpr int kStartWaitUs = 3000000;
// Compare route: sample a full-screen readback every N frames (same idea as the
// live shadow check - reading the engine screen back is expensive).
constexpr uint64_t kCompareEvery = 30;
// Dev: per-message codec A/B. Off by default: it reads the engine's whole surface
// once per Progressive message / ClearCodec band / cache command, which costs a
// GPU readback per command and drags the replay far past real time. Flip to true
// only while a decoder divergence has to be pinned to a message (the replay then
// looks hung / the picture stays black, so never leave it on).
constexpr bool kCodecAbEnabled = false;
// Dev: write both screens (RGB24 PPM, 3120x2080 = ~19 MB each) on the first
// divergence, so the *shape* of a divergence can be seen offline. Off by default:
// the files are large and land in the app's filesDir.
constexpr bool kDumpCompareScreens = false;
// Dev: whole-surface A/B after every Progressive message. The per-rect A/B above
// only checks the tiles a message decodes itself, so it is blind to the tiles
// FreeRDP *re-composites* (update_tiles re-stamps the frame's whole tile list) -
// and to a surface that was left stale by an earlier message. Comparing the whole
// surface names the first message after which the engine's surface stops agreeing
// with gdi's, which is the command that has to be explained. Off by default: a
// read of the mapped surface per message is cheap, but the log is not.
constexpr bool kSurfaceAbEnabled = false;
constexpr uint64_t kSurfaceAbMaxChecks = 300;
// Dev: per-tile predictor-state A/B (doc_agent/gfx-progressive-kernel.md §3). After
// a Progressive message the engine's `cur`/`sign`/bit positions for every tile that
// message decoded are compared with FreeRDP's own `current`/`sign`/bit positions
// (requires the patched FreeRDP accessor, native/scripts/patch-freerdp.ps1 patch 9).
// The *pixels* can agree while the state differs - the dequantise step is a no-op
// for `quant+progQuant-1 >= 16` and the DWT's integer halvings absorb small
// differences - and the state is what every later refinement builds on, so this is
// the measurement that names the message where the two decoders part ways. Off by
// default: it forces a submit+fence per message.
constexpr bool kTileStateAbEnabled = false;
// Only the capture's first messages (the first divergence is expected early, and
// every checked message costs a flush plus a state read per tile).
constexpr int kTileStateAbMessages = 40;
// Dev (`kWatchTileEnabled`): watch-tile history. The state A/B above only looks at
// the tiles a message decodes, so a tile an *earlier* message left divergent is
// reported at whatever later message happens to touch it again - the message that
// introduced the divergence, and the state the two decoders held before it, are both
// lost. This probe re-reads one fixed tile after *every* Progressive message
// instead, so the tile's whole history is on record. Pairs with the engine's
// per-message `vk watch` log (kLogWatchTile in hmrdp_vk_desktop.cpp), which names
// the quant table entries the engine used for that tile.
constexpr bool kWatchTileEnabled = false;
constexpr uint16_t kWatchTileX = 40;
constexpr uint16_t kWatchTileY = 3;
constexpr int kWatchTileMessages = 40;
// The state window dumped for the watch tile: the LL3 (DC) band starts at 4015, and
// that is where the residue's divergence has been seen.
constexpr int kWatchWindowStart = 4010;
constexpr int kWatchWindowCount = 11;

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Little-endian u32 out of a command's params blob (same layout the engines and
// hmrdp_gfx_driver use for surface commands).
uint32_t RdU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t RdU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// Stable route name for the stats panel / hilog.
const char* RouteName(GfxReplayRoute route) {
  switch (route) {
    case GfxReplayRoute::kCpu:
      return "cpu";
    case GfxReplayRoute::kVulkan:
      return "vulkan";
    case GfxReplayRoute::kVulkanCompare:
      return "vulkan-compare";
  }
  return "?";
}

// Sends the replayed commands into the engine; the driver supplies them.
class ReplaySink : public GfxCommandSink {
 public:
  ReplaySink(ReplayDesktop* engine, GfxReplay* owner) : engine_(engine), owner_(owner) {}

  void ApplyGfx(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                uint32_t payloadLen) override {
    if (engine_ == nullptr) {
      return;
    }
    // The surface command carries its codec id in scalars[0]; 0 marks the
    // non-surface commands (fill/copy/cache/...).
    const uint32_t codecId =
        (cmdId == kGpuCmdWireToSurface && scalars != nullptr) ? scalars[0] : 0u;
    // Dev (`kCodecAbEnabled`): mirror FreeRDP's per-frame tile list so the restamp
    // (re-composite) pass can be A/B-checked too; the list is reset when the frame id
    // changes, exactly like PROGRESSIVE_SURFACE_CONTEXT::numUpdatedTiles.
    if (cmdId == kGpuCmdStartFrame && scalars != nullptr) {
      startFrameId_ = scalars[0];
    }
    const int64_t t0 = NowUs();
    engine_->Apply(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
    const int64_t total = NowUs() - t0;
    if (owner_ != nullptr) {
      owner_->RecordApply(cmdId, codecId, static_cast<uint64_t>(total));
      // Dev (kCodecAbEnabled): record the rects this command claims to write, so
      // GdiAbFlush - one callback later, with gdi and the engine at the same
      // stream position - can diff exactly those pixels against gdi's own surface.
      // Only the command's own rects are checked: the pixels a Progressive message
      // also *re-composites* (FreeRDP's update_tiles) are covered by the full-tile
      // check of the message that decoded them.
      // Dev (`kTileStateAbEnabled`): record the (tile,component) streams this
      // Progressive message decodes so TileStateAbFlush can diff the engine's own
      // predictor state against FreeRDP's for the same tiles. `progIndex_` numbers
      // the Progressive messages for both probes below, so it is bumped once even
      // when only the watch-tile probe is on.
      if ((kTileStateAbEnabled || kWatchTileEnabled) && cmdId == kGpuCmdWireToSurface &&
          payload != nullptr && codecId == kGpuCodecCaprogressive) {
        ++progIndex_;
        if (kWatchTileEnabled) {
          owner_->WatchTileNote(static_cast<uint16_t>(surfaceId));
        }
        if (kTileStateAbEnabled) {
          ParseRfxProgressive(
              payload, payloadLen,
              [&](const RfxTileRef& t) {
                owner_->TileStateAbCheck(static_cast<uint16_t>(surfaceId), t.xIdx, t.yIdx,
                                         progIndex_);
              },
              nullptr);
        }
      }
      // The whole-surface A/B runs on *every* command: the divergence it hunts can
      // be introduced by a non-Progressive command (a ClearCodec band, an
      // uncompressed upload, a cache restore) that the per-rect A/B does not cover.
      if (kSurfaceAbEnabled) {
        const char* op = "?";
        switch (cmdId) {
          case kGpuCmdWireToSurface:
            op = (codecId == kGpuCodecCaprogressive)     ? "progressive"
                 : (codecId == kGpuCodecClearCodec)      ? "clearcodec"
                 : (codecId == kGpuCodecUncompressed)    ? "upload"
                                                         : "wireOther";
            break;
          case kGpuCmdSolidFill:
            op = "fill";
            break;
          case kGpuCmdSurfaceToCache:
            op = "cacheStore";
            break;
          case kGpuCmdCacheToSurface:
            op = "cacheRestore";
            break;
          case kGpuCmdEvictCacheEntry:
            op = "evictCache";
            break;
          case kGpuCmdSurfaceToSurface:
            op = "surfaceToSurface";
            break;
          case kGpuCmdCreateSurface:
            op = "createSurface";
            break;
          case kGpuCmdDeleteSurface:
            op = "deleteSurface";
            break;
          case kGpuCmdMapSurfaceToOutput:
            op = "mapOutput";
            break;
          case kGpuCmdMapSurfaceToScaledOutput:
            op = "mapScaled";
            break;
          case kGpuCmdResetGraphics:
            op = "resetGraphics";
            break;
          case kGpuCmdStartFrame:
            op = "startFrame";
            break;
          case kGpuCmdEndFrame:
            op = "endFrame";
            break;
          default:
            break;
        }
        owner_->SurfaceAbCheck(static_cast<uint16_t>(surfaceId), op);
      }
      if (kCodecAbEnabled && params != nullptr) {
        if (cmdId == kGpuCmdWireToSurface && paramsLen >= 32 && payload != nullptr) {
          if (codecId == kGpuCodecCaprogressive) {
            const int ox = static_cast<int>(RdU32(params + 8));
            const int oy = static_cast<int>(RdU32(params + 12));
            ++progIndex_;
            // Count the message's shape first, then check its tiles: the culprit line
            // carries it so it can be matched against the `vk progressive msg #n`
            // log (a kFirst-diff message and an all-upgrade one are different bugs).
            uint32_t nTiles = 0;
            uint32_t nUpgrade = 0;
            uint32_t nDiff = 0;
            // The message's clip, as the *union bbox* of its raw region rects: the
            // engine/gdi clip with the band-merged region (a superset of the raw
            // rects), so this bbox is a superset of the real clip - over-including a
            // tile only costs a comparison that is expected to pass anyway.
            int clipX0 = 1 << 30;
            int clipY0 = 1 << 30;
            int clipX1 = -1;
            int clipY1 = -1;
            std::vector<uint32_t> ownTiles;
            ParseRfxProgressive(
                payload, payloadLen,
                [&](const RfxTileRef& t) {
                  if (t.type == RfxTileType::kUpgrade) {
                    nUpgrade++;
                  } else {
                    nTiles++;
                  }
                  if ((t.flags & 1u) != 0u) {
                    nDiff++;
                  }
                  ownTiles.push_back(static_cast<uint32_t>(t.xIdx) |
                                     (static_cast<uint32_t>(t.yIdx) << 16));
                },
                nullptr, nullptr,
                [&](const RfxRegionRef& region) {
                  for (uint16_t ri = 0; ri < region.numRects; ++ri) {
                    const RfxRect& r = region.rects[ri];
                    const int x0 = ox + r.x;
                    const int y0 = oy + r.y;
                    const int x1 = x0 + r.width;
                    const int y1 = y0 + r.height;
                    if (x0 < clipX0) clipX0 = x0;
                    if (y0 < clipY0) clipY0 = y0;
                    if (x1 > clipX1) clipX1 = x1;
                    if (y1 > clipY1) clipY1 = y1;
                  }
                });
            char label[64];
            std::snprintf(label, sizeof(label), "progressive#%d(%u/%u/%u)", progIndex_, nTiles,
                          nUpgrade, nDiff);
            ParseRfxProgressive(
                payload, payloadLen,
                [&](const RfxTileRef& t) {
                  owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId), ox + t.xIdx * 64,
                                     oy + t.yIdx * 64, 64, 64, label);
                },
                nullptr);
            // FreeRDP's update_tiles composites the frame's *whole* tile list on every
            // message (clipped by this message's region rects), so the tiles decoded by
            // an earlier message of the same frame are re-composited here (the engine's
            // type-3 "restamps"). The per-rect check above covers only the tiles this
            // message decodes itself, which is exactly why a restamp is the blind spot;
            // mirror the frame list and check those tiles as well.
            const uint16_t sid = static_cast<uint16_t>(surfaceId);
            if (frameIdForSurface_[sid] != startFrameId_) {
              frameIdForSurface_[sid] = startFrameId_;
              frameTilesForSurface_[sid].clear();
            }
            if (clipX1 >= clipX0 && clipY1 >= clipY0) {
              for (const uint32_t key : frameTilesForSurface_[sid]) {
                const int tx = static_cast<int>(key & 0xFFFFu);
                const int ty = static_cast<int>(key >> 16);
                bool own = false;
                for (const uint32_t o : ownTiles) {
                  if (o == key) {
                    own = true;
                    break;
                  }
                }
                if (own) {
                  continue;
                }
                const int px = ox + tx * 64;
                const int py = oy + ty * 64;
                if (px < clipX1 && px + 64 > clipX0 && py < clipY1 && py + 64 > clipY0) {
                  owner_->GdiAbCheck(sid, px, py, 64, 64, "restamp");
                }
              }
            }
            for (const uint32_t key : ownTiles) {
              frameTilesForSurface_[sid].push_back(key);
            }
          } else if (codecId == kGpuCodecClearCodec || codecId == kGpuCodecUncompressed) {
            const char* op = (codecId == kGpuCodecClearCodec) ? "clearcodec" : "upload";
            owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId),
                               static_cast<int>(RdU32(params + 8)),
                               static_cast<int>(RdU32(params + 12)),
                               static_cast<int>(RdU32(params + 24)),
                               static_cast<int>(RdU32(params + 28)), op);
          }
        } else if (cmdId == kGpuCmdSolidFill && scalars != nullptr) {
          for (uint32_t i = 0; i < scalars[1]; ++i) {
            const int left = static_cast<int>(RdU16(params + i * 8));
            const int top = static_cast<int>(RdU16(params + i * 8 + 2));
            const int right = static_cast<int>(RdU16(params + i * 8 + 4));
            const int bottom = static_cast<int>(RdU16(params + i * 8 + 6));
            owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId), left, top, right - left,
                               bottom - top, "fill");
          }
        } else if ((cmdId == kGpuCmdSurfaceToSurface || cmdId == kGpuCmdCacheToSurface) &&
                   scalars != nullptr && (cmdId == kGpuCmdSurfaceToSurface ? paramsLen >= 8 : true)) {
          // The dest points are u16 pairs; the blitted size comes from the engine's
          // surface rect for a copy and from the cache entry for a restore, so only
          // the points are known here - use a 64x64 window like the tile checks.
          const uint32_t count = scalars[1];
          const size_t base = (cmdId == kGpuCmdSurfaceToSurface) ? 8u : 0u;
          const char* op = (cmdId == kGpuCmdSurfaceToSurface) ? "surfaceToSurface" : "cacheRestore";
          for (uint32_t i = 0; i < count && (base + i * 4 + 4) <= paramsLen; ++i) {
            const int px = static_cast<int>(RdU16(params + base + i * 4));
            const int py = static_cast<int>(RdU16(params + base + i * 4 + 2));
            owner_->GdiAbCheck(static_cast<uint16_t>(surfaceId), px, py, 64, 64, op);
          }
        }
      }
    }
  }

 private:
  ReplayDesktop* engine_ = nullptr;
  GfxReplay* owner_ = nullptr;
  // Dev (kCodecAbEnabled): Progressive message counter, for the culprit label.
  int progIndex_ = 0;
  // Dev (kCodecAbEnabled): mirror of FreeRDP's per-frame updated-tile list, per
  // surface - the tiles an earlier message of the current frame decoded, which every
  // later message re-composites ("restamps"). Reset when the frame id changes.
  uint32_t startFrameId_ = 0;
  std::map<uint16_t, uint32_t> frameIdForSurface_;
  std::map<uint16_t, std::vector<uint32_t>> frameTilesForSurface_;
};

}  // namespace

GfxReplay& GfxReplay::Instance() {
  static GfxReplay instance;
  return instance;
}

GfxReplay::~GfxReplay() {
  Stop();
}

bool GfxReplay::Start(void* nativeWindow, int surfaceW, int surfaceH,
                      const std::string& gfxPath, GfxReplayRoute route) {
  Stop();
  if (nativeWindow == nullptr || surfaceW <= 0 || surfaceH <= 0 || gfxPath.empty()) {
    if (nativeWindow != nullptr) {
      OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(nativeWindow));
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    {
      std::lock_guard<std::mutex> err(errorMutex_);
      lastError_.clear();
      traffic_.clear();
    }
    window_ = nativeWindow;
    surfaceW_ = surfaceW;
    surfaceH_ = surfaceH;
    gfxPath_ = gfxPath;
    route_.store(static_cast<int>(route));
    cpuDesktop_ = nullptr;
    desktop_.reset();
    frames_.store(0);
    presents_.store(0);
    presentSkips_.store(0);
    presentFailures_.store(0);
    applyUs_.store(0);
    applyCount_.store(0);
    progUs_.store(0);
    progCount_.store(0);
    clearUs_.store(0);
    clearCount_.store(0);
    uncompUs_.store(0);
    uncompCount_.store(0);
    fillUs_.store(0);
    fillCount_.store(0);
    blitUs_.store(0);
    blitCount_.store(0);
    cacheUs_.store(0);
    cacheCount_.store(0);
    otherUs_.store(0);
    otherCount_.store(0);
    cmpChecks_.store(0);
    cmpBad_.store(0);
    cmpMaxDiff_.store(0);
    cmpRgbDiff_.store(0);
    cmpAlphaDiff_.store(0);
    cmpBBoxX0_.store(-1);
    cmpBBoxY0_.store(-1);
    cmpBBoxX1_.store(-1);
    cmpBBoxY1_.store(-1);
    cmpMaxDelta_.store(0);
    cmpSmallDeltaPx_.store(0);
    cmpFirstX_.store(-1);
    cmpFirstY_.store(-1);
    cmpDumpDone_ = false;
    // Dev A/B state is per run: without this a second replay inside one app
    // session keeps the previous run's "first culprit" and dump flags and logs
    // nothing.
    gdiChecks_.store(0);
    gdiBad_.store(0);
    gdiBadPx_.store(0);
    gdiFirstLogged_ = false;
    gdiBadOp_.clear();
    gdiFirstLine_.clear();
    gdiAbPending_.clear();
    presentUs_.store(0);
    pumpUs_.store(0);
    pumpStartUs_.store(0);
    paceUs_.store(0);
    lastFrameEndUs_ = 0;
    firstPacedUs_.store(0);
    startUs_.store(NowUs());
    endUs_.store(0);
    // The pure CPU route presents raw gdi frames through the Vulkan presenter;
    // the engine route builds its own presenter inside the worker.
    if (route == GfxReplayRoute::kCpu) {
      presenter_ = CreateFramePresenter();
      presenter_->SetSurface(window_, surfaceW_, surfaceH_);
    } else {
      presenter_.reset();
    }
    running_.store(true);
    thread_ = std::thread(&GfxReplay::Run, this);
  }
  const int64_t deadline = NowUs() + kStartWaitUs;
  while (NowUs() < deadline) {
    if (!running_.load() || presents_.load() > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!running_.load()) {
    std::lock_guard<std::mutex> err(errorMutex_);
    HMRDP_LOGE("gfx replay: start failed (%{public}s)",
               lastError_.empty() ? "unknown" : lastError_.c_str());
    Stop();
    return false;
  }
  HMRDP_LOGI("gfx replay: started route=%{public}s surface=%{public}dx%{public}d path=%{public}s",
             RouteName(static_cast<GfxReplayRoute>(route_.load())), surfaceW, surfaceH,
             gfxPath.c_str());
  return true;
}

void GfxReplay::Resize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  pendingW_.store(width);
  pendingH_.store(height);
}

void GfxReplay::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.exchange(false)) {
      if (!thread_.joinable()) {
        if (window_ != nullptr) {
          OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(window_));
          window_ = nullptr;
        }
        return;
      }
    }
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  presenter_.reset();
  if (window_ != nullptr) {
    OH_NativeWindow_DestroyNativeWindow(static_cast<OHNativeWindow*>(window_));
    window_ = nullptr;
  }
  HMRDP_LOGI("gfx replay: stopped frames=%{public}llu presents=%{public}llu",
             static_cast<unsigned long long>(frames_.load()),
             static_cast<unsigned long long>(presents_.load()));
}

std::string GfxReplay::StatsLines() {
  // Freeze the clock once the replay has ended: the UI keeps polling every
  // second, and a growing denominator would make fps/feed decay on a paused
  // (finished) replay.
  const int64_t stopUs = endUs_.load();
  const int64_t nowUs = stopUs != 0 ? stopUs : NowUs();
  // fps is the **playback** rate: measured from the first paced frame (see
  // PaceFrame) to the end, so the run's start-up (engine/presenter init, waiting for
  // the first frame) does not dilute it. PaceFrame counts one frame per paced
  // presentation, hence `presents - 1` periods.
  const uint64_t presents = presents_.load();
  const int64_t firstPacedUs = firstPacedUs_.load();
  const int64_t elapsedUs =
      firstPacedUs != 0 ? nowUs - firstPacedUs : (startUs_.load() != 0 ? nowUs - startUs_.load() : 0);
  const double fps =
      (elapsedUs > 0 && presents > 1)
          ? static_cast<double>(presents - 1) * 1000000.0 / static_cast<double>(elapsedUs)
          : 0.0;
  auto avgMs = [](uint64_t total, uint64_t count) {
    return count > 0 ? static_cast<double>(total) / static_cast<double>(count) / 1000.0 : 0.0;
  };
  std::string err;
  std::string traffic;
  {
    std::lock_guard<std::mutex> lock(errorMutex_);
    err = lastError_;
    traffic = traffic_;
  }
  const uint64_t pace = paceUs_.load();
  // "feed" = compute time, excluding the deliberate playback throttling. Once
  // the pump has returned its exact figure is used; while still running the wall
  // clock minus the paced sleep is a good live approximation.
  const uint64_t elapsed = elapsedUs > 0 ? static_cast<uint64_t>(elapsedUs) : 0;
  const uint64_t measuredPump = pumpUs_.load();
  uint64_t pumpUs = measuredPump > 0
                        ? (measuredPump > pace ? measuredPump - pace : 0)
                        : (elapsed > pace ? elapsed - pace : 0);
  // Exclude the run's start-up (engine/presenter init, first frame wait) so `feed`
  // is the playback's own compute, comparable between runs and routes.
  const int64_t firstPaced = firstPacedUs_.load();
  const int64_t pumpStart = pumpStartUs_.load();
  if (firstPaced != 0 && pumpStart != 0 && firstPaced > pumpStart &&
      static_cast<uint64_t>(firstPaced - pumpStart) < pumpUs) {
    pumpUs -= static_cast<uint64_t>(firstPaced - pumpStart);
  }

  const char* routeName = RouteName(static_cast<GfxReplayRoute>(route_.load()));
  const unsigned long long frames = static_cast<unsigned long long>(frames_.load());
  char head[320];
  std::snprintf(head, sizeof(head),
                "route=%s  frames=%llu  presents=%llu  fps=%.1f  fail=%llu  skip=%llu\n"
                "feed=%llums   parse=%llums   present=%.2fms   (running=%d)",
                routeName, frames,
                static_cast<unsigned long long>(presents), fps,
                static_cast<unsigned long long>(presentFailures_.load()),
                static_cast<unsigned long long>(presentSkips_.load()),
                static_cast<unsigned long long>(pumpUs / 1000),
                static_cast<unsigned long long>(hmrdp::GfxReplayParseUs() / 1000),
                avgMs(presentUs_.load(), presents),
                running_.load() ? 1 : 0);
  std::string out(head);

  // Per-command-class breakdown only exists on the GPU route (the CPU route
  // decodes inside FreeRDP and never calls the sink).
  if (applyCount_.load() > 0) {
    char cls[640];
    std::snprintf(
        cls, sizeof(cls),
        "\nprog  %.2fms x%llu\nclear %.2fms x%llu\nunc   %.2fms x%llu\n"
        "fill  %.2fms x%llu\nblit  %.2fms x%llu\ncache %.2fms x%llu\nother %.2fms x%llu",
        avgMs(progUs_.load(), progCount_.load()),
        static_cast<unsigned long long>(progCount_.load()),
        avgMs(clearUs_.load(), clearCount_.load()),
        static_cast<unsigned long long>(clearCount_.load()),
        avgMs(uncompUs_.load(), uncompCount_.load()),
        static_cast<unsigned long long>(uncompCount_.load()),
        avgMs(fillUs_.load(), fillCount_.load()),
        static_cast<unsigned long long>(fillCount_.load()),
        avgMs(blitUs_.load(), blitCount_.load()),
        static_cast<unsigned long long>(blitCount_.load()),
        avgMs(cacheUs_.load(), cacheCount_.load()),
        static_cast<unsigned long long>(cacheCount_.load()),
        avgMs(otherUs_.load(), otherCount_.load()),
        static_cast<unsigned long long>(otherCount_.load()));
    out += cls;
  }
  const uint64_t cmpChecks = cmpChecks_.load();
  if (cmpChecks > 0) {
    char cmp[192];
    std::snprintf(cmp, sizeof(cmp),
                  "\ncompare(GPU vs gdi): checks=%llu bad=%llu rgbPx=%llu alphaPx=%llu "
                  "maxRgbPx=%llu bbox=(%d,%d)-(%d,%d) maxDelta=%d smallDeltaPx=%llu",
                  static_cast<unsigned long long>(cmpChecks),
                  static_cast<unsigned long long>(cmpBad_.load()),
                  static_cast<unsigned long long>(cmpRgbDiff_.load()),
                  static_cast<unsigned long long>(cmpAlphaDiff_.load()),
                  static_cast<unsigned long long>(cmpMaxDiff_.load()), cmpBBoxX0_.load(),
                  cmpBBoxY0_.load(), cmpBBoxX1_.load(), cmpBBoxY1_.load(), cmpMaxDelta_.load(),
                  static_cast<unsigned long long>(cmpSmallDeltaPx_.load()));
    out += cmp;
  }
  const std::string rab = GdiAbSummary();
  if (!rab.empty()) {
    out += "\n";
    out += rab;
  }
  if (!tileStateProbe_.empty()) {
    out += "\n";
    out += tileStateProbe_;
  }
  if (!traffic.empty()) {
    out += "\n";
    out += traffic;
  }
  if (!err.empty()) {
    out += "\nerr=";
    out += err;
  }
  return out;
}

std::string GfxReplay::Stats() {
  // Single-line form for logging: the multi-line panel text with the newlines
  // flattened, so one hilog record carries the whole summary.
  std::string s = StatsLines();
  for (char& c : s) {
    if (c == '\n') {
      c = ' ';
    }
  }
  return s;
}

void GfxReplay::RecordApply(uint16_t cmdId, uint32_t codecId, uint64_t micros) {
  applyUs_.fetch_add(micros);
  applyCount_.fetch_add(1);
  std::atomic<uint64_t>* us = &otherUs_;
  std::atomic<uint64_t>* count = &otherCount_;
  if (cmdId == kGpuCmdWireToSurface) {
    if (codecId == kGpuCodecCaprogressive || codecId == kGpuCodecCaprogressiveV2) {
      us = &progUs_;
      count = &progCount_;
    } else if (codecId == kGpuCodecClearCodec) {
      us = &clearUs_;
      count = &clearCount_;
    } else {
      us = &uncompUs_;
      count = &uncompCount_;
    }
  } else if (cmdId == kGpuCmdSolidFill) {
    us = &fillUs_;
    count = &fillCount_;
  } else if (cmdId == kGpuCmdSurfaceToSurface) {
    us = &blitUs_;
    count = &blitCount_;
  } else if (cmdId == kGpuCmdSurfaceToCache || cmdId == kGpuCmdCacheToSurface ||
             cmdId == kGpuCmdEvictCacheEntry) {
    us = &cacheUs_;
    count = &cacheCount_;
  }
  us->fetch_add(micros);
  count->fetch_add(1);
}

void GfxReplay::RecordPresent(uint64_t micros) {
  presentUs_.fetch_add(micros);
}

void GfxReplay::PaceFrame(bool presented) {
  // Give every presented frame a kFrameMs period: once the frame's own work
  // (decode + command application + present) is done, sleep for the rest of the
  // budget.
  //
  // Nothing is carried over between frames, on purpose. A frame that overruns keeps
  // its longer period, and a frame that finishes early sleeps the remainder, so the
  // reported fps / present / feed figures describe the playback as it actually
  // happened. A "catch up on later frames" schedule would make the average reach the
  // target while individual frames were still measured under a different (faster)
  // cadence - exactly what a reference measurement must not do - and it does not
  // make playback any smoother either.
  //
  // Only frames that produced a picture are paced: the stream carries many frame
  // markers with no drawable update (management/ack frames, off-screen surfaces),
  // and those must not spend a frame budget (work between two presented frames is
  // still inside their period).
  if (!presented) {
    return;
  }
  const int64_t now = static_cast<int64_t>(NowUs());
  if (lastFrameEndUs_ != 0) {
    const int64_t target = lastFrameEndUs_ + static_cast<int64_t>(kFrameMs) * 1000;
    if (target > now) {
      std::this_thread::sleep_until(
          std::chrono::steady_clock::time_point(std::chrono::microseconds(target)));
      paceUs_.fetch_add(static_cast<uint64_t>(static_cast<int64_t>(NowUs()) - now));
    }
  }
  lastFrameEndUs_ = static_cast<int64_t>(NowUs());
  if (firstPacedUs_.load() == 0) {
    // fps is measured from here, so the run's start-up is not counted as playback.
    firstPacedUs_.store(lastFrameEndUs_);
  }
}

void GfxReplay::OnReplayFrame() {
  const int64_t nowUs = static_cast<int64_t>(NowUs());
  if (nowUs - startUs_.load() > kMaxRunUs) {
    HMRDP_LOGI("gfx replay: 120s cap reached");
    running_.store(false);
    return;
  }
  const int pw = pendingW_.exchange(0);
  const int ph = pendingH_.exchange(0);
  if (pw > 0 && ph > 0 && desktop_ != nullptr) {
    desktop_->Resize(pw, ph);
  }
  const int64_t presentStart = NowUs();
  const bool presented = desktop_ != nullptr && desktop_->Present();
  RecordPresent(static_cast<uint64_t>(NowUs() - presentStart));
  frames_.fetch_add(1);
  if (presented) {
    presents_.fetch_add(1);
  } else if (desktop_ != nullptr && !desktop_->screenDirty()) {
    // Nothing to show: no surface had a dirty region mapped to the output, so
    // Compose() bailed out before touching the screen. This is the normal
    // "static frame" case (typically the first frame markers before
    // ResetGraphics / before any drawable update), not a failure.
    const uint64_t n = presentSkips_.fetch_add(1) + 1;
    if (n <= 4) {
      HMRDP_LOGI("gfx replay: no present #%{public}llu at frame=%{public}llu (nothing dirty)",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(frames_.load()));
    }
  } else {
    presentFailures_.fetch_add(1);
    const uint64_t n = presentFailures_.load();
    if (n <= 4) {
      HMRDP_LOGW("gfx replay: present failed #%{public}llu at frame=%{public}llu",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(frames_.load()));
    }
  }
  if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
    if (desktop_ != nullptr) {
      const std::string t = desktop_->Summary();
      std::lock_guard<std::mutex> lock(errorMutex_);
      traffic_ = t;
    }
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  PaceFrame(presented);
}

void GfxReplay::Run() {
  switch (static_cast<GfxReplayRoute>(route_.load())) {
    case GfxReplayRoute::kCpu:
      RunCpuReplay(gfxPath_);
      break;
    case GfxReplayRoute::kVulkan:
      RunVulkanReplay(gfxPath_, false);
      break;
    case GfxReplayRoute::kVulkanCompare:
      RunVulkanReplay(gfxPath_, true);
      break;
  }
  endUs_.store(NowUs());
  running_.store(false);
}

void GfxReplay::RunVulkanReplay(const std::string& gfxPath, bool compare) {
  std::unique_ptr<ReplayDesktop> desktop(new ReplayDesktop());
  std::string error;
  if (!desktop->Init(window_, surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error.empty() ? "engine init failed" : error;
    return;
  }
  desktop_ = std::move(desktop);
  // The correctness routes feed the exact same bytes into FreeRDP's own gdi
  // pipeline and compare the two composed screens. The CPU desktop decodes but
  // never presents, so there is no double present and no timing meaning here.
  GfxCpuDesktop cpu;
  if (compare) {
    if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
      std::lock_guard<std::mutex> err(errorMutex_);
      lastError_ = error;
      desktop_.reset();
      return;
    }
    cpuDesktop_ = &cpu;
  }
  ReplaySink sink(desktop_.get(), this);

  // The replayed stream must not re-trigger the capture hook on the recorder.
  hmrdp::GfxDumpSetReplaying(true);
  hmrdp::GfxReplayResetParseUs();

  const int64_t pumpStart = NowUs();
  pumpStartUs_.store(pumpStart);

  bool ok = false;
  // Dev: the watch-tile probe counts Progressive messages, so its counters belong to
  // this run (the page restarts the replay on every route / `重新回放` click, and the
  // engine instance is re-created with it).
  watchMsgCount_ = 0;
  watchProbed_ = 0;
  watchSeen_ = false;
  if (compare) {
    // gdi and the engine consume the capture interleaved per PDU, so the
    // per-command A/B (GdiAbFlush, only with kCodecAbEnabled) runs right after
    // every command and CompareFrames() on gdi's EndFrame - both with both sides
    // at the same stream position. OnReplayFrame keeps presenting and pacing.
    ok = GfxReplayStreamCompare(gfxPath, &sink, [this]() { OnReplayFrame(); },
                                [this]() { CompareFrames(); },
                                [this]() {
                                  GdiAbFlush();
                                  TileStateAbFlush();
                                  WatchTileProbe();
                                },
                                cpu.gfx(), &running_, &error);
  } else {
    ok = GfxReplayStream(gfxPath, &sink, [this]() { OnReplayFrame(); }, &running_, &error);
  }
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  {
    const std::string summary = desktop_->Summary();
    std::lock_guard<std::mutex> lock(errorMutex_);
    traffic_ = summary;
  }
  hmrdp::GfxDumpSetReplaying(false);
  cpuDesktop_ = nullptr;
  if (!gdiFirstLine_.empty()) {
    // The periodic stats line floods hilog within seconds, so repeat the first
    // culprit once here (it is the entry point for every follow-up session).
    HMRDP_LOGW("gfx replay: gdiAB FIRST CULPRIT (rerun) %{public}s", gdiFirstLine_.c_str());
  }
  {
    // Dev: independent-reference divergence summary.
    const std::string ab = GdiAbSummary();
    if (!ab.empty()) {
      HMRDP_LOGI("gfx replay: %{public}s", ab.c_str());
    }
  }
  const int64_t releaseStart = NowUs();
  desktop_.reset();
  HMRDP_LOGI("gfx replay: engine teardown %{public}llu ms",
             static_cast<unsigned long long>((NowUs() - releaseStart) / 1000));
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished (route=%{public}s): %{public}s",
             RouteName(static_cast<GfxReplayRoute>(route_.load())), Stats().c_str());
}

void GfxReplay::RunCpuReplay(const std::string& gfxPath) {
  // FreeRDP's own gdi pipeline handles the decoding (clear/progressive/...),
  // exactly like a live session. Only the destination changes: gdi's primary
  // buffer is uploaded through the presenter's CPU frame path instead of the
  // engine's screen texture, so this route is the CPU reference for the engine.
  presenter_->Prepare();
  GfxCpuDesktop cpu;
  std::string error;
  if (!cpu.Init(surfaceW_, surfaceH_, &error)) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
    return;
  }
  cpu.SetFrameFn([this, &cpu]() { OnCpuFrame(&cpu); });

  hmrdp::GfxDumpSetReplaying(true);
  hmrdp::GfxReplayResetParseUs();

  const int64_t pumpStart = NowUs();
  pumpStartUs_.store(pumpStart);

  const bool ok = GfxReplayPump(gfxPath, cpu.gfx(), &running_, &error);
  pumpUs_.store(static_cast<uint64_t>(NowUs() - pumpStart));
  HMRDP_LOGI("gfx replay: pump %{public}llu ms (paced %{public}llu ms)",
             static_cast<unsigned long long>(pumpUs_.load() / 1000),
             static_cast<unsigned long long>(paceUs_.load() / 1000));

  hmrdp::GfxDumpSetReplaying(false);
  cpu.SetFrameFn(nullptr);
  if (!ok) {
    std::lock_guard<std::mutex> err(errorMutex_);
    lastError_ = error;
  }
  HMRDP_LOGI("gfx replay: finished (cpu): %{public}s", Stats().c_str());
}

std::string GfxReplay::GdiAbSummary() const {
  if (gdiChecks_.load() == 0) {
    return std::string();
  }
  char buf[256];
  std::snprintf(buf, sizeof(buf), "gdiAB checks=%llu bad=%llu badPx=%llu firstBadOp=%s",
                static_cast<unsigned long long>(gdiChecks_.load()),
                static_cast<unsigned long long>(gdiBad_.load()),
                static_cast<unsigned long long>(gdiBadPx_.load()),
                gdiBadOp_.empty() ? "-" : gdiBadOp_.c_str());
  return std::string(buf);
}

void GfxReplay::GdiAbCheck(uint16_t surfaceId, int x, int y, int width, int height,
                           const char* op) {
  // Records the rect this command claims to write; GdiAbFlush compares it against
  // gdi's own surface right after the command (both sides are at the same stream
  // position there, so the first failure names the culprit command).
  if (cpuDesktop_ == nullptr || width <= 0 || height <= 0 || gdiAbPending_.size() >= 4096) {
    return;
  }
  GdiAbRect rect;
  rect.x = x;
  rect.y = y;
  rect.w = width;
  rect.h = height;
  rect.surfaceId = surfaceId;
  rect.op = op;
  gdiAbPending_.push_back(rect);
}

void GfxReplay::GdiAbFlush() {
  // Authoritative A/B: the engine's surface vs FreeRDP's own gdi surface, for the
  // same id and at the same stream position. (An earlier, hand-written mirror of
  // the gdi geometry was removed: it produced false positives.)
  if (cpuDesktop_ == nullptr || gdiAbPending_.empty()) {
    return;
  }
  std::vector<GdiAbRect> pending;
  pending.swap(gdiAbPending_);
  for (const GdiAbRect& rect : pending) {
    int x = rect.x;
    int y = rect.y;
    int width = rect.w;
    int height = rect.h;
    int gw = 0;
    int gh = 0;
    int gstride = 0;
    uint32_t gformat = 0;
    const uint8_t* gdiSurface =
        cpuDesktop_->SurfaceData(rect.surfaceId, &gw, &gh, &gstride, &gformat);
    if (gdiSurface == nullptr || gstride <= 0 || gw <= 0 || gh <= 0) {
      continue;
    }
    if (x < 0) {
      width += x;
      x = 0;
    }
    if (y < 0) {
      height += y;
      y = 0;
    }
    if (x + width > gw) {
      width = gw - x;
    }
    if (y + height > gh) {
      height = gh - y;
    }
    if (width <= 0 || height <= 0) {
      continue;
    }
    std::vector<uint8_t> actual;
    if (!desktop_->ReadSurfaceRect(rect.surfaceId, x, y, width, height, &actual)) {
      continue;
    }
    if (actual.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
      continue;
    }
    gdiChecks_.fetch_add(1);
    uint64_t bad = 0;
    int firstX = -1;
    int firstY = -1;
    int maxDelta = 0;
    for (int row = 0; row < height; ++row) {
      const uint8_t* a = actual.data() + static_cast<size_t>(row) * width * 4;
      const uint8_t* b =
          gdiSurface + static_cast<size_t>(y + row) * gstride + static_cast<size_t>(x) * 4;
      for (int col = 0; col < width; ++col) {
        if (a[col * 4 + 0] == b[col * 4 + 0] && a[col * 4 + 1] == b[col * 4 + 1] &&
            a[col * 4 + 2] == b[col * 4 + 2]) {
          continue;
        }
        if (firstX < 0) {
          firstX = x + col;
          firstY = y + row;
        }
        for (int k = 0; k < 3; ++k) {
          const int d = static_cast<int>(a[col * 4 + k]) - static_cast<int>(b[col * 4 + k]);
          const int ad = d < 0 ? -d : d;
          if (ad > maxDelta) {
            maxDelta = ad;
          }
        }
        bad++;
      }
    }
    if (bad == 0) {
      continue;
    }
    gdiBad_.fetch_add(1);
    gdiBadPx_.fetch_add(bad);
    if (!gdiFirstLogged_) {
      gdiFirstLogged_ = true;
      if (gdiBadOp_.empty()) {
        gdiBadOp_ = rect.op;
      }
      // Report the first failing *pixel* (both sides), not the rect origin.
      const size_t off = (static_cast<size_t>(firstY) * gstride) + static_cast<size_t>(firstX) * 4;
      const size_t eoff =
          (static_cast<size_t>(firstY - y) * static_cast<size_t>(width) +
           static_cast<size_t>(firstX - x)) *
          4;
      char firstLine[320];
      std::snprintf(firstLine, sizeof(firstLine),
                    "op=%s rect=(%d,%d)+%dx%d bad=%llu maxDelta=%d px=(%d,%d) engine=b%u g%u r%u gdi=b%u g%u r%u",
                    rect.op.c_str(), x, y, width, height, static_cast<unsigned long long>(bad),
                    maxDelta, firstX, firstY, actual[eoff], actual[eoff + 1], actual[eoff + 2],
                    gdiSurface[off], gdiSurface[off + 1], gdiSurface[off + 2]);
      gdiFirstLine_ = firstLine;
      HMRDP_LOGW("gfx replay: gdiAB CULPRIT %{public}s", firstLine);
    }
  }
}

void GfxReplay::TileStateAbCheck(uint16_t surfaceId, uint16_t xIdx, uint16_t yIdx,
                                 int progIndex) {
  if (!kTileStateAbEnabled || cpuDesktop_ == nullptr || desktop_ == nullptr) {
    return;
  }
  if (tileStateAbFirst_ || progIndex > kTileStateAbMessages) {
    return;
  }
  TileStatePending pending;
  pending.surfaceId = surfaceId;
  pending.xIdx = xIdx;
  pending.yIdx = yIdx;
  pending.progIndex = progIndex;
  tileStatePending_.push_back(pending);
}

void GfxReplay::TileStateAbFlush() {
  if (!kTileStateAbEnabled || tileStatePending_.empty() || cpuDesktop_ == nullptr ||
      desktop_ == nullptr || tileStateAbFirst_) {
    tileStatePending_.clear();
    return;
  }
  std::vector<TileStatePending> pending;
  pending.swap(tileStatePending_);
  // One submit per message: the decode batch must have executed before the
  // engine's state mapping can be read.
  if (!desktop_->Flush()) {
    return;
  }
  std::vector<int16_t> engCur(3u * 4096u);
  std::vector<int16_t> engSign(3u * 4096u);
  uint8_t engBitPos[30] = {0};
  for (const TileStatePending& t : pending) {
    if (!desktop_->TileState(t.surfaceId, t.xIdx, t.yIdx, engCur.data(), engSign.data(),
                             engBitPos)) {
      continue;
    }
    const int16_t* gdiCur[3] = {nullptr, nullptr, nullptr};
    const int16_t* gdiSign[3] = {nullptr, nullptr, nullptr};
    uint8_t gdiBitPos[30] = {0};
    if (!cpuDesktop_->TileState(t.surfaceId, t.xIdx, t.yIdx, gdiCur, gdiSign, gdiBitPos)) {
      tileStateAbUnavailable_++;
      continue;
    }
    tileStateAbChecks_++;
    // The engine's planes are laid out exactly like gdi's (see ReadTileState).
    const char* which = nullptr;
    int comp = 0;
    int index = 0;
    int engineValue = 0;
    int gdiValue = 0;
    // Cause order, not symptom order: the bit positions decide the dequantise shift
    // (band order), `sign` is the *raw* RLGR output and `cur` the dequantised plane -
    // so the first of the three that differs names the stage.
    for (int c = 0; c < 3 && which == nullptr; ++c) {
      for (int b = 0; b < 10; ++b) {
        const int e = engBitPos[c * 10 + b];
        const int g = gdiBitPos[c * 10 + b];
        if (e != g) {
          which = "bitPos";
          comp = c;
          index = b;
          engineValue = e;
          gdiValue = g;
          break;
        }
      }
    }
    for (int c = 0; c < 3 && which == nullptr; ++c) {
      for (int k = 0; k < 4096; ++k) {
        const int e = engSign[static_cast<size_t>(c) * 4096u + k];
        const int g = gdiSign[c][k];
        if (e != g) {
          which = "sign(raw)";
          comp = c;
          index = k;
          engineValue = e;
          gdiValue = g;
          break;
        }
      }
    }
    for (int c = 0; c < 3 && which == nullptr; ++c) {
      for (int k = 0; k < 4096; ++k) {
        const int e = engCur[static_cast<size_t>(c) * 4096u + k];
        const int g = gdiCur[c][k];
        if (e != g) {
          which = "cur";
          comp = c;
          index = k;
          engineValue = e;
          gdiValue = g;
          break;
        }
      }
    }
    if (which == nullptr) {
      continue;
    }
    tileStateAbFirst_ = true;
    // Dump the same position out of all three arrays: the stage that first differs
    // names the bug, and the raw/shift pair next to `cur` says whether the dequantise
    // input or the shift itself is the one that disagrees.
    const int band = index < 4096 ? (index >= 4015 ? 9
                                     : index >= 3951 ? 8
                                     : index >= 3879 ? 7
                                     : index >= 3807 ? 6
                                     : index >= 3551 ? 5
                                     : index >= 3279 ? 4
                                     : index >= 3007 ? 3
                                     : index >= 2046 ? 2
                                     : index >= 1023 ? 1
                                                     : 0)
                                  : 0;
    const int k = which != nullptr && index < 4096 ? index : 0;
    // Dump the hit plus its neighbours out of `cur` and `sign` on both sides: a band
    // that is *shifted* by one element looks completely different from one where a
    // single coefficient was dequantised/summed differently.
    char aroundE[256];
    char aroundG[256];
    char aroundSE[256];
    char aroundSG[256];
    size_t ne = 0;
    size_t ng = 0;
    size_t nse = 0;
    size_t nsg = 0;
    aroundE[0] = aroundG[0] = aroundSE[0] = aroundSG[0] = '\0';
    for (int d = -5; d <= 5; ++d) {
      const int idx = k + d;
      if (idx < 0 || idx >= 4096) {
        continue;
      }
      const size_t co = static_cast<size_t>(comp) * 4096u + static_cast<size_t>(idx);
      ne += static_cast<size_t>(std::snprintf(aroundE + ne, sizeof(aroundE) - ne, "%s%d",
                                              ne == 0 ? "" : ",", static_cast<int>(engCur[co])));
      ng += static_cast<size_t>(std::snprintf(aroundG + ng, sizeof(aroundG) - ng, "%s%d",
                                              ng == 0 ? "" : ",", static_cast<int>(gdiCur[comp][idx])));
      nse += static_cast<size_t>(std::snprintf(aroundSE + nse, sizeof(aroundSE) - nse, "%s%d",
                                               nse == 0 ? "" : ",",
                                               static_cast<int>(engSign[co])));
      nsg += static_cast<size_t>(std::snprintf(aroundSG + nsg, sizeof(aroundSG) - nsg, "%s%d",
                                               nsg == 0 ? "" : ",",
                                               static_cast<int>(gdiSign[comp][idx])));
    }
    char line[1400];
    std::snprintf(line, sizeof(line),
                  "tileState CULPRIT msg=#%d tile=(%d,%d) %s c=%d i=%d engine=%d gdi=%d | "
                  "signE=%d signG=%d curE=%d curG=%d band=%d bpE=%u bpG=%u "
                  "curE[%d..]=[%s] curG=[%s] signE=[%s] signG=[%s]",
                  t.progIndex, static_cast<int>(t.xIdx), static_cast<int>(t.yIdx), which, comp,
                  index, engineValue, gdiValue,
                  static_cast<int>(engSign[static_cast<size_t>(comp) * 4096u + k]),
                  static_cast<int>(gdiSign[comp][k]),
                  static_cast<int>(engCur[static_cast<size_t>(comp) * 4096u + k]),
                  static_cast<int>(gdiCur[comp][k]), band,
                  static_cast<unsigned>(engBitPos[comp * 10 + band]),
                  static_cast<unsigned>(gdiBitPos[comp * 10 + band]), k - 5, aroundE, aroundG,
                  aroundSE, aroundSG);
    HMRDP_LOGW("gfx replay: %{public}s", line);
    {
      std::lock_guard<std::mutex> lock(errorMutex_);
      tileStateProbe_ = line;
    }
    break;
  }
  // Always leave a status line, so "no divergence in the checked messages" and
  // "the reference accessor is not available" cannot be confused.
  if (tileStateProbe_.empty()) {
    char status[160];
    std::snprintf(status, sizeof(status), "tileState checks=%llu unavailable=%llu (msgs<=%d)",
                  static_cast<unsigned long long>(tileStateAbChecks_),
                  static_cast<unsigned long long>(tileStateAbUnavailable_), kTileStateAbMessages);
    std::lock_guard<std::mutex> lock(errorMutex_);
    tileStateProbe_ = status;
  }
}

void GfxReplay::WatchTileNote(uint16_t surfaceId) {
  if (!kWatchTileEnabled) {
    return;
  }
  ++watchMsgCount_;
  watchSurfaceId_ = surfaceId;
  watchSeen_ = true;
}

void GfxReplay::WatchTileProbe() {
  if (!kWatchTileEnabled || desktop_ == nullptr || cpuDesktop_ == nullptr || !watchSeen_) {
    return;
  }
  // Once per Progressive message: the probe describes the state *after* a message,
  // so it must not re-run for the non-Progressive commands in between.
  if (watchProbed_ >= watchMsgCount_ || watchMsgCount_ > kWatchTileMessages) {
    return;
  }
  watchProbed_ = watchMsgCount_;
  // One submit per message: the decode batch must have executed before the
  // engine's state mapping can be read.
  if (!desktop_->Flush()) {
    return;
  }
  std::vector<int16_t> engCur(3u * 4096u);
  std::vector<int16_t> engSign(3u * 4096u);
  uint8_t engBitPos[30] = {0};
  if (!desktop_->TileState(watchSurfaceId_, kWatchTileX, kWatchTileY, engCur.data(),
                           engSign.data(), engBitPos)) {
    HMRDP_LOGW("gfx replay: watch msg=#%d tile=(%u,%u) engine state unavailable", watchMsgCount_,
               static_cast<unsigned>(kWatchTileX), static_cast<unsigned>(kWatchTileY));
    return;
  }
  const int16_t* gdiCur[3] = {nullptr, nullptr, nullptr};
  const int16_t* gdiSign[3] = {nullptr, nullptr, nullptr};
  uint8_t gdiBitPos[30] = {0};
  if (!cpuDesktop_->TileState(watchSurfaceId_, kWatchTileX, kWatchTileY, gdiCur, gdiSign,
                              gdiBitPos)) {
    HMRDP_LOGW("gfx replay: watch msg=#%d tile=(%u,%u) gdi state unavailable", watchMsgCount_,
               static_cast<unsigned>(kWatchTileX), static_cast<unsigned>(kWatchTileY));
    return;
  }
  // Band order: HL1 LH1 HH1 HL2 LH2 HH2 HL3 LH3 HH3 LL3 (FreeRDP's write order).
  static const char kCompName[3] = {'Y', 'C', 'R'};
  char bpDiff[256];
  size_t nd = 0;
  bpDiff[0] = '\0';
  for (int c = 0; c < 3; ++c) {
    for (int b = 0; b < 10; ++b) {
      const unsigned e = engBitPos[c * 10 + b];
      const unsigned g = gdiBitPos[c * 10 + b];
      if (e != g) {
        nd += static_cast<size_t>(std::snprintf(bpDiff + nd, sizeof(bpDiff) - nd,
                                                "%s%c:b%d:%u>%u", nd ? " " : "", kCompName[c], b, e,
                                                g));
      }
    }
  }
  if (nd == 0) {
    std::snprintf(bpDiff, sizeof(bpDiff), "none");
  }
  int signDiff = 0;
  int curDiff = 0;
  unsigned long long curAbs = 0;
  for (int c = 0; c < 3; ++c) {
    for (int k = 0; k < 4096; ++k) {
      const int e = engCur[static_cast<size_t>(c) * 4096u + static_cast<size_t>(k)];
      const int g = gdiCur[c][k];
      if (e != g) {
        curDiff++;
        curAbs += static_cast<unsigned long long>((e > g) ? (e - g) : (g - e));
      }
      if (engSign[static_cast<size_t>(c) * 4096u + static_cast<size_t>(k)] != gdiSign[c][k]) {
        signDiff++;
      }
    }
  }
  char curE[192];
  char curG[192];
  char signE[192];
  char signG[192];
  size_t ce = 0;
  size_t cg = 0;
  size_t se = 0;
  size_t sg = 0;
  curE[0] = curG[0] = signE[0] = signG[0] = '\0';
  for (int k = kWatchWindowStart; k < kWatchWindowStart + kWatchWindowCount; ++k) {
    ce += static_cast<size_t>(std::snprintf(curE + ce, sizeof(curE) - ce, "%s%d", ce ? "," : "",
                                            static_cast<int>(engCur[static_cast<size_t>(k)])));
    cg += static_cast<size_t>(std::snprintf(curG + cg, sizeof(curG) - cg, "%s%d", cg ? "," : "",
                                            static_cast<int>(gdiCur[0][k])));
    se += static_cast<size_t>(std::snprintf(signE + se, sizeof(signE) - se, "%s%d", se ? "," : "",
                                            static_cast<int>(engSign[static_cast<size_t>(k)])));
    sg += static_cast<size_t>(std::snprintf(signG + sg, sizeof(signG) - sg, "%s%d", sg ? "," : "",
                                            static_cast<int>(gdiSign[0][k])));
  }
  // One pre-formatted string: hilog redacts plain conversion specifiers (only
  // `%{public}...` survives), and a probe whose numbers are `<private>` is useless.
  char line[768];
  std::snprintf(line, sizeof(line),
                "watch msg=#%d surf=%u tile=(%u,%u) bpDiff=[%s] signDiff=%d curDiff=%d "
                "curAbs=%llu | Y.cur[%d..]=E[%s] G[%s] Y.sign=E[%s] G[%s]",
                watchMsgCount_, static_cast<unsigned>(watchSurfaceId_),
                static_cast<unsigned>(kWatchTileX), static_cast<unsigned>(kWatchTileY), bpDiff,
                signDiff, curDiff, curAbs, kWatchWindowStart, curE, curG, signE, signG);
  HMRDP_LOGW("gfx replay: %{public}s", line);
}

void GfxReplay::SurfaceAbCheck(uint16_t surfaceId, const char* op) {
  // Whole-surface A/B, run right after a command with gdi and the engine at the
  // same stream position. The engine's Progressive decode is batched, so the batch
  // has to be submitted before the mapping can be read.
  if (!kSurfaceAbEnabled || cpuDesktop_ == nullptr || desktop_ == nullptr ||
      surfaceAbChecks_ >= kSurfaceAbMaxChecks) {
    return;
  }
  int gw = 0;
  int gh = 0;
  int gstride = 0;
  uint32_t gformat = 0;
  const uint8_t* gsurf = cpuDesktop_->SurfaceData(surfaceId, &gw, &gh, &gstride, &gformat);
  if (gsurf == nullptr || gw <= 0 || gh <= 0 || gstride <= 0) {
    return;
  }
  int ew = 0;
  int eh = 0;
  int estride = 0;
  if (!desktop_->SurfaceSize(surfaceId, &ew, &eh, &estride) || ew <= 0 || eh <= 0 ||
      estride <= 0) {
    return;
  }
  if (!desktop_->Flush()) {
    return;
  }
  std::vector<uint8_t> eng;
  if (!desktop_->ReadSurface(surfaceId, &eng)) {
    return;
  }
  const int cmpW = gw < ew ? gw : ew;
  const int cmpH = gh < eh ? gh : eh;
  surfaceAbChecks_++;
  const int tgw = (cmpW + 63) / 64;
  const int tgh = (cmpH + 63) / 64;
  std::vector<int> tileDiff(static_cast<size_t>(tgw) * static_cast<size_t>(tgh), 0);
  uint64_t bad = 0;
  int firstX = -1;
  int firstY = -1;
  uint32_t firstEngine = 0;
  uint32_t firstGdi = 0;
  for (int row = 0; row < cmpH; ++row) {
    const size_t eoff = static_cast<size_t>(row) * estride;
    const size_t goff = static_cast<size_t>(row) * gstride;
    if (eoff + static_cast<size_t>(cmpW) * 4u > eng.size()) {
      break;
    }
    const uint8_t* a = eng.data() + eoff;
    const uint8_t* b = gsurf + goff;
    for (int col = 0; col < cmpW; ++col) {
      const uint8_t* pa = a + col * 4;
      const uint8_t* pb = b + col * 4;
      if (pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2]) {
        continue;
      }
      if (firstX < 0) {
        firstX = col;
        firstY = row;
        std::memcpy(&firstEngine, pa, 4);
        std::memcpy(&firstGdi, pb, 4);
      }
      tileDiff[static_cast<size_t>(row / 64) * tgw + col / 64]++;
      bad++;
    }
  }
  // Log the progression of the first checks (a divergence that appears at message
  // N and then stays, versus one that heals, are different bugs), and after that
  // only the checks that actually diverge.
  if (surfaceAbChecks_ <= 60 || (bad > 0 && !surfaceAbFirst_)) {
    if (bad > 0) {
      surfaceAbFirst_ = true;
    }
    // Tile shape: a whole-tile difference (the engine never landed the tile) versus
    // a value-level one (its coefficients differ), plus the first few tiles.
    int tilesTouched = 0;
    int tilesFull = 0;
    std::string firstTiles;
    for (int ty = 0; ty < tgh; ++ty) {
      for (int tx = 0; tx < tgw; ++tx) {
        const int n = tileDiff[static_cast<size_t>(ty) * tgw + tx];
        if (n == 0) {
          continue;
        }
        tilesTouched++;
        if (n >= 4000) {
          tilesFull++;
        }
        if (firstTiles.size() < 60) {
          char b2[32];
          std::snprintf(b2, sizeof(b2), "(%d,%d)=%d ", tx, ty, n);
          firstTiles += b2;
        }
      }
    }
    HMRDP_LOGW("gfx replay: surfaceAB #%{public}llu op=%{public}s sid=%{public}u "
               "eng=%{public}dx%{public}d gdi=%{public}dx%{public}d bad=%{public}llu "
               "touched=%{public}d full=%{public}d first=(%{public}d,%{public}d) "
               "tile=(%{public}d,%{public}d) engine=0x%{public}x gdi=0x%{public}x tiles=%{public}s",
               static_cast<unsigned long long>(surfaceAbChecks_), op, surfaceId, ew, eh, gw, gh,
               static_cast<unsigned long long>(bad), tilesTouched, tilesFull, firstX, firstY,
               firstX / 64, firstY / 64, static_cast<unsigned>(firstEngine),
               static_cast<unsigned>(firstGdi), firstTiles.c_str());
  }
}

void GfxReplay::CompareFrames() {
  ReplayDesktop* engine = desktop_.get();
  GfxCpuDesktop* cpu = cpuDesktop_;
  if (engine == nullptr || cpu == nullptr) {
    return;
  }
  // Sample like the live shadow check: a full screen readback is expensive.
  if ((frames_.load() % static_cast<uint64_t>(kCompareEvery)) != 0) {
    return;
  }
  rdpGdi* gdi = cpu->gdi();
  const int w = engine->screenWidth();
  const int h = engine->screenHeight();
  if (gdi == nullptr || gdi->primary_buffer == nullptr || w <= 0 || h <= 0) {
    return;
  }
  const int cmpW = w < static_cast<int>(gdi->width) ? w : static_cast<int>(gdi->width);
  const int cmpH = h < static_cast<int>(gdi->height) ? h : static_cast<int>(gdi->height);
  if (cmpW <= 0 || cmpH <= 0) {
    return;
  }
  std::vector<uint8_t> screen;
  if (!engine->ReadScreen(&screen)) {
    return;
  }
  if (screen.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
    return;
  }
  // Separate RGB differences (a real decoding/composition mismatch) from
  // alpha-only differences (BGRX surfaces: the two decoders may disagree only on
  // the unused byte, which is not a visual difference).
  size_t diffRgb = 0;
  size_t diffAlpha = 0;
  int firstX = -1;
  int firstY = -1;
  // Bounding box of the RGB differences and the largest per-channel delta:
  // "one rectangle" means a command/region the engine failed to paint (or
  // composed differently), while "+/-1 everywhere" means rounding.
  int bx0 = cmpW;
  int by0 = cmpH;
  int bx1 = -1;
  int by1 = -1;
  int maxDelta = 0;
  size_t smallDeltaPx = 0;  // all channels within +/-2: rounding-level
  // Attribution counters (dev diagnostics): distinguishes "the engine never
  // painted" from "it painted a different colour" from "channels swapped".
  size_t engWhiteOnly = 0;  // engine white (0xFFFFFFFF), gdi not: missing paint
  size_t gdiWhiteOnly = 0;  // gdi white, engine not: extra paint / wrong clip
  size_t chanSwap = 0;      // engine pixel == gdi with R/B swapped
  size_t delta255 = 0;      // at least one channel off by 255
  uint32_t firstEngine = 0;
  uint32_t firstGdi = 0;
  for (int row = 0; row < cmpH; ++row) {
    const uint8_t* a = screen.data() + static_cast<size_t>(row) * w * 4;
    const uint8_t* b = gdi->primary_buffer + static_cast<size_t>(row) * gdi->stride;
    for (int col = 0; col < cmpW; ++col) {
      const uint8_t* pa = a + col * 4;
      const uint8_t* pb = b + col * 4;
      if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) {
        if (firstX < 0) {
          firstX = col;
          firstY = row;
          std::memcpy(&firstEngine, pa, 4);
          std::memcpy(&firstGdi, pb, 4);
        }
        if (col < bx0) bx0 = col;
        if (row < by0) by0 = row;
        if (col > bx1) bx1 = col;
        if (row > by1) by1 = row;
        int worst = 0;
        for (int k = 0; k < 3; ++k) {
          const int d = static_cast<int>(pa[k]) - static_cast<int>(pb[k]);
          const int ad = d < 0 ? -d : d;
          if (ad > worst) worst = ad;
        }
        if (worst > maxDelta) maxDelta = worst;
        if (worst <= 2) smallDeltaPx++;
        if (worst == 255) delta255++;
        const bool engWhite = (pa[0] == 0xFF && pa[1] == 0xFF && pa[2] == 0xFF);
        const bool gdiWhite = (pb[0] == 0xFF && pb[1] == 0xFF && pb[2] == 0xFF);
        if (engWhite && !gdiWhite) {
          engWhiteOnly++;
        } else if (gdiWhite && !engWhite) {
          gdiWhiteOnly++;
        } else if (pa[0] == pb[2] && pa[1] == pb[1] && pa[2] == pb[0]) {
          chanSwap++;
        }
        diffRgb++;
      } else if (pa[3] != pb[3]) {
        diffAlpha++;
      }
    }
  }
  cmpSmallDeltaPx_.fetch_add(smallDeltaPx);
  // Dev diagnosis: on the first divergence, write the two screens (RGB24 PPM) and
  // a per-64x64-tile diff summary. The pixel dump alone cannot say whether a tile
  // is *entirely* wrong (a missed/stale tile) or only drifts by a few levels (a
  // coefficient-level decode difference); the images and the tile histogram do.
  if (firstX >= 0 && cmpScreenDumpDone_ == false) {
    cmpScreenDumpDone_ = true;
    auto writePpm = [&](const std::string& path, const uint8_t* buf, int stride) {
      std::ofstream ppm(path, std::ios::out | std::ios::trunc | std::ios::binary);
      if (!ppm) {
        return;
      }
      ppm << "P6\n" << cmpW << " " << cmpH << "\n255\n";
      std::vector<uint8_t> row(static_cast<size_t>(cmpW) * 3u);
      for (int y = 0; y < cmpH; ++y) {
        const uint8_t* p = buf + static_cast<size_t>(y) * stride;
        for (int x = 0; x < cmpW; ++x) {
          row[x * 3] = p[x * 4 + 2];
          row[x * 3 + 1] = p[x * 4 + 1];
          row[x * 3 + 2] = p[x * 4];
        }
        ppm.write(reinterpret_cast<const char*>(row.data()),
                  static_cast<std::streamsize>(row.size()));
      }
    };
    if (kDumpCompareScreens) {
      writePpm(gfxPath_ + ".eng.ppm", screen.data(), w * 4);
      writePpm(gfxPath_ + ".gdi.ppm", gdi->primary_buffer, static_cast<int>(gdi->stride));
    }
    // Per-tile diff. `tilesTouched` counts tiles with any difference, `tilesFull`
    // those where (almost) every pixel differs - the "missed/stale tile" shape.
    const int gw = (cmpW + 63) / 64;
    const int gh = (cmpH + 63) / 64;
    std::vector<int> tileDiff(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
    for (int row = 0; row < cmpH; ++row) {
      const uint8_t* a = screen.data() + static_cast<size_t>(row) * w * 4;
      const uint8_t* b = gdi->primary_buffer + static_cast<size_t>(row) * gdi->stride;
      for (int col = 0; col < cmpW; ++col) {
        const uint8_t* pa = a + col * 4;
        const uint8_t* pb = b + col * 4;
        if (pa[0] != pb[0] || pa[1] != pb[1] || pa[2] != pb[2]) {
          tileDiff[static_cast<size_t>(row / 64) * gw + col / 64]++;
        }
      }
    }
    int tilesTouched = 0;
    int tilesFull = 0;
    int tilesHalf = 0;
    for (const int n : tileDiff) {
      if (n == 0) {
        continue;
      }
      tilesTouched++;
      if (n >= 4000) {
        tilesFull++;
      } else if (n >= 2048) {
        tilesHalf++;
      }
    }
    char line[320];
    std::snprintf(line, sizeof(line),
                  "compare tiles: grid=%dx%d touched=%d full(>=4000px)=%d half(>=2048px)=%d "
                  "frame=%llu",
                  gw, gh, tilesTouched, tilesFull, tilesHalf,
                  static_cast<unsigned long long>(frames_.load()));
    HMRDP_LOGW("gfx replay: %{public}s", line);
  }
  // Dev diagnosis: dump every differing pixel so the values can be inspected.
  if (firstX >= 0 && cmpDumpDone_ == false) {
    cmpDumpDone_ = true;
    std::ofstream out(gfxPath_ + ".cmpdump", std::ios::out | std::ios::trunc);
    if (out) {
      out << "# frame=" << frames_.load() << " diff=" << diffRgb << " bbox=(" << bx0 << "," << by0
          << ")-(" << bx1 << "," << by1 << ")\n";
      for (int row = by0; row <= by1 && static_cast<int>(out.tellp()) < 200000; ++row) {
        const uint8_t* a2 = screen.data() + static_cast<size_t>(row) * w * 4;
        const uint8_t* b2 = gdi->primary_buffer + static_cast<size_t>(row) * gdi->stride;
        for (int col = bx0; col <= bx1; ++col) {
          const uint8_t* pa2 = a2 + col * 4;
          const uint8_t* pb2 = b2 + col * 4;
          if (pa2[0] == pb2[0] && pa2[1] == pb2[1] && pa2[2] == pb2[2]) {
            continue;
          }
          char line[128];
          std::snprintf(line, sizeof(line), "%d %d e=%02x%02x%02x g=%02x%02x%02x\n", col, row,
                        pa2[0], pa2[1], pa2[2], pb2[0], pb2[1], pb2[2]);
          out << line;
        }
      }
    }
    HMRDP_LOGW("gfx replay: compare dump written (frame=%{public}llu diff=%{public}llu)",
               static_cast<unsigned long long>(frames_.load()),
               static_cast<unsigned long long>(diffRgb));
  }
  if (firstX >= 0) {
    HMRDP_LOGW(
        "gfx replay: compare first diff (%{public}d,%{public}d) engine=0x%{public}x "
        "gdi=0x%{public}x",
        firstX, firstY, static_cast<unsigned>(firstEngine), static_cast<unsigned>(firstGdi));
  }
  if (bx1 >= 0) {
    cmpBBoxX0_.store(bx0);
    cmpBBoxY0_.store(by0);
    cmpBBoxX1_.store(bx1);
    cmpBBoxY1_.store(by1);
    cmpMaxDelta_.store(maxDelta);
    HMRDP_LOGW("gfx replay: compare bbox frame=%{public}llu diff=%{public}llu bbox=(%{public}d,%{public}d)-(%{public}d,%{public}d) maxDelta=%{public}d",
               static_cast<unsigned long long>(frames_.load()),
               static_cast<unsigned long long>(diffRgb), bx0, by0, bx1, by1, maxDelta);
  }
  cmpChecks_.fetch_add(1);
  cmpRgbDiff_.fetch_add(diffRgb);
  cmpAlphaDiff_.fetch_add(diffAlpha);
  if (diffRgb != 0 || diffAlpha != 0) {
    cmpBad_.fetch_add(1);
    const uint64_t prev = cmpMaxDiff_.load();
    if (static_cast<uint64_t>(diffRgb) > prev) {
      cmpMaxDiff_.store(static_cast<uint64_t>(diffRgb));
    }
    if (cmpFirstX_.load() < 0 && firstX >= 0) {
      cmpFirstX_.store(firstX);
      cmpFirstY_.store(firstY);
    }
    HMRDP_LOGW("gfx replay: compare diff rgb=%{public}llu alphaOnly=%{public}llu first=(%{public}d,%{public}d) frame=%{public}llu engWhite=%{public}llu gdiWhite=%{public}llu chanSwap=%{public}llu d255=%{public}llu maxDelta=%{public}d small=%{public}llu",
               static_cast<unsigned long long>(diffRgb),
               static_cast<unsigned long long>(diffAlpha), firstX, firstY,
               static_cast<unsigned long long>(frames_.load()),
               static_cast<unsigned long long>(engWhiteOnly),
               static_cast<unsigned long long>(gdiWhiteOnly),
               static_cast<unsigned long long>(chanSwap),
               static_cast<unsigned long long>(delta255), maxDelta,
               static_cast<unsigned long long>(smallDeltaPx));
  }
}

void GfxReplay::OnCpuFrame(GfxCpuDesktop* cpu) {
  if (cpu == nullptr) {
    return;
  }
  const int64_t nowUs = static_cast<int64_t>(NowUs());
  if (nowUs - startUs_.load() > kMaxRunUs) {
    HMRDP_LOGI("gfx replay: 120s cap reached");
    running_.store(false);
    return;
  }
  const int pw = pendingW_.exchange(0);
  const int ph = pendingH_.exchange(0);
  if (pw > 0 && ph > 0) {
    presenter_->ResizeSurface(pw, ph);
  }
  const int64_t presentStart = NowUs();
  const bool presented = PresentGdiFrame(cpu->gdi(), presenter_.get());
  RecordPresent(static_cast<uint64_t>(NowUs() - presentStart));
  frames_.fetch_add(1);
  if (presented) {
    presents_.fetch_add(1);
  } else {
    presentFailures_.fetch_add(1);
  }
  if ((frames_.load() % static_cast<uint64_t>(kLogEvery)) == 0) {
    HMRDP_LOGI("gfx replay: %{public}s", Stats().c_str());
  }
  PaceFrame(presented);
}

}  // namespace hmrdp
