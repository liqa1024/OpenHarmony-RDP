/*
 * HmRdp - the per-desktop frame pipeline (see hmrdp_frame_pipeline.h).
 */
#include "hmrdp_frame_pipeline.h"

#include "hmrdp_gfx_cpu.h"

namespace hmrdp {

FramePipeline::FramePipeline()
    : presenter_(CreateFramePresenter()),
      host_(std::make_unique<GdiFrameHost>()) {
  // The host drives the presenter and reports its blocked-time figures to this
  // pipeline's meter, never to a process-global one (hmrdp_gfx_cpu.h).
  host_->SetPresenter(presenter_.get());
  host_->SetMeter(&meter_);
}

FramePipeline::~FramePipeline() = default;

void FramePipeline::SetSuperResolution(bool enabled, SuperResolutionBackend backend,
                                       int outputWidth, int outputHeight) {
  presenter_->SetSuperResolution(enabled, backend, outputWidth, outputHeight);
}

}  // namespace hmrdp
