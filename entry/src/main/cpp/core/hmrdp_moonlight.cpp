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
#include "hmrdp_video_decoder.h"

namespace hmrdp {

std::string MoonlightProbe() {
  STREAM_CONFIGURATION config = {};
  LiInitializeStreamConfiguration(&config);
  HMRDP_LOGI("moonlight probe: client linked (config %dx%d)", config.width, config.height);
  return "1";
}

namespace {

// Cap on decoder input backlog; a keyframe request past it is safe because
// submits run on Moonlight's own decoder thread.
constexpr size_t kMaxPendingBytes = 4u * 1024u * 1024u;

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
class MoonlightVideoRenderer {
 public:
  static MoonlightVideoRenderer& Instance() {
    static MoonlightVideoRenderer renderer;
    return renderer;
  }

  void SetContext(const MoonlightVideoContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = context != nullptr ? context->window : nullptr;
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
    HMRDP_LOGI("moonlight video: setup %{public}s %{public}x%{public}d @%{public}d (%{public}s)",
               mime_.c_str(), width_, height_, redrawRate_,
               VideoDecoderDescription(mime_).c_str());
    return 0;
  }

  std::string Codec() {
    std::lock_guard<std::mutex> lock(mutex_);
    return codec_;
  }

  std::string Warning() {
    std::lock_guard<std::mutex> lock(mutex_);
    return warning_;
  }

  // Cumulative counters only; the caller diffs them into rates.
  MoonlightTelemetry Telemetry() {
    MoonlightTelemetry telemetry;
    telemetry.frames = statFrames_.load();
    telemetry.bytes = statBytes_.load();
    telemetry.hostLatencyTenthMs = statHostLatencySum_.load();
    telemetry.hostLatencyFrames = statHostLatencyCount_.load();
    telemetry.decodeRoundTripUs = statDecodeRtSumUs_.load();
    telemetry.decodeRoundTripFrames = statDecodeRtCount_.load();
    return telemetry;
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window_ == nullptr || mime_.empty()) {
      HMRDP_LOGE("moonlight video: start needs a surface and a setup codec");
      return;
    }
    ResetStats();
    // The frame callback pairs each decoded output with its submit time; surface
    // mode otherwise has no per-frame hook.
    VideoDecoder::FrameReadyFn onFrame = [this](int, int, int64_t ptsUs) {
      NoteDecoded(ptsUs);
    };
    if (!decoder_.Start(mime_, VideoOutputMode::kSurface, window_, width_, height_, onFrame,
                        nullptr)) {
      HMRDP_LOGE("moonlight video: decoder start failed (%{public}s)",
                 decoder_.lastError().c_str());
      return;
    }
    running_ = true;
    HMRDP_LOGI("moonlight video: started");
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    running_ = false;
    decoder_.Stop();
    HMRDP_LOGI("moonlight video: stopped");
  }

  int Submit(PDECODE_UNIT decodeUnit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || decodeUnit == nullptr) {
      return DR_NEED_IDR;
    }
    if (decoder_.pendingBytes() > kMaxPendingBytes) {
      HMRDP_LOGW("moonlight video: input backlog, requesting keyframe");
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
    statFrames_.fetch_add(1, std::memory_order_relaxed);
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
    statFrames_.store(0);
    statBytes_.store(0);
    statHostLatencySum_.store(0);
    statHostLatencyCount_.store(0);
    statDecodeRtSumUs_.store(0);
    statDecodeRtCount_.store(0);
    std::lock_guard<std::mutex> lock(submitMutex_);
    submitNsByPts_.clear();
  }

  // Runs on the codec callback thread; pairs with the submit time recorded in
  // Submit() by presentation time.
  void NoteDecoded(int64_t ptsUs) {
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
  VideoDecoder decoder_;
  std::string mime_;
  std::string codec_;
  std::string warning_;
  int width_ = 0;
  int height_ = 0;
  int redrawRate_ = 0;
  void* window_ = nullptr;
  bool running_ = false;
  std::vector<uint8_t> buffer_;

  std::atomic<uint64_t> statFrames_{0};
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

void* MoonlightVideoCallbacks() {
  return &g_videoCallbacks;
}

}  // namespace hmrdp
