/*
 * HmRdp - per-frame client work meter (see hmrdp_gfx_work.h).
 */
#include "hmrdp_gfx_work.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include <freerdp/channels/rdpgfx.h>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

std::atomic<GfxWorkMeter*> g_activeMeter{nullptr};

uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Callbacks this translation unit replaced, per GFX context, so they can be put
// back (a context may outlive the session/replay that installed them).
struct GfxOriginals {
  pcRdpgfxSurfaceCommand SurfaceCommand = nullptr;
  pcRdpgfxEndFrame EndFrame = nullptr;
  pcRdpgfxResetGraphics ResetGraphics = nullptr;
  pcRdpgfxCreateSurface CreateSurface = nullptr;
  pcRdpgfxDeleteSurface DeleteSurface = nullptr;
  pcRdpgfxMapSurfaceToOutput MapSurfaceToOutput = nullptr;
  pcRdpgfxMapSurfaceToScaledOutput MapSurfaceToScaledOutput = nullptr;
  pcRdpgfxSolidFill SolidFill = nullptr;
  pcRdpgfxSurfaceToSurface SurfaceToSurface = nullptr;
  pcRdpgfxSurfaceToCache SurfaceToCache = nullptr;
  pcRdpgfxCacheToSurface CacheToSurface = nullptr;
  pcRdpgfxEvictCacheEntry EvictCacheEntry = nullptr;
};

std::mutex g_installMutex;
std::unordered_map<RdpgfxClientContext*, GfxOriginals> g_originals;

// Returns the original callback stored for `gfx` (nullptr when not installed).
template <typename Fn>
Fn GfxOriginal(RdpgfxClientContext* gfx, Fn GfxOriginals::*member) {
  std::lock_guard<std::mutex> lock(g_installMutex);
  const auto it = g_originals.find(gfx);
  if (it == g_originals.end()) {
    return nullptr;
  }
  return it->second.*member;
}

UINT MeterSurfaceCommand(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_COMMAND* command) {
  const pcRdpgfxSurfaceCommand original = GfxOriginal(gfx, &GfxOriginals::SurfaceCommand);
  GfxWorkMeter* meter = ActiveWorkMeter();
  if (meter != nullptr) {
    // First command of the chunk this came from: the time since it arrived is
    // the ZGX decompress + PDU parse share of this frame.
    meter->AccountChunkPrefix();
  }
  const uint64_t start = NowUs();
  const UINT rc = original != nullptr ? original(gfx, command) : CHANNEL_RC_OK;
  if (meter != nullptr) {
    meter->OnDecode(NowUs() - start);
  }
  return rc;
}

UINT MeterEndFrame(RdpgfxClientContext* gfx, const RDPGFX_END_FRAME_PDU* endFrame) {
  const pcRdpgfxEndFrame original = GfxOriginal(gfx, &GfxOriginals::EndFrame);
  GfxWorkMeter* meter = ActiveWorkMeter();
  const uint64_t start = NowUs();
  // The original is gdi_EndFrame: it composites every mapped surface into the
  // primary buffer and, through the update pipeline, presents.
  const UINT rc = original != nullptr ? original(gfx, endFrame) : CHANNEL_RC_OK;
  if (meter != nullptr) {
    meter->OnFrameEnd(NowUs() - start);
  }
  return rc;
}

