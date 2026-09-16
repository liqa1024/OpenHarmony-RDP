/*
 * HmRdp - shared RDPGFX driver (doc_agent/gfx-engine.md §6).
 *
 * Two things live here, so the live session and the offline replay share one
 * implementation instead of each keeping their own copy:
 *
 *  1. GfxCommandSink + GfxMap*(): the RDPGFX PDU -> GPU engine command mapping.
 *     The live GFX wrappers and the replay callbacks both feed a sink, so the
 *     scalars/params/payload layout is written exactly once.
 *
 *  2. GfxReplayStream(): pumps one raw hmrdp_gfx.bin capture back through
 *     FreeRDP's own ZGX + RDPGFX parsing (needs the patched rdpgfx client) and
 *     invokes a per-frame callback. The caller only chooses what a frame means
 *     (present it, compare it, ...), so the on-screen replay and any offline
 *     pixel comparison share the whole read-decompress-parse-apply chain.
 */
#ifndef HMRDP_GFX_DRIVER_H
#define HMRDP_GFX_DRIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/rdpgfx.h>

// Provided by the patched rdpgfx client (native/scripts/patch-freerdp.ps1).
// Weak so the app still links against stock FreeRDP, where the replay reports
// "unavailable" instead of crashing.
extern "C" RdpgfxClientContext* HmrdpGfxReplayNew(void) __attribute__((weak));
extern "C" void HmrdpGfxReplayFree(RdpgfxClientContext* context) __attribute__((weak));
// Same as New/Free, but binds the plugin to a caller-owned rdpContext (used by
// the offline CPU/gdi replay route so gfx->rdpcontext carries the real
// settings); the caller keeps ownership of the context.
extern "C" RdpgfxClientContext* HmrdpGfxReplayNewWithContext(rdpContext* rcontext)
    __attribute__((weak));
extern "C" void HmrdpGfxReplayFreeWithContext(RdpgfxClientContext* context)
    __attribute__((weak));
extern "C" UINT HmrdpGfxReplayRecv(RdpgfxClientContext* context, const BYTE* data, UINT32 size)
    __attribute__((weak));

namespace hmrdp {

// Destination for GFX commands decoded from RDPGFX PDUs. The live session
// (lazily bringing the GPU engine up under its mutex) and the replay (engine
// directly) each implement it.
class GfxCommandSink {
 public:
  virtual ~GfxCommandSink() = default;
  virtual void ApplyGfx(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                        const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                        uint32_t payloadLen) = 0;
};

// RDPGFX PDU -> engine command. `sink` may be null (no-op); `pdu` may be null.
// Only the commands the GPU engine acts on are mapped. `GfxMapStartFrame` is not
// a drawing command: it marks the frame boundary the Progressive re-composite
// semantics depend on (see the implementation).
void GfxMapStartFrame(GfxCommandSink* sink, uint32_t frameId);
void GfxMapSurfaceCommand(GfxCommandSink* sink, const RDPGFX_SURFACE_COMMAND* command);
void GfxMapResetGraphics(GfxCommandSink* sink, const RDPGFX_RESET_GRAPHICS_PDU* pdu);
void GfxMapCreateSurface(GfxCommandSink* sink, const RDPGFX_CREATE_SURFACE_PDU* pdu);
void GfxMapDeleteSurface(GfxCommandSink* sink, const RDPGFX_DELETE_SURFACE_PDU* pdu);
void GfxMapSolidFill(GfxCommandSink* sink, const RDPGFX_SOLID_FILL_PDU* pdu);
void GfxMapSurfaceToSurface(GfxCommandSink* sink, const RDPGFX_SURFACE_TO_SURFACE_PDU* pdu);
void GfxMapSurfaceToCache(GfxCommandSink* sink, const RDPGFX_SURFACE_TO_CACHE_PDU* pdu);
void GfxMapCacheToSurface(GfxCommandSink* sink, const RDPGFX_CACHE_TO_SURFACE_PDU* pdu);
void GfxMapEvictCacheEntry(GfxCommandSink* sink, const RDPGFX_EVICT_CACHE_ENTRY_PDU* pdu);
void GfxMapSurfaceToOutput(GfxCommandSink* sink, const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* pdu);
void GfxMapSurfaceToScaledOutput(GfxCommandSink* sink,
                                 const RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU* pdu);

// Optional per-record pacing hook for the "realtime" replay mode: called with a
// record's recorded arrival time (monotonic microseconds) right before that
// record is fed, so the caller can sleep until the original cadence is reached.
// The call is made outside the pump's own timing, and is skipped entirely when
// the capture carries no arrival times (version 0 files).
using ReplayPaceFn = std::function<void(uint64_t timestampUs)>;
// Returns how much deliberate pacing sleep has accumulated so far. Needed because
// on the CPU route the frame is presented (and paced) *inside* the recv call, so
// the raw recv time would otherwise count the playback throttling as parse work.
using ReplayPaceAccumFn = std::function<uint64_t()>;

// Feeds every raw chunk of one hmrdp_gfx.bin capture into an already-built
// RDPGFX context (FreeRDP does the ZGX + PDU parsing). This is the whole
// read/decompress/parse loop, shared by both replay routes: the GPU route
// supplies a context whose callbacks map into a GfxCommandSink, the CPU route
// supplies a stock gdi-backed context. `stop` may be null; `pace`/`paceAccum` may
// be empty. Returns false and fills `error` (when non-null) on failure - in
// particular when FreeRDP was built without the HmRdp GFX capture patch.
bool GfxReplayPump(const std::string& path, RdpgfxClientContext* gfx,
                   const std::atomic<bool>* stop, std::string* error,
                   const ReplayPaceFn& pace, const ReplayPaceAccumFn& paceAccum);

// GPU replay route: builds the replay context, installs the GfxMap* callbacks
// feeding `sink`, pumps the capture and invokes `onFrame` after every EndFrame.
// `stop`/`pace` may be null/empty. Returns false and fills `error` on failure.
bool GfxReplayStream(const std::string& path, GfxCommandSink* sink,
                     const std::function<void()>& onFrame, const std::atomic<bool>* stop,
                     std::string* error, const ReplayPaceFn& pace,
                     const ReplayPaceAccumFn& paceAccum);

// Compare route: gdi and the engine/mirror consume the capture *interleaved, per
// PDU*. The harness wraps the callbacks gdi installed on its own context
// (`gfxB`), so each PDU goes to gdi first and to `sink` immediately after - the
// only arrangement in which a per-command A/B is valid (feeding two contexts
// chunk by chunk leaves gdi a whole chunk behind and can only compare at chunk
// boundaries). `onCommand` is invoked after every single PDU, `onFrame` (present
// / frame accounting) and `onSync` (frame comparison) on EndFrame, after gdi
// composed its frame.
bool GfxReplayStreamCompare(const std::string& path, GfxCommandSink* sink,
                            const std::function<void()>& onFrame,
                            const std::function<void()>& onSync,
                            const std::function<void()>& onCommand, RdpgfxClientContext* gfxB,
                            const std::atomic<bool>* stop, std::string* error,
                            const ReplayPaceFn& pace);

// Dev (perf): accumulated time spent in the ZGFX + RDPGFX PDU parse during a replay
// (measured in the pump, before any backend sees the command). It is a cost every
// route pays, so separating it from the backend's own time is what says whether a
// backend is actually faster than FreeRDP's gdi.
void GfxReplayResetParseUs();
void GfxReplayAddParseUs(uint64_t micros);
uint64_t GfxReplayParseUs();

}  // namespace hmrdp

#endif  // HMRDP_GFX_DRIVER_H
