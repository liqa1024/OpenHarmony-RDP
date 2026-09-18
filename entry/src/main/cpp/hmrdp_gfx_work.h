/*
 * HmRdp - per-frame client work meter (see doc_agent/session-and-input.md §3).
 *
 * The live session and the offline replay feed the *same* meter through the
 * *same* hooks - the raw chunk arrival (before ZGX), the wrapped GFX
 * SurfaceCommand, the wrapped GFX EndFrame and the presenter - so every phase
 * means exactly the same thing in a live session and in a replay of it. That is
 * what makes the two comparable: a live frame and the same frame replayed offline
 * can be put side by side, per phase and normalized by payload.
 *
 * The meter deliberately exposes *only* the sub-items, no pre-summed "client
 * work" total: the phases carry different kinds of time and each reader adds the
 * ones it wants (the live toolbar sums the four work phases and leaves the
 * blocked `sync` out).
 *
 * What the phases are:
 *   zgx+parse  chunk arrival (capture hook, before ZGX) -> first command of it
 *   decode     the wrapped SurfaceCommand (includes the codec's own re-composite)
 *   compose    the wrapped EndFrame minus the present and the present sync inside
 *              it (gdi surface -> primary); the ack FreeRDP writes afterwards is
 *              outside
 *   present    the presenter (the caller times its own present call)
 *   sync       waiting for the GPU to release the pixel source before this frame
 *              may write it (the CPU route composes into the presenter's own
 *              buffer, so the previous frame's read has to drain first). It is
 *              blocked time, not compose work, and it is not present work either
 *              - it waits on the *previous* frame's present.
 * Not covered: the transport read (it happens before the hook).
 *
 * `pace` (the replay's deliberate playback throttling) is the one thing that is
 * subtracted outright: it is not client work at all.
 */
#ifndef HMRDP_GFX_WORK_H
#define HMRDP_GFX_WORK_H

#include <atomic>
#include <cstdint>
#include <functional>

#include <freerdp/client/rdpgfx.h>

namespace hmrdp {

// The GFX commands that are *not* the per-frame pixel work but sit in the same
// window (they run before the first SurfaceCommand of a chunk, so their time was
// landing in the `zgx+parse` bucket - e.g. CreateSurface/ResetGraphics memset a
// whole surface): counted and timed separately so the attribution is explicit.
enum class GfxSetupKind {
  kReset = 0,     // ResetGraphics (memsets every surface + resets both codecs)
  kCreate,        // CreateSurface (allocates + 0xFF-fills a whole surface)
  kDelete,        // DeleteSurface
  kMap,           // MapSurfaceToOutput / MapSurfaceToScaledOutput
  kFill,          // SolidFill
  kBlit,          // SurfaceToSurface
  kCache,         // SurfaceToCache / CacheToSurface / EvictCacheEntry
  kImport,        // Import/ExportCacheEntry / CacheImportReply / DeleteEncodingContext
  kCount,
};

class GfxWorkMeter {
 public:
  // One window of accounted work. Per-frame figures are value / frames.
  //
  // Deliberately *no* aggregate "client work" figure: the phases carry different
  // kinds of time (CPU work, GPU submit, blocked waits) and the right sum depends
  // on who is reading it, so each reader adds the sub-items it wants. The live
  // toolbar shows the four work phases and leaves `syncUs` (blocked) out; the
  // replay reports every sub-item on its own. Hoisting a total in here would bake
  // in one reader's choice (and one reader's inclusion of waiting).
  struct Sample {
    uint64_t frames = 0;      // frames the server ended (EndFrame markers)
    uint64_t zgxParseUs = 0;  // chunk arrival -> first command of that chunk
    uint64_t decodeUs = 0;    // wrapped SurfaceCommand
    uint64_t composeUs = 0;   // gdi EndFrame minus present, present sync and pacing
    uint64_t presentUs = 0;   // presenter's own work (record/upload/submit)
    uint64_t presentWaitUs = 0;  // of the present span, the part blocked on the display
    uint64_t syncUs = 0;      // waiting for the GPU buffer the frame writes into
    uint64_t bytes = 0;       // raw (still ZGX-compressed) GFX bytes
    uint64_t commands = 0;    // surface commands, i.e. Progressive/bitmap messages
    // Non-pixel GFX commands (see GfxSetupKind), counted/timed per kind.
    uint64_t setupUs[static_cast<int>(GfxSetupKind::kCount)] = {};
    uint64_t setupCount[static_cast<int>(GfxSetupKind::kCount)] = {};