// The non-pixel commands: timed and counted per kind so the `zgx+parse` bucket
// (everything up to the first SurfaceCommand of a chunk) can be split into
// "decompress+parse" and "surface lifecycle" (a CreateSurface/ResetGraphics
// 0xFF-fills a whole surface, which is a per-frame-sized memset).
#define HMRDP_SETUP_WRAPPER(NAME, FIELD, KIND)                                          \
  UINT Meter##NAME(RdpgfxClientContext* gfx, const FIELD* pdu) {                        \
    const auto original = GfxOriginal(gfx, &GfxOriginals::NAME);                        \
    const uint64_t start = NowUs();                                                     \
    const UINT rc = original != nullptr ? original(gfx, pdu) : CHANNEL_RC_OK;           \
    const uint64_t dt = NowUs() - start;                                                \
    if (GfxWorkMeter* meter = ActiveWorkMeter()) {                                      \
      meter->OnSetup(KIND, dt);                                                         \
    }                                                                                   \
    /* A single structural command that takes tens of ms is a visible hitch (a        \
     * mode change: buffer + swapchain rebuild, whole-surface 0xFF fill), so it is     \
     * worth a log line of its own instead of hiding in an average. */                 \
    if (dt > 20000) {                                                                   \
      if (KIND == GfxSetupKind::kReset) {                                               \
        UINT16 count = 0;                                                               \
        UINT16* ids = nullptr;                                                          \
        if (gfx->GetSurfaceIds != nullptr) {                                            \
          gfx->GetSurfaceIds(gfx, &ids, &count);                                        \
        }                                                                               \
        HMRDP_LOGW("gfx setup: %{public}s took %{public}llu us (surfaces=%{public}u)",  \
                   #NAME, static_cast<unsigned long long>(dt),                          \
                   static_cast<unsigned>(count));                                       \
        free(ids);                                                                      \
      } else {                                                                          \
        HMRDP_LOGW("gfx setup: %{public}s took %{public}llu us", #NAME,                 \
                   static_cast<unsigned long long>(dt));                                \
      }                                                                                 \
    }                                                                                   \
    return rc;                                                                          \
  }

HMRDP_SETUP_WRAPPER(ResetGraphics, RDPGFX_RESET_GRAPHICS_PDU, GfxSetupKind::kReset)
HMRDP_SETUP_WRAPPER(CreateSurface, RDPGFX_CREATE_SURFACE_PDU, GfxSetupKind::kCreate)
HMRDP_SETUP_WRAPPER(DeleteSurface, RDPGFX_DELETE_SURFACE_PDU, GfxSetupKind::kDelete)
HMRDP_SETUP_WRAPPER(MapSurfaceToOutput, RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU, GfxSetupKind::kMap)
HMRDP_SETUP_WRAPPER(MapSurfaceToScaledOutput, RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU,
                    GfxSetupKind::kMap)
HMRDP_SETUP_WRAPPER(SolidFill, RDPGFX_SOLID_FILL_PDU, GfxSetupKind::kFill)
HMRDP_SETUP_WRAPPER(SurfaceToSurface, RDPGFX_SURFACE_TO_SURFACE_PDU, GfxSetupKind::kBlit)
HMRDP_SETUP_WRAPPER(SurfaceToCache, RDPGFX_SURFACE_TO_CACHE_PDU, GfxSetupKind::kCache)
HMRDP_SETUP_WRAPPER(CacheToSurface, RDPGFX_CACHE_TO_SURFACE_PDU, GfxSetupKind::kCache)
HMRDP_SETUP_WRAPPER(EvictCacheEntry, RDPGFX_EVICT_CACHE_ENTRY_PDU, GfxSetupKind::kCache)

}  // namespace

void GfxWorkMeter::OnChunk(uint32_t bytes) {
  // A chunk is parsed (and its commands decoded) to completion before the next
  // one arrives, so a single pending stamp is enough.
  chunkPending_ = true;
  chunkArrivalUs_ = NowUs();
  frameBytes_ += bytes;
}

void GfxWorkMeter::AccountChunkPrefix() {
  if (!chunkPending_) {
    return;
  }
  chunkPending_ = false;
  frameZgxUs_ += NowUs() - chunkArrivalUs_;
}

void GfxWorkMeter::OnDecode(uint64_t micros) {
  frameDecodeUs_ += micros;
  frameCommands_ += 1;
}

void GfxWorkMeter::OnSetup(GfxSetupKind kind, uint64_t micros) {
  const int k = static_cast<int>(kind);
  if (k < 0 || k >= static_cast<int>(GfxSetupKind::kCount)) {
    return;
  }
  windowSetupUs_[k].fetch_add(micros);
  windowSetupCount_[k].fetch_add(1);
}

void GfxWorkMeter::OnPresent(uint64_t micros) {
  framePresentUs_ += micros;
}

void GfxWorkMeter::OnPace(uint64_t micros) {
  framePaceUs_ += micros;
}

