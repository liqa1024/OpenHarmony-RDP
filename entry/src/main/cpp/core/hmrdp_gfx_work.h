#ifndef HMRDP_GFX_WORK_H
#define HMRDP_GFX_WORK_H

#include <atomic>
#include <cstdint>
#include <functional>

#include <freerdp/client/rdpgfx.h>

namespace hmrdp {

enum class GfxSetupKind {
  kReset = 0,
  kCreate,
  kDelete,
  kMap,
  kFill,
  kBlit,
  kCache,
  kImport,
  kCount,
};

class GfxWorkMeter {
 public:
  struct Sample {
    uint64_t frames = 0;
    uint64_t zgxParseUs = 0;
    uint64_t decodeUs = 0;
    uint64_t composeUs = 0;
    uint64_t presentUs = 0;
    uint64_t presentWaitUs = 0;
    uint64_t syncUs = 0;
    uint64_t bytes = 0;
    uint64_t commands = 0;
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

  void OnChunk(uint32_t bytes);
  void AccountChunkPrefix();
  void OnDecode(uint64_t micros);
  void OnSetup(GfxSetupKind kind, uint64_t micros);
  void OnPresent(uint64_t micros);
  void OnPresentWait(uint64_t micros);
  // A blocked wait inside the window AccountChunkPrefix() charges to `zgx+parse` is
  // removed from the pending prefix, or the prefix would count it twice.
  void OnBlockedBeforeFrameWork(uint64_t micros);
  void OnPresentSync(uint64_t micros);
  void OnPace(uint64_t micros);
  void OnFrameEnd(uint64_t endFrameMicros);

  void Reset();

  Sample Drain();
  Sample Peek() const;

 private:
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

void GfxWorkInstall(RdpgfxClientContext* gfx, GfxWorkMeter* meter);
void GfxWorkUninstall(RdpgfxClientContext* gfx);
GfxWorkMeter* GfxWorkMeterFor(RdpgfxClientContext* gfx);

// Run at every RDPGFX START_FRAME, i.e. *before* that frame's surface commands write any
// pixel. `update->BeginPaint` is too late: FreeRDP calls it from `gdi_OutputUpdate` after
// the frame has already written the buffer. Keyed by the GFX context, so a replay next to
// a live session cannot clear the session's wait.
void GfxWorkSetFrameBeginHook(RdpgfxClientContext* gfx, std::function<void()> hook);

// Takes ownership of the raw RDPGFX channel chunk: while set, FreeRDP hands the chunk over
// instead of processing it inline on the drdynvc thread (which every dynamic channel -
// audio and input included - shares), and the owner must re-enter through
// HmrdpGfxReplayRecv on the thread of its choice.
void GfxWorkSetDataSinkHook(RdpgfxClientContext* gfx,
                            std::function<void(const uint8_t*, size_t)> hook);

}  // namespace hmrdp

#endif  // HMRDP_GFX_WORK_H
