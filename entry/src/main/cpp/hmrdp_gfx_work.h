/*
 * HmRdp - per-frame client work meter (see doc_agent/session-and-input.md §3).
 *
 * The live session and the offline replay feed the *same* meter through the
 * *same* hooks - the raw chunk arrival (before ZGX), the wrapped GFX
 * SurfaceCommand, the wrapped GFX EndFrame and the presenter - so "本机" means
 * exactly the same thing in a live session and in a replay of it. That is what
 * makes the two figures comparable: a live frame and the same frame replayed
 * offline can be put side by side, per phase and normalized by payload.
 *
 * What the phases are:
 *   zgx+parse  chunk arrival (capture hook, before ZGX) -> first command of it
 *   decode     the wrapped SurfaceCommand (includes the codec's own re-composite)
 *   compose    the wrapped EndFrame minus the present inside it (gdi
 *              surface -> primary); the ack FreeRDP writes afterwards is outside
 *   present    the presenter (the caller times its own present call)
 * Not covered: the transport read (it happens before the hook).
 */
#ifndef HMRDP_GFX_WORK_H
#define HMRDP_GFX_WORK_H

#include <atomic>
#include <cstdint>

#include <freerdp/client/rdpgfx.h>

namespace hmrdp {

class GfxWorkMeter {
 public:
  // One window of accounted work. Per-frame figures are value / frames.
  struct Sample {
    uint64_t frames = 0;      // frames the server ended (EndFrame markers)
    uint64_t zgxParseUs = 0;  // chunk arrival -> first command of that chunk
    uint64_t decodeUs = 0;    // wrapped SurfaceCommand
    uint64_t composeUs = 0;   // gdi EndFrame minus the present inside it
    uint64_t presentUs = 0;   // presenter
    uint64_t bytes = 0;       // raw (still ZGX-compressed) GFX bytes
    uint64_t commands = 0;    // surface commands, i.e. Progressive/bitmap messages
    uint64_t maxFrameUs = 0;  // worst single frame's total client work

    uint64_t WorkUs() const { return zgxParseUs + decodeUs + composeUs + presentUs; }
  };

  // --- fed on the thread that handles the stream (RDP thread / replay thread) ---
  // One raw GFX chunk arrived, *after* any capture write so that disk I/O does
  // not land in the measured phases. `bytes` is the still-compressed size.
  void OnChunk(uint32_t bytes);
  // Charges the time since that chunk arrived to the frame being handled; the
  // first command of a chunk is where the ZGX decompress + PDU parse share ends.
  void AccountChunkPrefix();
  // One decoded surface command (also counts the message).
  void OnDecode(uint64_t micros);
  // The presenter finished one present of the current frame.
  void OnPresent(uint64_t micros);
  // The frame's EndFrame returned; `micros` is the whole EndFrame (compose +
  // present). Closes the frame and moves its work into the window totals.
  void OnFrameEnd(uint64_t endFrameMicros);

  // Drops everything in flight and the window totals (run/connection start).
  void Reset();

  // --- read from the reporting thread ---
  // Destructive: the live telemetry reports one window per second.
  Sample Drain();
  // Non-destructive: the replay reports whole-run totals while the UI polls.
  Sample Peek() const;

 private:
  // Per-frame accumulators, touched only on the stream thread: a frame always
  // contributes its work together with its frame count, so a window boundary can
  // split at most one frame.
  uint64_t frameZgxUs_ = 0;
  uint64_t frameDecodeUs_ = 0;
  uint64_t framePresentUs_ = 0;
  uint64_t frameBytes_ = 0;
  uint64_t frameCommands_ = 0;
  bool chunkPending_ = false;
  uint64_t chunkArrivalUs_ = 0;

  // Window totals, written on the stream thread and drained by the reporter.
  std::atomic<uint64_t> windowFrames_{0};
  std::atomic<uint64_t> windowZgxUs_{0};
  std::atomic<uint64_t> windowDecodeUs_{0};
  std::atomic<uint64_t> windowComposeUs_{0};
  std::atomic<uint64_t> windowPresentUs_{0};
  std::atomic<uint64_t> windowBytes_{0};
  std::atomic<uint64_t> windowCommands_{0};
  std::atomic<uint64_t> windowMaxFrameUs_{0};
};

// The meter currently installed (nullptr when nothing measures). The capture hook
// and the GFX wrappers fan out to it, so live and replay share the wiring. One
// stream is measured at a time: a connected session owns the slot, and a replay
// started next to one must not take it over (it reports nothing instead).
GfxWorkMeter* ActiveWorkMeter();
void SetActiveWorkMeter(GfxWorkMeter* meter);

// Chains RdpgfxClientContext::SurfaceCommand / ::EndFrame so they feed the active
// meter, and restores them again. No-op when that context is already installed.
// Works on any context whose callbacks are FreeRDP's (live gdi session, offline
// CPU desktop).
void GfxWorkInstall(RdpgfxClientContext* gfx);
void GfxWorkUninstall(RdpgfxClientContext* gfx);

}  // namespace hmrdp

#endif  // HMRDP_GFX_WORK_H
