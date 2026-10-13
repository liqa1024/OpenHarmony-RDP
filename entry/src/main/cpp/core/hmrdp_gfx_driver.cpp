#include "hmrdp_gfx_driver.h"

#include <atomic>
#include <chrono>

#include "hmrdp_gfx_capture.h"
#include "hmrdp_gfx_work.h"

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
    // Realtime mode: wait for this record's original arrival time before feeding,
    // so the chain runs on the live cadence (excluded from the parse timing below).
    if (paced) {
      pace(capture.timestampUs());
    }
    // Same arrival stamp the live sink takes, so the "本机" phases measure the same
    // on both sides; the meter is this replay's own, not a process-global slot.
    if (GfxWorkMeter* meter = GfxWorkMeterFor(gfx)) {
      meter->OnChunk(size);
    }
    // Dev (perf): the ZGFX + RDPGFX parse is timed here; the gdi decode and present
    // (with their playback sleep) also run inside, so the pacing is subtracted back out.
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

}  // namespace hmrdp
