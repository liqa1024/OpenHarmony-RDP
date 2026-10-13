#ifndef HMRDP_REPLAY_H
#define HMRDP_REPLAY_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_frame_pipeline.h"

namespace hmrdp {

class GfxCpuDesktop;
struct PresentUploadInfo;

enum class GfxReplayRefMode {
  kOff = 0,
  kExport = 1,
  kCompare = 2,
};

class GfxReplay {
 public:
  GfxReplay();
  ~GfxReplay();

  bool Start(void* nativeWindow, int surfaceW, int surfaceH, const std::string& gfxPath,
             bool realtime, GfxReplayRefMode refMode = GfxReplayRefMode::kOff);
  void Resize(int width, int height);
  void Stop();
  std::string Stats();
  std::string StatsLines();

  bool OnCpuFramePre();
  void OnCpuFramePresent(GfxCpuDesktop* cpu, bool presented, uint64_t presentUs,
                         const PresentUploadInfo& upload);

  void RecordPresent(uint64_t micros);

 private:
  GfxReplay(const GfxReplay&) = delete;
  GfxReplay& operator=(const GfxReplay&) = delete;

  void Run();
  void RunCpuReplay(const std::string& gfxPath);
  void RefCollectFrame(GfxCpuDesktop* cpu);
  bool RefLoad(const std::string& capturePath);
  void RefWrite(const std::string& capturePath, GfxCpuDesktop* cpu);
  void RefCompareImage(const uint8_t* data, int stride, int width, int height);
  void MarkFrameEnd(bool presented);
  void PaceRecord(uint64_t timestampUs);

  std::mutex mutex_;
  FramePipeline pipeline_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> runId_{0};
  std::atomic<bool> aborted_{false};
  void* window_ = nullptr;
  std::string gfxPath_;
  int surfaceW_ = 0;
  int surfaceH_ = 0;
  std::atomic<int> pendingW_{0};  std::atomic<int> pendingH_{0};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> presents_{0};
  std::atomic<uint64_t> presentFailures_{0};
  std::atomic<uint64_t> presentUs_{0};
  std::atomic<uint64_t> uploadBytes_{0};
  std::atomic<uint64_t> uploadBoxBytes_{0};
  std::atomic<uint64_t> uploadRectPresents_{0};
  std::atomic<uint64_t> uploadTruncated_{0};
  std::atomic<uint64_t> uploadMaxRects_{0};
  std::atomic<int64_t> cpuStartUs_{0};
  std::atomic<int64_t> cpuEndUs_{0};
  std::atomic<uint64_t> pumpUs_{0};
  std::atomic<uint64_t> paceUs_{0};
  std::atomic<int> realtimeRequested_{0};
  std::atomic<int> realtimeActive_{0};
  std::atomic<uint64_t> realtimeLagUs_{0};
  uint64_t recordBaseUs_ = 0;
  int64_t recordWallBaseUs_ = 0;
  std::atomic<int64_t> firstFrameEndUs_{0};
  std::atomic<int64_t> pumpStartUs_{0};
  std::atomic<int64_t> startUs_{0};
  std::atomic<int64_t> endUs_{0};
  std::atomic<uint64_t> imgDiffChecks_{0};
  std::atomic<uint64_t> imgDiffBad_{0};
  std::atomic<uint64_t> imgDiffRgbPx_{0};
  std::atomic<uint64_t> imgDiffAlphaPx_{0};
  std::atomic<int> imgDiffBBoxX0_{-1};
  std::atomic<int> imgDiffBBoxY0_{-1};
  std::atomic<int> imgDiffBBoxX1_{-1};
  std::atomic<int> imgDiffBBoxY1_{-1};
  std::atomic<int> imgDiffMaxDelta_{0};
  std::atomic<uint64_t> imgDiffSmallPx_{0};

  std::vector<uint64_t> refHashes_;
  std::atomic<int> refMode_{0};
  std::atomic<uint64_t> refFrames_{0};
  std::atomic<uint64_t> refHashUs_{0};
  std::atomic<uint64_t> refChecks_{0};
  std::atomic<uint64_t> refBad_{0};
  std::atomic<int64_t> refFirstBad_{-1};
  std::vector<uint8_t> refImage_;
  int refImageW_ = 0;
  int refImageH_ = 0;
  int64_t refImageFrame_ = -1;
  std::string refNote_;
  std::string refTag_;

  std::mutex errorMutex_;
  std::string lastError_;
};

}  // namespace hmrdp

#endif
