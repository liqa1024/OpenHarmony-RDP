/*
 * HmRdp - recorded RDPGFX channel replay pump (doc_agent/gfx-engine.md §6).
 *
 * Feeds one raw hmrdp_gfx.bin capture back through FreeRDP's own ZGX + RDPGFX
 * parsing. The live session and the offline replay share this read-decompress-
 * parse loop; the caller only chooses what a frame means (present it, account for
 * it, ...).
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
//
// New/FreeWithContext build the offline RDPGFX context bound to a caller-owned
// rdpContext (the CPU/gdi replay route, so gfx->rdpcontext carries the real
// settings); the caller keeps ownership of that context.
extern "C" RdpgfxClientContext* HmrdpGfxReplayNewWithContext(rdpContext* rcontext)
    __attribute__((weak));
extern "C" void HmrdpGfxReplayFreeWithContext(RdpgfxClientContext* context)
    __attribute__((weak));
extern "C" UINT HmrdpGfxReplayRecv(RdpgfxClientContext* context, const BYTE* data, UINT32 size)
    __attribute__((weak));

namespace hmrdp {

// Optional per-record pacing hook for the "realtime" replay mode: called with a
// record's recorded arrival time (monotonic microseconds) right before that
// record is fed, so the caller can sleep until the original cadence is reached.
// The call is made outside the pump's own timing, and is skipped entirely when
// the capture carries no arrival times (version 0 files).
using ReplayPaceFn = std::function<void(uint64_t timestampUs)>;
// Returns how much deliberate pacing sleep has accumulated so far. Needed because
// the frame is presented (and paced) *inside* the recv call, so the raw recv time
// would otherwise count the playback throttling as parse work.
using ReplayPaceAccumFn = std::function<uint64_t()>;

// Feeds every raw chunk of one hmrdp_gfx.bin capture into an already-built
// RDPGFX context (FreeRDP does the ZGX + PDU parsing). `stop` may be null;
// `pace`/`paceAccum` may be empty. Returns false and fills `error` (when
// non-null) on failure - in particular when FreeRDP was built without the HmRdp
// GFX capture patch.
//
// `aborted` (optional) reports whether the pump was stopped before the capture
// was exhausted: a run that was cut short is not a measurement, and the reference
// export must not be written from it.
bool GfxReplayPump(const std::string& path, RdpgfxClientContext* gfx,
                   const std::atomic<bool>* stop, std::string* error,
                   const ReplayPaceFn& pace, const ReplayPaceAccumFn& paceAccum,
                   bool* aborted = nullptr);

// Dev (perf): accumulated time spent in the ZGFX + RDPGFX PDU parse during a replay
// (measured in the pump, before any backend sees the command). It is a cost the
// replay always pays, so separating it from the decoder's own time is what says
// how much of a run went into parsing.
void GfxReplayResetParseUs();
void GfxReplayAddParseUs(uint64_t micros);
uint64_t GfxReplayParseUs();

}  // namespace hmrdp

#endif  // HMRDP_GFX_DRIVER_H
