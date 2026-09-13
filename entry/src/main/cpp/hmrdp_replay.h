/*
 * HmRdp - dev-only recorded-RDP replay (PERF-TODO §4).
 *
 * Replays a captured raw GFX channel stream (hmrdp_gfx.bin) through FreeRDP's
 * own ZGX + RDPGFX parsing into the GPU desktop engine and presents each frame
 * to the XComponent surface. Debug facility only; never replaces the gdi path.
 */
#ifndef HMRDP_REPLAY_H
#define HMRDP_REPLAY_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hmrdp {

class Renderer;
class GfxGpuDesktop;

class GfxReplay {
 public:
  static GfxReplay& Instance();

  // Takes ownership of `nativeWindow` (from the XComponent surface id) and
  // replays `gfxPath` (a raw hmrdp_gfx.bin capture) onto it. Waits briefly for
  // the first frame or failure.
  bool Start(void* nativeWindow, int surfaceW, int surfaceH, const std::string& gfxPath);
  void Resize(int width, int height);
  void Stop();
  std::string Stats();

  // Called by the replay GFX callbacks on every EndFrame (replay thread).
  void OnReplayFrame();

 private:
  GfxReplay() = default;
  ~GfxReplay();
  GfxReplay(const GfxReplay&) = delete;
  GfxReplay& operator=(const GfxReplay&) = delete;

  void Run();
  void RunReplay(const std::string& gfxPath);

  std::mutex mutex_;
  std::unique_ptr<Renderer> renderer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  void* window_ = nullptr;
  std::string gfxPath_;
  int surfaceW_ = 0;
  int surfaceH_ = 0;
  std::atomic<int> pendingW_{0};
  std::atomic<int> pendingH_{0};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> presents_{0};
  std::atomic<uint64_t> presentFailures_{0};
  std::atomic<int64_t> startUs_{0};
  // Replay-thread only (no locking needed).
  GfxGpuDesktop* engine_ = nullptr;
  int64_t nextFrameUs_ = 0;
  std::mutex errorMutex_;
  std::string lastError_;
};

}  // namespace hmrdp

#endif  // HMRDP_REPLAY_H
