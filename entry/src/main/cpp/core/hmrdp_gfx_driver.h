#ifndef HMRDP_GFX_DRIVER_H
#define HMRDP_GFX_DRIVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client/rdpgfx.h>

// Provided by the patched rdpgfx client; weak so the app still links against stock FreeRDP.
// ...WithContext bind the offline context to a caller-owned rdpContext; the caller keeps ownership.
extern "C" RdpgfxClientContext* HmrdpGfxReplayNewWithContext(rdpContext* rcontext)
    __attribute__((weak));
extern "C" void HmrdpGfxReplayFreeWithContext(RdpgfxClientContext* context)
    __attribute__((weak));
extern "C" UINT HmrdpGfxReplayRecv(RdpgfxClientContext* context, const BYTE* data, UINT32 size)
    __attribute__((weak));

namespace hmrdp {

// Optional per-record pacing hook for "realtime" replay: called with a record's arrival time
// right before it is fed; skipped when the capture carries no arrival times (version 0 files).
using ReplayPaceFn = std::function<void(uint64_t timestampUs)>;
// Returns the deliberate pacing sleep so far: presenting/pacing happens inside the recv call,
// so raw recv time would otherwise count throttling as parse work.
using ReplayPaceAccumFn = std::function<uint64_t()>;

// `aborted` reports whether the pump stopped before the capture was exhausted: a run cut short
// is not a measurement.
bool GfxReplayPump(const std::string& path, RdpgfxClientContext* gfx,
                   const std::atomic<bool>* stop, std::string* error,
                   const ReplayPaceFn& pace, const ReplayPaceAccumFn& paceAccum,
                   bool* aborted = nullptr);

// Accumulated ZGFX + RDPGFX PDU parse time during a replay, to separate parse from decode.
void GfxReplayResetParseUs();
void GfxReplayAddParseUs(uint64_t micros);
uint64_t GfxReplayParseUs();

}

#endif