void GfxWorkMeter::OnFrameEnd(uint64_t endFrameMicros) {
  // A frame that carried no surface command still paid for its chunk.
  AccountChunkPrefix();
  const uint64_t present = framePresentUs_;
  const uint64_t notWork = present + framePaceUs_;
  // The playback throttling is not client work: the CPU replay paces between
  // presented frames, and because the presenter runs inside gdi's EndFrame that
  // sleep would otherwise land in `compose` (measured at ~8 ms/frame on the
  // scrolling sample, i.e. most of a compose that really costs ~0.4 ms).
  const uint64_t compose = endFrameMicros > notWork ? endFrameMicros - notWork : 0;
  const uint64_t frameWork = frameZgxUs_ + frameDecodeUs_ + compose + present;
  windowFrames_.fetch_add(1);
  windowZgxUs_.fetch_add(frameZgxUs_);
  windowDecodeUs_.fetch_add(frameDecodeUs_);
  windowComposeUs_.fetch_add(compose);
  windowPresentUs_.fetch_add(present);
  windowBytes_.fetch_add(frameBytes_);
  windowCommands_.fetch_add(frameCommands_);
  uint64_t worst = windowMaxFrameUs_.load();
  while (frameWork > worst && !windowMaxFrameUs_.compare_exchange_weak(worst, frameWork)) {
  }
  frameZgxUs_ = 0;
  frameDecodeUs_ = 0;
  framePresentUs_ = 0;
  framePaceUs_ = 0;
  frameBytes_ = 0;
  frameCommands_ = 0;
}

void GfxWorkMeter::Reset() {
  frameZgxUs_ = 0;
  frameDecodeUs_ = 0;
  framePresentUs_ = 0;
  framePaceUs_ = 0;
  frameBytes_ = 0;
  frameCommands_ = 0;
  chunkPending_ = false;
  chunkArrivalUs_ = 0;
  windowFrames_ = 0;
  windowZgxUs_ = 0;
  windowDecodeUs_ = 0;
  windowComposeUs_ = 0;
  windowPresentUs_ = 0;
  windowBytes_ = 0;
  windowCommands_ = 0;
  windowMaxFrameUs_ = 0;
  for (int i = 0; i < static_cast<int>(GfxSetupKind::kCount); ++i) {
    windowSetupUs_[i] = 0;
    windowSetupCount_[i] = 0;
  }
}

GfxWorkMeter::Sample GfxWorkMeter::Drain() {
  Sample sample;
  sample.frames = windowFrames_.exchange(0);
  sample.zgxParseUs = windowZgxUs_.exchange(0);
  sample.decodeUs = windowDecodeUs_.exchange(0);
  sample.composeUs = windowComposeUs_.exchange(0);
  sample.presentUs = windowPresentUs_.exchange(0);
  sample.bytes = windowBytes_.exchange(0);
  sample.commands = windowCommands_.exchange(0);
  sample.maxFrameUs = windowMaxFrameUs_.exchange(0);
  for (int i = 0; i < static_cast<int>(GfxSetupKind::kCount); ++i) {
    sample.setupUs[i] = windowSetupUs_[i].exchange(0);
    sample.setupCount[i] = windowSetupCount_[i].exchange(0);
  }
  return sample;
}

GfxWorkMeter::Sample GfxWorkMeter::Peek() const {
  Sample sample;
  sample.frames = windowFrames_.load();
  sample.zgxParseUs = windowZgxUs_.load();
  sample.decodeUs = windowDecodeUs_.load();
  sample.composeUs = windowComposeUs_.load();
  sample.presentUs = windowPresentUs_.load();
  sample.bytes = windowBytes_.load();
  sample.commands = windowCommands_.load();
  sample.maxFrameUs = windowMaxFrameUs_.load();
  for (int i = 0; i < static_cast<int>(GfxSetupKind::kCount); ++i) {
    sample.setupUs[i] = windowSetupUs_[i].load();
    sample.setupCount[i] = windowSetupCount_[i].load();
  }
  return sample;
}

GfxWorkMeter* ActiveWorkMeter() {
  return g_activeMeter.load();
}

void SetActiveWorkMeter(GfxWorkMeter* meter) {
  g_activeMeter.store(meter);
}

