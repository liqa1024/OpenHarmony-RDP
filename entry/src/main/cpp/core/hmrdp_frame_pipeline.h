/*
 * HmRdp - the per-desktop frame pipeline (doc_agent/architecture.md §4.1).
 *
 * Owns everything one desktop needs to turn decoded pixels into a presented
 * picture, and nothing else: the presenter (and with it the GPU device it will
 * own in later steps), the frame work meter and the gdi frame host. A live
 * session and an offline replay both build exactly this object, so neither of
 * them reaches for a process-global for these - the ownership contract is what
 * keeps concurrent sessions from stepping on each other.
 *
 * The decode executor and the GFX offload worker move in here next (see the
 * plan); until then they stay with their current owner.
 */
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
  // This pipeline's decode executor: the tile decode of this desktop's frames
  // runs on its own queue, never a process-global one.
  DecodeExecutor* executor() { return &executor_; }

  // 超分 request, forwarded to the presenter (FramePresenter::SetSuperResolution).
  void SetSuperResolution(bool enabled, SuperResolutionBackend backend, int outputWidth,
                          int outputHeight);

 private:
  // Declaration order is the construction order and must match the initializer
  // list: the host is wired to the presenter and the meter as it is built.
  std::unique_ptr<FramePresenter> presenter_;
  std::unique_ptr<GdiFrameHost> host_;
  GfxWorkMeter meter_;
  DecodeExecutor executor_;
};

}  // namespace hmrdp

#endif  // HMRDP_FRAME_PIPELINE_H
