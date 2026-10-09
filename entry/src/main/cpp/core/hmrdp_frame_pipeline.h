#ifndef HMRDP_FRAME_PIPELINE_H
#define HMRDP_FRAME_PIPELINE_H

#include <memory>

#include "hmrdp_gfx_work.h"
#include "hmrdp_parallel.h"
#include "hmrdp_presenter.h"

namespace hmrdp {

class GdiFrameHost;

class FramePipeline {
 public:
  FramePipeline();
  ~FramePipeline();

  FramePipeline(const FramePipeline&) = delete;
  FramePipeline& operator=(const FramePipeline&) = delete;

  FramePresenter* presenter() { return presenter_.get(); }
  GfxWorkMeter* meter() { return &meter_; }
  GdiFrameHost* host() { return host_.get(); }
  DecodeExecutor* executor() { return &executor_; }

  void SetSuperResolution(bool enabled, SuperResolutionBackend backend, int outputWidth,
                          int outputHeight);

 private:
  // Declaration order is construction order and must match the initializer list:
  // the host is wired to the presenter and the meter as it is built.
  std::unique_ptr<FramePresenter> presenter_;
  std::unique_ptr<GdiFrameHost> host_;
  GfxWorkMeter meter_;
  DecodeExecutor executor_;
};

}  // namespace hmrdp

#endif  // HMRDP_FRAME_PIPELINE_H
