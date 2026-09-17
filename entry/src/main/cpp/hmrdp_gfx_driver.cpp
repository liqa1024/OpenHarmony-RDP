/*
 * HmRdp - shared RDPGFX driver (see hmrdp_gfx_driver.h).
 */
#include "hmrdp_gfx_driver.h"

#include <atomic>
#include <chrono>
#include <map>

#include <vector>

#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_work.h"
#include "hmrdp_log.h"
#include "hmrdp_rfx.h"  // GpuCmd ids

namespace hmrdp {

namespace {
std::atomic<uint64_t> g_parseUs{0};
int64_t ParseNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void GfxReplayResetParseUs() { g_parseUs.store(0); }
void GfxReplayAddParseUs(uint64_t micros) { g_parseUs.fetch_add(micros); }
uint64_t GfxReplayParseUs() { return g_parseUs.load(); }

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
                   const std::atomic<bool>* stop, std::string* error,
                   const ReplayPaceFn& pace, const ReplayPaceAccumFn& paceAccum,
                   bool* aborted) {
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
  const bool paced = pace && capture.hasTimestamps();
  const uint8_t* data = nullptr;
  uint32_t size = 0;
  while ((stop == nullptr || stop->load()) && capture.Next(&data, &size)) {
    // Realtime mode: wait for this record's original arrival time *before* the
    // record is fed, so the whole read/decode/present chain runs on the cadence
    // the live session saw. Kept outside the parse timing below (it is playback
    // throttling, not client work).
    if (paced) {
      pace(capture.timestampUs());
    }
    // Same arrival stamp the live capture hook takes, so the "本机" phases are
    // measured the same way on both sides (see hmrdp_gfx_work.h).
    if (GfxWorkMeter* meter = ActiveWorkMeter()) {
      meter->OnChunk(size);
    }
    // Dev (perf): the ZGFX + RDPGFX PDU parse is a per-record cost that is common
    // to every route (it happens before the engine or gdi sees the command), so it
    // is timed here rather than inside either backend.
    //
    // On the CPU route the gdi decode *and* the present (with its playback sleep)
    // also happen inside this call, so the deliberate pacing is subtracted back
    // out: without that, `parse=` would report most of the run's wall time and
    // could exceed `feed=` (which is defined as compute only).
    const uint64_t paceBefore = paceAccum ? paceAccum() : 0;
    const int64_t recvStart = ParseNowUs();
    HmrdpGfxReplayRecv(gfx, data, size);
    const uint64_t recvUs = static_cast<uint64_t>(ParseNowUs() - recvStart);
    const uint64_t slept = paceAccum ? paceAccum() - paceBefore : 0;
    GfxReplayAddParseUs(recvUs > slept ? recvUs - slept : 0);
  }
  // Cut short by a stop request, i.e. the capture was not played to its end.
  if (aborted != nullptr) {
    *aborted = (stop != nullptr) && !stop->load();
  }
  return true;
}

bool GfxReplayStream(const std::string& path, GfxCommandSink* sink,
                     const std::function<void()>& onFrame, const std::atomic<bool>* stop,
                     std::string* error, const ReplayPaceFn& pace,
                     const ReplayPaceAccumFn& paceAccum, bool* aborted) {
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

  const bool ok = GfxReplayPump(path, gfx, stop, error, pace, paceAccum, aborted);
  HmrdpGfxReplayFree(gfx);
  return ok;
}

}  // namespace hmrdp