    uint64_t SetupUs() const {
      uint64_t total = 0;
      for (uint64_t us : setupUs) {
        total += us;
      }
      return total;
    }
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
  // One non-pixel GFX command (surface lifecycle / mapping / fill / blit /
  // cache). Reported separately from the per-frame phases.
  void OnSetup(GfxSetupKind kind, uint64_t micros);
  // The presenter finished one present of the current frame; `micros` is the
  // present's *work* span (the caller subtracts the blocked part below).
  void OnPresent(uint64_t micros);
  // Of that present span, the part spent blocked on the display (waiting for a
  // swapchain image / the buffer swap). Blocked time, not work: reported as its
  // own sub-item so a reader summing the work phases leaves it out. Disjoint from
  // `OnPresent` (present + presentWait = the present's whole span).
  void OnPresentWait(uint64_t micros);
  // Blocked time the frame paid *before* its first command, i.e. inside the window
  // AccountChunkPrefix() charges to `zgx+parse`: the wait for the GPU to release the
  // buffer this frame writes (the frame-begin hook runs before the first command).
  // It is the same figure the caller reports as `sync`, so it is removed from the
  // pending prefix here - otherwise the prefix would count it twice whenever the
  // GPU is the slower side. No-op when no prefix is pending (nothing to correct).
  void OnBlockedBeforeFrameWork(uint64_t micros);
  // The frame waited for the GPU to finish reading the buffer it composes into
  // (one desktop buffer, so the previous frame's copy has to drain first). Blocked
  // time: taken out of the compose share and reported as its own phase, because it
  // says nothing about how expensive the composition itself is. It is never part of
  // any work sum - the frame's whole span is the work phases plus this.
  void OnPresentSync(uint64_t micros);
  // Playback throttling that happened *inside* the frame (the CPU replay paces
  // between presented frames, and that sleep sits inside gdi's EndFrame). It is
  // not client work, so it is subtracted from the compose share.
  void OnPace(uint64_t micros);
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
  uint64_t framePresentWaitUs_ = 0;
  uint64_t frameSyncUs_ = 0;
  uint64_t framePaceUs_ = 0;
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
  std::atomic<uint64_t> windowPresentWaitUs_{0};
  std::atomic<uint64_t> windowSyncUs_{0};
  std::atomic<uint64_t> windowBytes_{0};
  std::atomic<uint64_t> windowCommands_{0};
  std::atomic<uint64_t> windowSetupUs_[static_cast<int>(GfxSetupKind::kCount)];
  std::atomic<uint64_t> windowSetupCount_[static_cast<int>(GfxSetupKind::kCount)];
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

// A hook run at every RDPGFX START_FRAME, i.e. *before* that frame's surface
// commands write any pixel of the desktop.
//
// The presenter's zero-copy desktop buffer needs exactly this point: gdi (or the
// decoder) writes that buffer in place, so the previous frame's GPU copy out of it
// must have completed first. FreeRDP's `update->BeginPaint` - where that wait sits
// today - is called from `gdi_OutputUpdate`, i.e. *after* the frame has already
// written the buffer, so it waits too late and the upload can be a mix of two
// frames (visible as blocks of the previous frame at the wrong positions, and
// invisible to the pixel A/B, which reads gdi's own buffer).
//
// Keyed by the GFX context: a live session and an offline replay each own their
// own context, and a single global hook would let a replay started next to a live
// session replace (and on stop clear) the session's wait - leaving the session's
// zero-copy upload unprotected. Pass nullptr to remove that context's hook.
void GfxWorkSetFrameBeginHook(RdpgfxClientContext* gfx, std::function<void()> hook);

}  // namespace hmrdp

#endif  // HMRDP_GFX_WORK_H
