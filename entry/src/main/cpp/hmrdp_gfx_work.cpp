/*
 * HmRdp - per-frame client work meter (see hmrdp_gfx_work.h).
 */
#include "hmrdp_gfx_work.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include <freerdp/channels/rdpgfx.h>

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

void GfxWorkMeter::OnPresent(uint64_t micros) {
  framePresentUs_ += micros;
}

void GfxWorkMeter::OnFrameEnd(uint64_t endFrameMicros) {
  // A frame that carried no surface command still paid for its chunk.
  AccountChunkPrefix();
  const uint64_t present = framePresentUs_;
  const uint64_t compose = endFrameMicros > present ? endFrameMicros - present : 0;
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
  frameBytes_ = 0;
  frameCommands_ = 0;
}

void GfxWorkMeter::Reset() {
  frameZgxUs_ = 0;
  frameDecodeUs_ = 0;
  framePresentUs_ = 0;
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
  g_originals.erase(it);
}

}  // namespace hmrdp