void GfxWorkInstall(RdpgfxClientContext* gfx) {
  if (gfx == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_installMutex);
  if (g_originals.find(gfx) != g_originals.end()) {
    return;
  }
  GfxOriginals& orig = g_originals[gfx];
  orig.SurfaceCommand = gfx->SurfaceCommand;
  if (gfx->SurfaceCommand != nullptr) {
    gfx->SurfaceCommand = MeterSurfaceCommand;
  }
  orig.EndFrame = gfx->EndFrame;
  if (gfx->EndFrame != nullptr) {
    gfx->EndFrame = MeterEndFrame;
  }
  // The non-pixel commands, so their time stops hiding in `zgx+parse`.
  orig.ResetGraphics = gfx->ResetGraphics;
  if (gfx->ResetGraphics != nullptr) {
    gfx->ResetGraphics = MeterResetGraphics;
  }
  orig.CreateSurface = gfx->CreateSurface;
  if (gfx->CreateSurface != nullptr) {
    gfx->CreateSurface = MeterCreateSurface;
  }
  orig.DeleteSurface = gfx->DeleteSurface;
  if (gfx->DeleteSurface != nullptr) {
    gfx->DeleteSurface = MeterDeleteSurface;
  }
  orig.MapSurfaceToOutput = gfx->MapSurfaceToOutput;
  if (gfx->MapSurfaceToOutput != nullptr) {
    gfx->MapSurfaceToOutput = MeterMapSurfaceToOutput;
  }
  orig.MapSurfaceToScaledOutput = gfx->MapSurfaceToScaledOutput;
  if (gfx->MapSurfaceToScaledOutput != nullptr) {
    gfx->MapSurfaceToScaledOutput = MeterMapSurfaceToScaledOutput;
  }
  orig.SolidFill = gfx->SolidFill;
  if (gfx->SolidFill != nullptr) {
    gfx->SolidFill = MeterSolidFill;
  }
  orig.SurfaceToSurface = gfx->SurfaceToSurface;
  if (gfx->SurfaceToSurface != nullptr) {
    gfx->SurfaceToSurface = MeterSurfaceToSurface;
  }
  orig.SurfaceToCache = gfx->SurfaceToCache;
  if (gfx->SurfaceToCache != nullptr) {
    gfx->SurfaceToCache = MeterSurfaceToCache;
  }
  orig.CacheToSurface = gfx->CacheToSurface;
  if (gfx->CacheToSurface != nullptr) {
    gfx->CacheToSurface = MeterCacheToSurface;
  }
  orig.EvictCacheEntry = gfx->EvictCacheEntry;
  if (gfx->EvictCacheEntry != nullptr) {
    gfx->EvictCacheEntry = MeterEvictCacheEntry;
  }
}

void GfxWorkUninstall(RdpgfxClientContext* gfx) {
  if (gfx == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_installMutex);
  const auto it = g_originals.find(gfx);
  if (it == g_originals.end()) {
    return;
  }
  // Restore what this TU replaced: leaving the wrappers installed would make the
  // context call into a meter that is going away.
  if (gfx->SurfaceCommand == &MeterSurfaceCommand) {
    gfx->SurfaceCommand = it->second.SurfaceCommand;
  }
  if (gfx->EndFrame == &MeterEndFrame) {
    gfx->EndFrame = it->second.EndFrame;
  }
  if (gfx->ResetGraphics == &MeterResetGraphics) {
    gfx->ResetGraphics = it->second.ResetGraphics;
  }
  if (gfx->CreateSurface == &MeterCreateSurface) {
    gfx->CreateSurface = it->second.CreateSurface;
  }
  if (gfx->DeleteSurface == &MeterDeleteSurface) {
    gfx->DeleteSurface = it->second.DeleteSurface;
  }
  if (gfx->MapSurfaceToOutput == &MeterMapSurfaceToOutput) {
    gfx->MapSurfaceToOutput = it->second.MapSurfaceToOutput;
  }
  if (gfx->MapSurfaceToScaledOutput == &MeterMapSurfaceToScaledOutput) {
    gfx->MapSurfaceToScaledOutput = it->second.MapSurfaceToScaledOutput;
  }
  if (gfx->SolidFill == &MeterSolidFill) {
    gfx->SolidFill = it->second.SolidFill;
  }
  if (gfx->SurfaceToSurface == &MeterSurfaceToSurface) {
    gfx->SurfaceToSurface = it->second.SurfaceToSurface;
  }
  if (gfx->SurfaceToCache == &MeterSurfaceToCache) {
    gfx->SurfaceToCache = it->second.SurfaceToCache;
  }
  if (gfx->CacheToSurface == &MeterCacheToSurface) {
    gfx->CacheToSurface = it->second.CacheToSurface;
  }
  if (gfx->EvictCacheEntry == &MeterEvictCacheEntry) {
    gfx->EvictCacheEntry = it->second.EvictCacheEntry;
  }
  g_originals.erase(it);
}

}  // namespace hmrdp
