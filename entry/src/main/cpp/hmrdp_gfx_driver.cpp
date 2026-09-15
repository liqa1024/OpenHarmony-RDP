/*
 * HmRdp - shared RDPGFX driver (see hmrdp_gfx_driver.h).
 */
#include "hmrdp_gfx_driver.h"

#include <map>

#include <vector>

#include "hmrdp_gfx_capture.h"
#include "hmrdp_log.h"
#include "hmrdp_rfx.h"  // GpuCmd ids

namespace hmrdp {

namespace {

void PutU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
  PutU32(out, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  PutU32(out, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

void PutRect(std::vector<uint8_t>& out, const RECTANGLE_16& r) {
  PutU16(out, r.left);
  PutU16(out, r.top);
  PutU16(out, r.right);
  PutU16(out, r.bottom);
}

}  // namespace

// ---------------------------------------------------------------------------
// RDPGFX PDU -> engine command
// ---------------------------------------------------------------------------

void GfxMapSurfaceCommand(GfxCommandSink* sink, const RDPGFX_SURFACE_COMMAND* command) {
  if (sink == nullptr || command == nullptr) {
    return;
  }
  // codecId + geometry in the params blob; the payload (still compressed) as-is.
  uint32_t sc[4] = {command->codecId, 0, 0, 0};
  std::vector<uint8_t> params;
  PutU32(params, command->contextId);
  PutU32(params, command->format);
  PutU32(params, command->left);
  PutU32(params, command->top);
  PutU32(params, command->right);
  PutU32(params, command->bottom);
  PutU32(params, command->width);
  PutU32(params, command->height);
  sink->ApplyGfx(kGpuCmdWireToSurface, command->surfaceId, sc, params.data(),
                 static_cast<uint32_t>(params.size()), command->data, command->length);
}

void GfxMapStartFrame(GfxCommandSink* sink, uint32_t frameId) {
  if (sink == nullptr) {
    return;
  }
  // Frame boundary. FreeRDP's progressive decoder re-composites every tile it
  // has decoded in the current frame on each Progressive message, and resets that
  // list only when the *frame id changes* (PROGRESSIVE_SURFACE_CONTEXT::frameId is
  // compared in progressive_decompress - see gdi_StartFrame). A frame id can
  // repeat (and does in this capture), so the engine must apply the same rule
  // instead of clearing on every StartFrame.
  const uint32_t sc[4] = {frameId, 0, 0, 0};
  sink->ApplyGfx(kGpuCmdStartFrame, 0xFFFFFFFFu, sc, nullptr, 0, nullptr, 0);
}

void GfxMapResetGraphics(GfxCommandSink* sink, const RDPGFX_RESET_GRAPHICS_PDU* pdu) {  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->width, pdu->height, pdu->monitorCount, 0};
  sink->ApplyGfx(kGpuCmdResetGraphics, 0xFFFFFFFFu, sc, nullptr, 0, nullptr, 0);
}

void GfxMapCreateSurface(GfxCommandSink* sink, const RDPGFX_CREATE_SURFACE_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->width, pdu->height, pdu->pixelFormat, 0};
  sink->ApplyGfx(kGpuCmdCreateSurface, pdu->surfaceId, sc, nullptr, 0, nullptr, 0);
}

void GfxMapDeleteSurface(GfxCommandSink* sink, const RDPGFX_DELETE_SURFACE_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  sink->ApplyGfx(kGpuCmdDeleteSurface, pdu->surfaceId, nullptr, nullptr, 0, nullptr, 0);
}

void GfxMapSolidFill(GfxCommandSink* sink, const RDPGFX_SOLID_FILL_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t pixel = static_cast<uint32_t>(pdu->fillPixel.B) |
                         (static_cast<uint32_t>(pdu->fillPixel.G) << 8) |
                         (static_cast<uint32_t>(pdu->fillPixel.R) << 16) |
                         (static_cast<uint32_t>(pdu->fillPixel.XA) << 24);
  const uint32_t sc[4] = {pixel, pdu->fillRectCount, 0, 0};
  std::vector<uint8_t> params;
  for (uint16_t i = 0; i < pdu->fillRectCount; ++i) {
    PutRect(params, pdu->fillRects[i]);
  }
  sink->ApplyGfx(kGpuCmdSolidFill, pdu->surfaceId, sc, params.data(),
                 static_cast<uint32_t>(params.size()), nullptr, 0);
}

void GfxMapSurfaceToSurface(GfxCommandSink* sink, const RDPGFX_SURFACE_TO_SURFACE_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->surfaceIdSrc, pdu->destPtsCount, 0, 0};
  std::vector<uint8_t> params;
  PutRect(params, pdu->rectSrc);
  for (uint16_t i = 0; i < pdu->destPtsCount; ++i) {
    PutU16(params, pdu->destPts[i].x);
    PutU16(params, pdu->destPts[i].y);
  }
  sink->ApplyGfx(kGpuCmdSurfaceToSurface, pdu->surfaceIdDest, sc, params.data(),
                 static_cast<uint32_t>(params.size()), nullptr, 0);
}

