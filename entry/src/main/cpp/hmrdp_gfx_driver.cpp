/*
 * HmRdp - recorded RDPGFX channel replay pump (see hmrdp_gfx_driver.h).
 */
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
    // Dev (perf): the ZGFX + RDPGFX PDU parse is a per-record cost the replay
    // always pays, so it is timed here rather than inside the decoder.
    //
    // The gdi decode *and* the present (with its playback sleep) also happen
    // inside this call, so the deliberate pacing is subtracted back out: without
    // that, `parse=` would report most of the run's wall time and could exceed
    // `feed=` (which is defined as compute only).
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
