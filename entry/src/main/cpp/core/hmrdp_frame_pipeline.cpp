#include "hmrdp_frame_pipeline.h"

#include "hmrdp_gfx_cpu.h"

namespace hmrdp {

FramePipeline::FramePipeline()
    : presenter_(CreateFramePresenter()),
      host_(std::make_unique<GdiFrameHost>()) {
  host_->SetPresenter(presenter_.get());
  host_->SetMeter(&meter_);
}

FramePipeline::~FramePipeline() = default;

void FramePipeline::SetSuperResolution(bool enabled, SuperResolutionBackend backend,
                                       int outputWidth, int outputHeight) {
  presenter_->SetSuperResolution(enabled, backend, outputWidth, outputHeight);
}

}  // namespace hmrdp