void GfxMapSurfaceToCache(GfxCommandSink* sink, const RDPGFX_SURFACE_TO_CACHE_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->cacheSlot, 0, 0, 0};
  std::vector<uint8_t> params;
  PutU64(params, pdu->cacheKey);
  PutRect(params, pdu->rectSrc);
  sink->ApplyGfx(kGpuCmdSurfaceToCache, pdu->surfaceId, sc, params.data(),
                 static_cast<uint32_t>(params.size()), nullptr, 0);
}

void GfxMapCacheToSurface(GfxCommandSink* sink, const RDPGFX_CACHE_TO_SURFACE_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->cacheSlot, pdu->destPtsCount, 0, 0};
  std::vector<uint8_t> params;
  for (uint16_t i = 0; i < pdu->destPtsCount; ++i) {
    PutU16(params, pdu->destPts[i].x);
    PutU16(params, pdu->destPts[i].y);
  }
  sink->ApplyGfx(kGpuCmdCacheToSurface, pdu->surfaceId, sc, params.data(),
                 static_cast<uint32_t>(params.size()), nullptr, 0);
}

void GfxMapEvictCacheEntry(GfxCommandSink* sink, const RDPGFX_EVICT_CACHE_ENTRY_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->cacheSlot, 0, 0, 0};
  sink->ApplyGfx(kGpuCmdEvictCacheEntry, 0xFFFFFFFFu, sc, nullptr, 0, nullptr, 0);
}

void GfxMapSurfaceToOutput(GfxCommandSink* sink, const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->outputOriginX, pdu->outputOriginY, 0, 0};
  sink->ApplyGfx(kGpuCmdMapSurfaceToOutput, pdu->surfaceId, sc, nullptr, 0, nullptr, 0);
}

