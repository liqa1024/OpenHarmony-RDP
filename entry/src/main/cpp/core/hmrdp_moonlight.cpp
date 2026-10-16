#include "hmrdp_moonlight.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Limelight.h>

#include "hmrdp_log.h"
#include "hmrdp_presenter.h"
#include "hmrdp_video_consumer.h"
#include "hmrdp_video_decoder.h"

namespace hmrdp {

std::string MoonlightProbe() {
  STREAM_CONFIGURATION config = {};
  LiInitializeStreamConfiguration(&config);
  HMRDP_LOGI("moonlight probe: client linked (config %dx%d)", config.width, config.height);
  return "1";
}

namespace {

// Stale submit timestamps are dropped past this many outstanding frames, so a
// decoder that drops or reorders cannot grow the pairing map without bound.
constexpr size_t kMaxPendingSubmitTimes = 240;

// The codec the client decided for this session (0 = none); Setup enforces it.
std::atomic<int> g_decidedVideoFormat{0};

int64_t NowNs() {
  return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string MimeForVideoFormat(int videoFormat) {
  if ((videoFormat & VIDEO_FORMAT_MASK_H264) != 0) {
    return "video/avc";
  }
  if ((videoFormat & VIDEO_FORMAT_MASK_H265) != 0) {
    return "video/hevc";
  }
  if ((videoFormat & VIDEO_FORMAT_MASK_AV1) != 0) {
    return "video/av1";
  }
  return std::string();
}

std::string CodecTokenForMime(const std::string& mime) {
  if (mime == "video/avc") {
    return "h264";
  }
  if (mime == "video/hevc") {
    return "hevc";
  }
  if (mime == "video/av1") {
    return "av1";
  }
  return std::string();
}

// The video renderer is process-global: Moonlight's callbacks carry no per-call
// user data, and the library runs one video stream at a time.
class MoonlightVideoRenderer : public VideoFrameSink {
 public:
  static MoonlightVideoRenderer& Instance() {
    static MoonlightVideoRenderer renderer;
    return renderer;
  }

  // VideoFrameSink: the consumer thread calls this once per acquired frame. The presenter is called
  // under the hand-off lock, so DetachPresenter cannot return while a frame is being presented.
  void PresentVideoFrame(const void* buffer, int width, int height) override {
    std::lock_guard<std::mutex> lock(presenterMutex_);
    if (presenter_ == nullptr) {
      return;
    }
    if (!presenter_->PresentVideo(buffer, width, height) && !presentWarned_) {
      // One-shot: a refused frame repeats at the stream rate (the presenter logs the reason).
      presentWarned_ = true;
      HMRDP_LOGW("moonlight video: presenter refused a frame; 超分 is not presenting");
    }
  }

  void SetContext(const MoonlightVideoContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = context != nullptr ? context->window : nullptr;
  }

  // 超分: the session hand-off. The bridge only borrows the presenter: it is attached before the
  // connection starts, and Detach has returned before the session destroys it. Every use goes
  // through `presenterMutex_`, so a detach waits out a present already in flight.
  void AttachPresenter(void* presenter) {
    std::lock_guard<std::mutex> lock(presenterMutex_);
    presenter_ = static_cast<FramePresenter*>(presenter);
  }

  void DetachPresenter() {
    std::lock_guard<std::mutex> lock(presenterMutex_);
    presenter_ = nullptr;
  }

  void ResizePresentSurface(int width, int height) {
    std::lock_guard<std::mutex> lock(presenterMutex_);
    if (presenter_ != nullptr) {
      presenter_->ResizeSurface(width, height);
    }
  }

  int Setup(int videoFormat, int width, int height, int redrawRate) {
    const int decided = g_decidedVideoFormat.load();
    if (decided != 0 && videoFormat != decided) {
      HMRDP_LOGE("moonlight video: host negotiated 0x%{public}x, client chose 0x%{public}x",
                 videoFormat, decided);
      std::lock_guard<std::mutex> lock(mutex_);
      warning_ = "codec-mismatch|" + std::to_string(videoFormat);
      return -1;
    }
    const std::string mime = MimeForVideoFormat(videoFormat);
    if (mime.empty()) {
      HMRDP_LOGE("moonlight video: unsupported format 0x%{public}x", videoFormat);
      return -1;
    }
    const DecoderSupport support = VideoDecoderSupport(mime);
    if (support == DecoderSupport::kNone) {
      HMRDP_LOGE("moonlight video: no decoder for %{public}s", mime.c_str());
      std::lock_guard<std::mutex> lock(mutex_);
      warning_ = "no-decoder|" + mime;
      return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    warning_.clear();
    if (support == DecoderSupport::kSoftware) {
      warning_ = "software-decode|" + mime;
      HMRDP_LOGW("moonlight video: software decode for %{public}s", mime.c_str());
    }
    mime_ = mime;
    codec_ = CodecTokenForMime(mime);
    width_ = width;
    height_ = height;
    redrawRate_ = redrawRate;
    if (redrawRate > 0) {
      // Informational: the request was already capped at this in the control layer, so a shortfall
      // here means the ceiling moved (or was unknown) in between.
      const int maxFps = VideoDecoderMaxFps(mime, width, height);
      if (maxFps > 0 && redrawRate > maxFps) {
        HMRDP_LOGW("moonlight video: %{public}d fps over this decoder's %{public}d fps at "
                   "%{public}dx%{public}d", redrawRate, maxFps, width, height);
      }
    }
    HMRDP_LOGI("moonlight video: setup %{public}s %{public}x%{public}d @%{public}d (%{public}s)",
               mime_.c_str(), width_, height_, redrawRate_,
               VideoDecoderDescription(mime_).c_str());
    return 0;
  }

  std::string Codec() {
    std::lock_guard<std::mutex> lock(mutex_);
    return codec_;
  }

  bool HasPresenter() {
    std::lock_guard<std::mutex> lock(presenterMutex_);
    return presenter_ != nullptr;
  }

  std::string Warning() {
    std::lock_guard<std::mutex> lock(mutex_);
    return warning_;
  }

  // Cumulative counters only; the caller diffs them into rates.
  MoonlightTelemetry Telemetry() {
    MoonlightTelemetry telemetry;
    telemetry.frames = statDecoded_.load();
    telemetry.bytes = statBytes_.load();
    telemetry.submittedFrames = statSubmitted_.load();
    telemetry.droppedFrames = statDropped_.load();
    telemetry.hostLatencyTenthMs = statHostLatencySum_.load();
    telemetry.hostLatencyFrames = statHostLatencyCount_.load();
    telemetry.decodeRoundTripUs = statDecodeRtSumUs_.load();
    telemetry.decodeRoundTripFrames = statDecodeRtCount_.load();
    return telemetry;
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool sr = HasPresenter();
    // 超分: the presenter owns the real window, so the decoder renders into the consumer surface
    // instead; without a presenter the decoder renders straight into the window.
    if (sr && !consumer_.Start(width_, height_, this)) {
      HMRDP_LOGE("moonlight video: 超分 needs the offscreen consumer surface; not starting");
      return;
    }
    void* decodeWindow = sr ? consumer_.decodeWindow() : window_;
    if (decodeWindow == nullptr || mime_.empty()) {
      HMRDP_LOGE("moonlight video: start needs a surface and a setup codec");
      if (sr) {
        consumer_.Stop();
      }
      return;
    }
    ResetStats();
    // The frame callback pairs each decoded output with its submit time; surface
    // mode otherwise has no per-frame hook.
    VideoDecoder::FrameReadyFn onFrame = [this](int, int, int64_t ptsUs) {
      NoteDecoded(ptsUs);
    };
    if (!decoder_.Start(mime_, decodeWindow, width_, height_, redrawRate_, onFrame)) {
      HMRDP_LOGE("moonlight video: decoder start failed (%{public}s)",
                 decoder_.lastError().c_str());
      if (sr) {
        consumer_.Stop();
      }
      return;
    }
    running_ = true;
    HMRDP_LOGI("moonlight video: started (%{public}s)", sr ? "surface/超分" : "surface");
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool wasRunning = running_;
    running_ = false;
    // Both are idempotent, and ordered: the decoder must stop writing into the consumer surface
    // before the surface is torn down.
    decoder_.Stop();
    consumer_.Stop();
    if (wasRunning) {
      HMRDP_LOGI("moonlight video: stopped");
    }
  }

  int Submit(PDECODE_UNIT decodeUnit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || decodeUnit == nullptr) {
      return DR_NEED_IDR;
    }
    // Latency is the budget, not throughput: an access unit that has waited past it is already too
    // late to show, and leaving it queued would keep every later frame that far behind. Drop the
    // whole queue and let the library resync on a fresh IDR (it stops delivering frames until one
    // arrives, so an overload costs exactly one keyframe).
    const int64_t ageUs = decoder_.pendingAgeUs();
    if (ageUs > decoder_.inputBudgetUs()) {
      const size_t dropped = decoder_.DropPending() + 1;
      statDropped_.fetch_add(dropped, std::memory_order_relaxed);
      HMRDP_LOGW("moonlight video: input %{public}lldms behind budget, dropped %{public}llu units",
                 static_cast<long long>(ageUs / 1000),
                 static_cast<unsigned long long>(dropped));
      return DR_NEED_IDR;
    }
    // One decode unit is one Annex-B access unit, so a flat copy is the single
    // push the decoder requires.
    buffer_.clear();
    for (const LENTRY* entry = decodeUnit->bufferList; entry != nullptr; entry = entry->next) {
      buffer_.insert(buffer_.end(), entry->data, entry->data + entry->length);
    }
    if (buffer_.empty()) {
      return DR_OK;
    }
    const int64_t submitNs = NowNs();
    if (!decoder_.Submit(buffer_.data(), buffer_.size(),
                         static_cast<int64_t>(decodeUnit->presentationTimeUs))) {
      return DR_NEED_IDR;
    }
    statSubmitted_.fetch_add(1, std::memory_order_relaxed);
    if (decodeUnit->fullLength > 0) {
      statBytes_.fetch_add(static_cast<uint64_t>(decodeUnit->fullLength),
                           std::memory_order_relaxed);
    }
    if (decodeUnit->frameHostProcessingLatency > 0) {
      statHostLatencySum_.fetch_add(decodeUnit->frameHostProcessingLatency,
                                    std::memory_order_relaxed);
      statHostLatencyCount_.fetch_add(1, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      if (submitNsByPts_.size() >= kMaxPendingSubmitTimes) {
        submitNsByPts_.clear();
      }
      submitNsByPts_[static_cast<int64_t>(decodeUnit->presentationTimeUs)] = submitNs;
    }
    return DR_OK;
  }

 private:
  void ResetStats() {
    statDecoded_.store(0);
    statSubmitted_.store(0);
    statDropped_.store(0);
    statBytes_.store(0);
    statHostLatencySum_.store(0);
    statHostLatencyCount_.store(0);
    statDecodeRtSumUs_.store(0);
    statDecodeRtCount_.store(0);
    std::lock_guard<std::mutex> lock(submitMutex_);
    submitNsByPts_.clear();
  }

  // Runs once per decoded picture on the codec callback thread; pairs with the
  // submit time recorded in Submit() by presentation time.
  void NoteDecoded(int64_t ptsUs) {
    statDecoded_.fetch_add(1, std::memory_order_relaxed);
    int64_t submitNs = 0;
    {
      std::lock_guard<std::mutex> lock(submitMutex_);
      const auto it = submitNsByPts_.find(ptsUs);
      if (it == submitNsByPts_.end()) {
        return;
      }
      submitNs = it->second;
      submitNsByPts_.erase(it);
    }
    const int64_t deltaUs = (NowNs() - submitNs) / 1000;
    if (deltaUs >= 0) {
      statDecodeRtSumUs_.fetch_add(static_cast<uint64_t>(deltaUs), std::memory_order_relaxed);
      statDecodeRtCount_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::mutex mutex_;
  // Guards `presenter_` and every call into it; a detach therefore waits out a present in flight.
  std::mutex presenterMutex_;
  bool presentWarned_ = false;
  VideoDecoder decoder_;
  std::string mime_;
  std::string codec_;
  std::string warning_;
  int width_ = 0;
  int height_ = 0;
  int redrawRate_ = 0;
  void* window_ = nullptr;
  // 超分 presenter (session-owned, borrowed here); non-null switches the decode path to the
  // offscreen consumer surface. See AttachPresenter.
  FramePresenter* presenter_ = nullptr;
  // Offscreen consumer surface the decoder renders into when 超分 is on; see hmrdp_video_consumer.h.
  VideoFrameConsumer consumer_;
  bool running_ = false;
  std::vector<uint8_t> buffer_;

  // frames = decoded pictures out of the codec; submitted = access units handed to it.
  std::atomic<uint64_t> statDecoded_{0};
  std::atomic<uint64_t> statSubmitted_{0};
  std::atomic<uint64_t> statDropped_{0};
  std::atomic<uint64_t> statBytes_{0};
  std::atomic<uint64_t> statHostLatencySum_{0};
  std::atomic<uint64_t> statHostLatencyCount_{0};
  std::atomic<uint64_t> statDecodeRtSumUs_{0};
  std::atomic<uint64_t> statDecodeRtCount_{0};
  std::mutex submitMutex_;
  std::unordered_map<int64_t, int64_t> submitNsByPts_;
};

int VideoSetup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  (void)drFlags;
  MoonlightVideoRenderer::Instance().SetContext(
      static_cast<const MoonlightVideoContext*>(context));
  return MoonlightVideoRenderer::Instance().Setup(videoFormat, width, height, redrawRate);
}

void VideoStart() {
  MoonlightVideoRenderer::Instance().Start();
}

void VideoStop() {
  MoonlightVideoRenderer::Instance().Stop();
}

void VideoCleanup() {
  MoonlightVideoRenderer::Instance().Stop();
}

int VideoSubmit(PDECODE_UNIT decodeUnit) {
  return MoonlightVideoRenderer::Instance().Submit(decodeUnit);
}

// CAPABILITY_DIRECT_SUBMIT stays unset, keeping submitDecodeUnit on Moonlight's
// own decoder thread instead of the RTP receive thread.
DECODER_RENDERER_CALLBACKS g_videoCallbacks = {
    VideoSetup, VideoStart, VideoStop, VideoCleanup, VideoSubmit, 0,
};

}  // namespace

MoonlightTelemetry MoonlightReadTelemetry() {
  MoonlightTelemetry telemetry = MoonlightVideoRenderer::Instance().Telemetry();
  uint32_t rtt = 0;
  uint32_t variance = 0;
  if (LiGetEstimatedRttInfo(&rtt, &variance)) {
    telemetry.rttMs = static_cast<int32_t>(rtt);
    telemetry.rttVarianceMs = static_cast<int32_t>(variance);
  }
  const RTP_VIDEO_STATS* stats = LiGetRTPVideoStats();
  if (stats != nullptr) {
    telemetry.packetVideo = stats->packetCountVideo;
    telemetry.packetFec = stats->packetCountFec;
    telemetry.packetFecRecovered = stats->packetCountFecRecovered;
    telemetry.packetFecFailed = stats->packetCountFecFailed;
    telemetry.packetOos = stats->packetCountOOS;
    telemetry.packetInvalid = stats->packetCountInvalid;
  }
  telemetry.pendingVideoFrames = LiGetPendingVideoFrames();
  return telemetry;
}

int MoonlightVideoFormats(MoonlightVideoCodecChoice choice, int serverCodecModeSupport) {
  const DecoderSupport avc = VideoDecoderSupport("video/avc");
  const DecoderSupport hevc = VideoDecoderSupport("video/hevc");
  const DecoderSupport av1 = VideoDecoderSupport("video/av1");

  // H.264 needs no host check (the library falls back to it unconditionally);
  // AV1 / HEVC need the host's SCM_* family, gated the same as its DESCRIBE ad.
  const bool hostHevc = (serverCodecModeSupport & SCM_MASK_HEVC) != 0;
  const bool hostAv1 = (serverCodecModeSupport & SCM_MASK_AV1) != 0;

  int decided = 0;
  if (choice != MoonlightVideoCodecChoice::kAuto) {
    if (choice == MoonlightVideoCodecChoice::kH264) {
      decided = avc != DecoderSupport::kNone ? VIDEO_FORMAT_H264 : 0;
    } else if (choice == MoonlightVideoCodecChoice::kHevc) {
      decided = (hevc != DecoderSupport::kNone && hostHevc) ? VIDEO_FORMAT_H265 : 0;
    } else if (choice == MoonlightVideoCodecChoice::kAv1) {
      decided = (av1 != DecoderSupport::kNone && hostAv1) ? VIDEO_FORMAT_AV1_MAIN8 : 0;
    }
    if (decided == 0) {
      HMRDP_LOGW("moonlight video: forced codec %{public}d unusable here",
                 static_cast<int>(choice));
    }
  } else {
    if (av1 == DecoderSupport::kHardware && hostAv1) {
      decided = VIDEO_FORMAT_AV1_MAIN8;
    } else if (hevc == DecoderSupport::kHardware && hostHevc) {
      decided = VIDEO_FORMAT_H265;
    } else if (avc == DecoderSupport::kHardware) {
      decided = VIDEO_FORMAT_H264;
    } else if (av1 == DecoderSupport::kSoftware && hostAv1) {
      decided = VIDEO_FORMAT_AV1_MAIN8;
    } else if (hevc == DecoderSupport::kSoftware && hostHevc) {
      decided = VIDEO_FORMAT_H265;
    } else if (avc == DecoderSupport::kSoftware) {
      decided = VIDEO_FORMAT_H264;
    }
  }

  g_decidedVideoFormat.store(decided);
  return decided;
}

int MoonlightVideoMaxFps(int videoFormat, int width, int height) {
  return VideoDecoderMaxFps(MimeForVideoFormat(videoFormat), width, height);
}

int MoonlightVideoCodecSupport(int codec) {
  const char* mime = nullptr;
  switch (static_cast<MoonlightVideoCodecChoice>(codec)) {
    case MoonlightVideoCodecChoice::kH264:
      mime = "video/avc";
      break;
    case MoonlightVideoCodecChoice::kHevc:
      mime = "video/hevc";
      break;
    case MoonlightVideoCodecChoice::kAv1:
      mime = "video/av1";
      break;
    default:
      return static_cast<int>(DecoderSupport::kNone);
  }
  return static_cast<int>(VideoDecoderSupport(mime));
}

std::string MoonlightVideoCodec() {
  return MoonlightVideoRenderer::Instance().Codec();
}

std::string MoonlightVideoWarning() {
  return MoonlightVideoRenderer::Instance().Warning();
}

void MoonlightVideoAttachPresenter(void* presenter) {
  MoonlightVideoRenderer::Instance().AttachPresenter(presenter);
}

void MoonlightVideoDetachPresenter() {
  MoonlightVideoRenderer::Instance().DetachPresenter();
}

void MoonlightVideoResizeSurface(int width, int height) {
  MoonlightVideoRenderer::Instance().ResizePresentSurface(width, height);
}

void* MoonlightVideoCallbacks() {
  return &g_videoCallbacks;
}

}  // namespace hmrdp
