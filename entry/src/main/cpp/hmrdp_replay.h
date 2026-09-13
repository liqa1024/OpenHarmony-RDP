/*
 * HmRdp - dev-only recorded-RDP replay (PERF-TODO §4).
 *
 * Feeds a captured hmrdp_gfx.bin command stream through the GPU desktop engine
 * and presents each EndFrame to the XComponent surface via the shared
 * GpuPresentComposed() path. This is a DEBUG facility only: it is not a
 * production decode switch (that is FreeRDP's own "hardware decode" setting) and
 * never replaces the gdi software path.
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

class GfxReplay {
 public:
  static GfxReplay& Instance();

  // Takes ownership of `nativeWindow` (from the XComponent surface id) and
  // replays `gfxPath` onto it. Waits briefly for the first frame or failure.
  bool Start(void* nativeWindow, int surfaceW, int surfaceH, const std::string& gfxPath);
  void Resize(int width, int height);
  void Stop();
  std::string Stats();

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
  std::mutex errorMutex_;
  std::string lastError_;
};

}  // namespace hmrdp

#endif  // HMRDP_REPLAY_H