void GfxMapSurfaceToScaledOutput(GfxCommandSink* sink,
                                 const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU* pdu) {
  if (sink == nullptr || pdu == nullptr) {
    return;
  }
  const uint32_t sc[4] = {pdu->outputOriginX, pdu->outputOriginY, pdu->targetWidth,
                          pdu->targetHeight};
  sink->ApplyGfx(kGpuCmdMapSurfaceToScaledOutput, pdu->surfaceId, sc, nullptr, 0, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Raw capture replay pump
// ---------------------------------------------------------------------------

namespace {

struct PumpState {
  GfxCommandSink* sink = nullptr;
  const std::function<void()>* onFrame = nullptr;
  // Compare route only: set when the current chunk contained an EndFrame, so the
  // caller knows both decoders are at a comparable (composed) state.
  bool* sawFrame = nullptr;
};

PumpState* PumpOf(RdpgfxClientContext* gfx) {
  return gfx == nullptr ? nullptr : static_cast<PumpState*>(gfx->custom);
}

UINT PmpSurfaceCommand(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_COMMAND* pdu) {
  GfxMapSurfaceCommand(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpResetGraphics(RdpgfxClientContext* gfx, const RDPGFX_RESET_GRAPHICS_PDU* pdu) {
  GfxMapResetGraphics(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpCreateSurface(RdpgfxClientContext* gfx, const RDPGFX_CREATE_SURFACE_PDU* pdu) {
  GfxMapCreateSurface(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpDeleteSurface(RdpgfxClientContext* gfx, const RDPGFX_DELETE_SURFACE_PDU* pdu) {
  GfxMapDeleteSurface(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpSolidFill(RdpgfxClientContext* gfx, const RDPGFX_SOLID_FILL_PDU* pdu) {
  GfxMapSolidFill(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpSurfaceToSurface(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_TO_SURFACE_PDU* pdu) {
  GfxMapSurfaceToSurface(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpSurfaceToCache(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_TO_CACHE_PDU* pdu) {
  GfxMapSurfaceToCache(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpCacheToSurface(RdpgfxClientContext* gfx, const RDPGFX_CACHE_TO_SURFACE_PDU* pdu) {
  GfxMapCacheToSurface(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpEvictCacheEntry(RdpgfxClientContext* gfx, const RDPGFX_EVICT_CACHE_ENTRY_PDU* pdu) {
  GfxMapEvictCacheEntry(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpMapSurfaceToOutput(RdpgfxClientContext* gfx,
                           const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* pdu) {
  GfxMapSurfaceToOutput(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpMapSurfaceToScaledOutput(RdpgfxClientContext* gfx,
                                 const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU* pdu) {
  GfxMapSurfaceToScaledOutput(PumpOf(gfx) ? PumpOf(gfx)->sink : nullptr, pdu);
  return CHANNEL_RC_OK;
}

UINT PmpStartFrame(RdpgfxClientContext* gfx, const RDPGFX_START_FRAME_PDU* pdu) {
  PumpState* state = PumpOf(gfx);
  if (state == nullptr) {
    return CHANNEL_RC_OK;
  }
  GfxMapStartFrame(state->sink, pdu != nullptr ? pdu->frameId : 0u);
  return CHANNEL_RC_OK;
}

UINT PmpEndFrame(RdpgfxClientContext* gfx, const RDPGFX_END_FRAME_PDU* pdu) {
  (void)pdu;
  PumpState* state = PumpOf(gfx);
  if (state == nullptr) {
    return CHANNEL_RC_OK;
  }
  if (state->sawFrame != nullptr) {
    *state->sawFrame = true;
  }
  if (state->onFrame != nullptr && *state->onFrame) {
    (*state->onFrame)();
  }
  return CHANNEL_RC_OK;
}

// Client -> server callbacks are irrelevant offline (no channel to write to);
// no-op them so FreeRDP never fails on a session-less context.
UINT PmpNoopCapsAdvertise(RdpgfxClientContext* gfx, const RDPGFX_CAPS_ADVERTISE_PDU* pdu) {
  (void)gfx;
  (void)pdu;
  return CHANNEL_RC_OK;
}

UINT PmpNoopFrameAcknowledge(RdpgfxClientContext* gfx,
                             const RDPGFX_FRAME_ACKNOWLEDGE_PDU* pdu) {
  (void)gfx;
  (void)pdu;
  return CHANNEL_RC_OK;
}

UINT PmpNoopCacheImportOffer(RdpgfxClientContext* gfx,
                             const RDPGFX_CACHE_IMPORT_OFFER_PDU* pdu) {
  (void)gfx;
  (void)pdu;
  return CHANNEL_RC_OK;
}

UINT PmpNoopQoeFrameAcknowledge(RdpgfxClientContext* gfx,
                                const RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU* pdu) {
  (void)gfx;
  (void)pdu;
  return CHANNEL_RC_OK;
}

void InstallPumpCallbacks(RdpgfxClientContext* gfx) {
  gfx->SurfaceCommand = PmpSurfaceCommand;
  gfx->ResetGraphics = PmpResetGraphics;
  gfx->CreateSurface = PmpCreateSurface;
  gfx->DeleteSurface = PmpDeleteSurface;
  gfx->SolidFill = PmpSolidFill;
  gfx->SurfaceToSurface = PmpSurfaceToSurface;
  gfx->SurfaceToCache = PmpSurfaceToCache;
  gfx->CacheToSurface = PmpCacheToSurface;
  gfx->EvictCacheEntry = PmpEvictCacheEntry;
  gfx->MapSurfaceToOutput = PmpMapSurfaceToOutput;
  gfx->MapSurfaceToScaledOutput = PmpMapSurfaceToScaledOutput;
  gfx->StartFrame = PmpStartFrame;
  gfx->EndFrame = PmpEndFrame;
  gfx->CapsAdvertise = PmpNoopCapsAdvertise;
  gfx->FrameAcknowledge = PmpNoopFrameAcknowledge;
  gfx->CacheImportOffer = PmpNoopCacheImportOffer;
  gfx->QoeFrameAcknowledge = PmpNoopQoeFrameAcknowledge;
}

}  // namespace

bool GfxReplayPump(const std::string& path, RdpgfxClientContext* gfx,
                   const std::atomic<bool>* stop, std::string* error) {
  auto fail = [error](const char* why) {
    if (error != nullptr) {
      *error = why;
    }
  };
  if (HmrdpGfxReplayRecv == nullptr) {
    fail("FreeRDP was not built with the HmRdp GFX capture patch");
    return false;
  }
  if (gfx == nullptr) {
    fail("no RDPGFX replay context");
    return false;
  }
  GfxRawCapture capture;
  if (!capture.Open(path)) {
    fail("cannot open capture");
    return false;
  }
  const uint8_t* data = nullptr;
  uint32_t size = 0;
  while ((stop == nullptr || stop->load()) && capture.Next(&data, &size)) {
    HmrdpGfxReplayRecv(gfx, data, size);
  }
  return true;
}

bool GfxReplayStream(const std::string& path, GfxCommandSink* sink,
                     const std::function<void()>& onFrame, const std::atomic<bool>* stop,
                     std::string* error) {
  auto fail = [error](const char* why) {
    if (error != nullptr) {
      *error = why;
    }
  };
  if (HmrdpGfxReplayNew == nullptr || HmrdpGfxReplayFree == nullptr) {
    fail("FreeRDP was not built with the HmRdp GFX capture patch");
    return false;
  }
  RdpgfxClientContext* gfx = HmrdpGfxReplayNew();
  if (gfx == nullptr) {
    fail("cannot create replay context");
    return false;
  }
  PumpState state;
  state.sink = sink;
  state.onFrame = &onFrame;
  gfx->custom = &state;
  InstallPumpCallbacks(gfx);

  const bool ok = GfxReplayPump(path, gfx, stop, error);
  HmrdpGfxReplayFree(gfx);
  return ok;
}

// ---------------------------------------------------------------------------
// Compare route: gdi/engine interleaved per PDU.
//
// The A/B is only meaningful when both sides have applied exactly the same set of
// commands. Feeding two contexts chunk by chunk (the old arrangement) left gdi a
// whole chunk behind, so a comparison could only happen at chunk boundaries and
// could not name the command that diverged. Instead the harness wraps the
// callbacks gdi already installed on its own context: each PDU goes to gdi first
// and is handed to the engine/mirror sink immediately afterwards, so `onCommand`
// runs with both sides at the same stream position.
// ---------------------------------------------------------------------------
namespace {

struct CompareChain {
  GfxCommandSink* sink = nullptr;
  std::function<void()> onCommand;  // after every single PDU
  std::function<void()> onFrame;    // EndFrame: present / frame accounting
  std::function<void()> onSync;     // EndFrame: gdi vs engine frame comparison
  // gdi's original callbacks.
  pcRdpgfxStartFrame startFrame = nullptr;
  pcRdpgfxEndFrame endFrame = nullptr;
  pcRdpgfxResetGraphics resetGraphics = nullptr;
  pcRdpgfxCreateSurface createSurface = nullptr;
  pcRdpgfxDeleteSurface deleteSurface = nullptr;
  pcRdpgfxSurfaceCommand surfaceCommand = nullptr;
  pcRdpgfxSolidFill solidFill = nullptr;
  pcRdpgfxSurfaceToSurface surfaceToSurface = nullptr;
  pcRdpgfxSurfaceToCache surfaceToCache = nullptr;
  pcRdpgfxCacheToSurface cacheToSurface = nullptr;
  pcRdpgfxEvictCacheEntry evictCacheEntry = nullptr;
  pcRdpgfxMapSurfaceToOutput mapSurfaceToOutput = nullptr;
  pcRdpgfxMapSurfaceToScaledOutput mapSurfaceToScaledOutput = nullptr;
};

std::map<RdpgfxClientContext*, CompareChain> g_compareChains;

CompareChain* ChainOf(RdpgfxClientContext* gfx) {
  const auto it = g_compareChains.find(gfx);
  return it == g_compareChains.end() ? nullptr : &it->second;
}

void ChainDone(CompareChain* chain) {
  if (chain != nullptr && chain->onCommand) {
    chain->onCommand();
  }
}

UINT ChainStartFrame(RdpgfxClientContext* gfx, const RDPGFX_START_FRAME_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->startFrame != nullptr ? chain->startFrame(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapStartFrame(chain->sink, pdu != nullptr ? pdu->frameId : 0u);
  ChainDone(chain);
  return rc;
}

UINT ChainEndFrame(RdpgfxClientContext* gfx, const RDPGFX_END_FRAME_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->endFrame != nullptr ? chain->endFrame(gfx, pdu) : CHANNEL_RC_OK;
  ChainDone(chain);
  if (chain->onFrame) {
    chain->onFrame();
  }
  if (chain->onSync) {
    chain->onSync();
  }
  return rc;
}

UINT ChainResetGraphics(RdpgfxClientContext* gfx, const RDPGFX_RESET_GRAPHICS_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->resetGraphics != nullptr ? chain->resetGraphics(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapResetGraphics(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainCreateSurface(RdpgfxClientContext* gfx, const RDPGFX_CREATE_SURFACE_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->createSurface != nullptr ? chain->createSurface(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapCreateSurface(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainDeleteSurface(RdpgfxClientContext* gfx, const RDPGFX_DELETE_SURFACE_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->deleteSurface != nullptr ? chain->deleteSurface(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapDeleteSurface(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainSurfaceCommand(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_COMMAND* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->surfaceCommand != nullptr ? chain->surfaceCommand(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapSurfaceCommand(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainSolidFill(RdpgfxClientContext* gfx, const RDPGFX_SOLID_FILL_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->solidFill != nullptr ? chain->solidFill(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapSolidFill(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainSurfaceToSurface(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_TO_SURFACE_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc =
      chain->surfaceToSurface != nullptr ? chain->surfaceToSurface(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapSurfaceToSurface(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainSurfaceToCache(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_TO_CACHE_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc =
      chain->surfaceToCache != nullptr ? chain->surfaceToCache(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapSurfaceToCache(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainCacheToSurface(RdpgfxClientContext* gfx, const RDPGFX_CACHE_TO_SURFACE_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc =
      chain->cacheToSurface != nullptr ? chain->cacheToSurface(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapCacheToSurface(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainEvictCacheEntry(RdpgfxClientContext* gfx, const RDPGFX_EVICT_CACHE_ENTRY_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc =
      chain->evictCacheEntry != nullptr ? chain->evictCacheEntry(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapEvictCacheEntry(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainMapSurfaceToOutput(RdpgfxClientContext* gfx,
                             const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc =
      chain->mapSurfaceToOutput != nullptr ? chain->mapSurfaceToOutput(gfx, pdu) : CHANNEL_RC_OK;
  GfxMapSurfaceToOutput(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

UINT ChainMapSurfaceToScaledOutput(RdpgfxClientContext* gfx,
                                   const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU* pdu) {
  CompareChain* chain = ChainOf(gfx);
  if (chain == nullptr) {
    return CHANNEL_RC_OK;
  }
  const UINT rc = chain->mapSurfaceToScaledOutput != nullptr
                      ? chain->mapSurfaceToScaledOutput(gfx, pdu)
                      : CHANNEL_RC_OK;
  GfxMapSurfaceToScaledOutput(chain->sink, pdu);
  ChainDone(chain);
  return rc;
}

// Wraps the callbacks already installed on `gfx` (gdi's) with the sink chain.
void InstallCompareChain(RdpgfxClientContext* gfx, GfxCommandSink* sink,
                         std::function<void()> onCommand, std::function<void()> onFrame,
                         std::function<void()> onSync) {
  CompareChain chain;
  chain.sink = sink;
  chain.onCommand = std::move(onCommand);
  chain.onFrame = std::move(onFrame);
  chain.onSync = std::move(onSync);
  chain.startFrame = gfx->StartFrame;
  chain.endFrame = gfx->EndFrame;
  chain.resetGraphics = gfx->ResetGraphics;
  chain.createSurface = gfx->CreateSurface;
  chain.deleteSurface = gfx->DeleteSurface;
  chain.surfaceCommand = gfx->SurfaceCommand;
  chain.solidFill = gfx->SolidFill;
  chain.surfaceToSurface = gfx->SurfaceToSurface;
  chain.surfaceToCache = gfx->SurfaceToCache;
  chain.cacheToSurface = gfx->CacheToSurface;
  chain.evictCacheEntry = gfx->EvictCacheEntry;
  chain.mapSurfaceToOutput = gfx->MapSurfaceToOutput;
  chain.mapSurfaceToScaledOutput = gfx->MapSurfaceToScaledOutput;
  g_compareChains[gfx] = std::move(chain);
  gfx->StartFrame = ChainStartFrame;
  gfx->EndFrame = ChainEndFrame;
  gfx->ResetGraphics = ChainResetGraphics;
  gfx->CreateSurface = ChainCreateSurface;
  gfx->DeleteSurface = ChainDeleteSurface;
  gfx->SurfaceCommand = ChainSurfaceCommand;
  gfx->SolidFill = ChainSolidFill;
  gfx->SurfaceToSurface = ChainSurfaceToSurface;
  gfx->SurfaceToCache = ChainSurfaceToCache;
  gfx->CacheToSurface = ChainCacheToSurface;
  gfx->EvictCacheEntry = ChainEvictCacheEntry;
  gfx->MapSurfaceToOutput = ChainMapSurfaceToOutput;
  gfx->MapSurfaceToScaledOutput = ChainMapSurfaceToScaledOutput;
}

void RemoveCompareChain(RdpgfxClientContext* gfx) {
  const auto it = g_compareChains.find(gfx);
  if (it == g_compareChains.end()) {
    return;
  }
  gfx->StartFrame = it->second.startFrame;
  gfx->EndFrame = it->second.endFrame;
  gfx->ResetGraphics = it->second.resetGraphics;
  gfx->CreateSurface = it->second.createSurface;
  gfx->DeleteSurface = it->second.deleteSurface;
  gfx->SurfaceCommand = it->second.surfaceCommand;
  gfx->SolidFill = it->second.solidFill;
  gfx->SurfaceToSurface = it->second.surfaceToSurface;
  gfx->SurfaceToCache = it->second.surfaceToCache;
  gfx->CacheToSurface = it->second.cacheToSurface;
  gfx->EvictCacheEntry = it->second.evictCacheEntry;
  gfx->MapSurfaceToOutput = it->second.mapSurfaceToOutput;
  gfx->MapSurfaceToScaledOutput = it->second.mapSurfaceToScaledOutput;
  g_compareChains.erase(it);
}

}  // namespace

bool GfxReplayStreamCompare(const std::string& path, GfxCommandSink* sink,
                            const std::function<void()>& onFrame,
                            const std::function<void()>& onSync,
                            const std::function<void()>& onCommand, RdpgfxClientContext* gfxB,
                            const std::atomic<bool>* stop, std::string* error) {
  auto fail = [error](const char* why) {
    if (error != nullptr) {
      *error = why;
    }
  };
  if (HmrdpGfxReplayRecv == nullptr) {
    fail("FreeRDP was not built with the HmRdp GFX capture patch");
    return false;
  }
  if (gfxB == nullptr) {
    fail("no gdi replay context");
    return false;
  }
  InstallCompareChain(gfxB, sink, onCommand ? onCommand : std::function<void()>(), onFrame, onSync);

  GfxRawCapture capture;
  if (!capture.Open(path)) {
    RemoveCompareChain(gfxB);
    fail("cannot open capture");
    return false;
  }
  const uint8_t* data = nullptr;
  uint32_t size = 0;
  while ((stop == nullptr || stop->load()) && capture.Next(&data, &size)) {
    // gdi (and, through the chain, the engine/mirror) consume this chunk's PDUs.
    HmrdpGfxReplayRecv(gfxB, data, size);
  }
  RemoveCompareChain(gfxB);
  return true;
}

}  // namespace hmrdp
