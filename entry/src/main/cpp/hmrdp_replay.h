/*
 * HmRdp - dev-only recorded-RDP replay (PERF-TODO §4).
 *
 * Feeds a captured hmrdp_gfx.bin command stream through an engine and presents
 * each EndFrame to the XComponent surface via the shared GpuPresentComposed()
 * path. This is a DEBUG facility only: `useCpu` selects the GPU engine or the
 * CPU reference at runtime so both can be compared on identical input. It is not
 * a production soft/hard switch (that is FreeRDP's own "hardware decode"
 * setting) and never replaces the gdi software path.
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
  // replays `gfxPath` onto it. `useCpu` selects the CPU reference (debug) or the
  // GPU engine. Waits briefly for the first frame or failure.
  bool Start(void* nativeWindow, int surfaceW, int surfaceH, const std::string& gfxPath,
             bool useCpu);
  void Resize(int width, int height);
  void Stop();
  std::string Stats();

 private:
  GfxReplay() = default;
  ~GfxReplay();
  GfxReplay(const GfxReplay&) = delete;
  GfxReplay& operator=(const GfxReplay&) = delete;

  void Run();
  template <typename Engine>
  void RunEngine(const std::string& gfxPath);

  std::mutex mutex_;
  std::unique_ptr<Renderer> renderer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  void* window_ = nullptr;
  std::string gfxPath_;
  std::atomic<int> cpuMode_{0};  // 0 = GPU engine, 1 = CPU reference (debug)
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
