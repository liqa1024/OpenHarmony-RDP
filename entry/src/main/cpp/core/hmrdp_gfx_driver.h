#ifndef HMRDP_GFX_DRIVER_H
#define HMRDP_GFX_DRIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/rdpgfx.h>

// Provided by the patched rdpgfx client. Weak so the app still links against
// stock FreeRDP, where the replay reports "unavailable" instead of crashing.
// New/FreeWithContext build the offline RDPGFX context bound to a caller-owned
// rdpContext (so gfx->rdpcontext carries the real settings); the caller keeps
// ownership of that context.
extern "C" RdpgfxClientContext* HmrdpGfxReplayNewWithContext(rdpContext* rcontext)
    __attribute__((weak));
extern "C" void HmrdpGfxReplayFreeWithContext(RdpgfxClientContext* context)
    __attribute__((weak));
extern "C" UINT HmrdpGfxReplayRecv(RdpgfxClientContext* context, const BYTE* data, UINT32 size)
    __attribute__((weak));

namespace hmrdp {

// Optional per-record pacing hook for the "realtime" replay mode: called with a
// record's arrival time right before that record is fed, so the caller can sleep
// until the original cadence is reached. Skipped when the capture carries no
// arrival times (version 0 files).
using ReplayPaceFn = std::function<void(uint64_t timestampUs)>;
// Returns the deliberate pacing sleep accumulated so far: the frame is presented
// (and paced) inside the recv call, so the raw recv time would otherwise count
// the throttling as parse work.
using ReplayPaceAccumFn = std::function<uint64_t()>;

// Feeds every raw chunk of one capture into an already-built RDPGFX context.
// `stop` may be null; `pace`/`paceAccum` may be empty. Returns false and fills
// `error` on failure.
// `aborted` (optional) reports whether the pump was stopped before the capture
// was exhausted: a run cut short is not a measurement.
bool GfxReplayPump(const std::string& path, RdpgfxClientContext* gfx,
                   const std::atomic<bool>* stop, std::string* error,
                   const ReplayPaceFn& pace, const ReplayPaceAccumFn& paceAccum,
                   bool* aborted = nullptr);

// Accumulated ZGFX + RDPGFX PDU parse time during a replay, to separate parse
// from decode.
void GfxReplayResetParseUs();
void GfxReplayAddParseUs(uint64_t micros);
uint64_t GfxReplayParseUs();

}  // namespace hmrdp

#endif  // HMRDP_GFX_DRIVER_H
