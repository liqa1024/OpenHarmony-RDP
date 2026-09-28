/*
 * HmRdp - per-frame client work meter (doc_agent/session-and-input.md §3,
 * doc_agent/gfx-engine.md §8.1).
 *
 * The live session and the offline replay feed the same meter through the same
 * hooks (raw chunk arrival before ZGX, the wrapped SurfaceCommand, the wrapped
 * EndFrame, the presenter), so a phase means the same thing in both and the two
 * can be compared per phase, normalized by payload.
 *
 * Phases: zgx+parse (chunk arrival -> first command), decode (wrapped
 * SurfaceCommand, includes the codec's own re-composite), compose (wrapped
 * EndFrame minus the present and the present-sync inside it), present (the
 * presenter call). Blocked items, reported separately and outside any work sum:
 * sync (waiting for the GPU to release the buffer this frame writes - it waits on
 * the previous frame's present) and presentWait (the part of the present span
 * blocked on the display). `pace` (the replay's throttling) is subtracted
 * outright. The transport read happens before the hook and is not covered.
 *
 * The meter exposes only the sub-items, no pre-summed total: the phases carry
 * different kinds of time and each reader adds the ones it wants.
 */
#ifndef HMRDP_GFX_WORK_H
#define HMRDP_GFX_WORK_H

#include <atomic>
#include <cstdint>
#include <functional>

#include <freerdp/client/rdpgfx.h>

namespace hmrdp {

// The GFX commands that are *not* the per-frame pixel work but sit in the same
// window (they run before the first SurfaceCommand of a chunk, so their time is
// part of `zgx+parse` - e.g. CreateSurface/ResetGraphics memset a whole surface):
// counted and timed separately so the attribution is explicit.
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
  // One window of accounted work. Per-frame figures are value / frames. No
  // aggregate "client work" total (see the file comment).
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

// Chains RdpgfxClientContext::SurfaceCommand / ::EndFrame (and the other surface
// commands) so they feed `meter`, and restores them again. The meter is stored
// per context, so a session and a replay each feed their own - there is no
// process-global meter slot, and two concurrent streams cannot feed each other's
// figures. No-op when that context is already installed. Works on any context
// whose callbacks are FreeRDP's (live gdi session, offline CPU desktop).
void GfxWorkInstall(RdpgfxClientContext* gfx, GfxWorkMeter* meter);
void GfxWorkUninstall(RdpgfxClientContext* gfx);
// The meter installed for `gfx` (nullptr when that context is not installed). The
// replay pump uses it to stamp a chunk's arrival without a global slot.
GfxWorkMeter* GfxWorkMeterFor(RdpgfxClientContext* gfx);

// A hook run at every RDPGFX START_FRAME, i.e. *before* that frame's surface
// commands write any pixel of the desktop.
//
// The presenter's zero-copy desktop buffer needs exactly this point: gdi (or the
// decoder) writes that buffer in place, so the previous frame's GPU copy out of it
// must have completed first. `update->BeginPaint` is called from
// `gdi_OutputUpdate`, i.e. *after* the frame has already written the buffer, so
// waiting only there is too late and the upload can mix two frames (blocks of the
// previous frame at the wrong positions). `bad=0` cannot catch it: the reference
// hashes gdi's own buffer, not what the presenter uploaded.
//
// Keyed by the GFX context: a live session and an offline replay each own their
// own context, and a single global hook would let a replay started next to a live
// session replace (and on stop clear) the session's wait - leaving the session's
// zero-copy upload unprotected. Pass nullptr to remove that context's hook.
void GfxWorkSetFrameBeginHook(RdpgfxClientContext* gfx, std::function<void()> hook);

// A hook that takes ownership of the raw RDPGFX channel chunk. While one is set
// for a context, FreeRDP hands the chunk over instead of processing it inline on
// the drdynvc thread, and the owner must re-enter through the replay entry
// (HmrdpGfxReplayRecv) on the thread of its choice.
//
// That is what moves the whole frame pipeline off the drdynvc dispatch thread,
// which every dynamic channel - audio and input included - shares: a heavy frame
// can then no longer hold their data back. Keyed by the GFX context; when no
// hook is set the chunk is processed inline, exactly as before.
void GfxWorkSetDataSinkHook(RdpgfxClientContext* gfx,
                            std::function<void(const uint8_t*, size_t)> hook);

}  // namespace hmrdp

#endif  // HMRDP_GFX_WORK_H
