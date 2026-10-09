#include "hmrdp_moonlight.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
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

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window_ == nullptr || mime_.empty()) {
      HMRDP_LOGE("moonlight video: start needs a surface and a setup codec");
      return;
    }
    if (!decoder_.Start(mime_, VideoOutputMode::kSurface, window_, width_, height_, redrawRate_,
                        nullptr, nullptr)) {
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
    if (!decoder_.Submit(buffer_.data(), buffer_.size(),
                         static_cast<int64_t>(decodeUnit->presentationTimeUs))) {
      return DR_NEED_IDR;
    }
    return DR_OK;
  }

 private:
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

int MoonlightVideoFormats(MoonlightVideoPreference preference) {
  const DecoderSupport avc = VideoDecoderSupport("video/avc");
  const DecoderSupport hevc = VideoDecoderSupport("video/hevc");
  const DecoderSupport av1 = VideoDecoderSupport("video/av1");

  if (preference != MoonlightVideoPreference::kAuto) {
    const bool supported =
        (preference == MoonlightVideoPreference::kH264 && avc != DecoderSupport::kNone) ||
        (preference == MoonlightVideoPreference::kHevc && hevc != DecoderSupport::kNone) ||
        (preference == MoonlightVideoPreference::kAv1 && av1 != DecoderSupport::kNone);
    if (supported) {
      if (preference == MoonlightVideoPreference::kH264) {
        return VIDEO_FORMAT_H264;
      }
      return preference == MoonlightVideoPreference::kHevc ? VIDEO_FORMAT_H265
                                                           : VIDEO_FORMAT_AV1_MAIN8;
    }
    HMRDP_LOGW("moonlight video: preferred codec has no decoder, using auto");
  }

  const int hardware = (avc == DecoderSupport::kHardware ? VIDEO_FORMAT_H264 : 0) |
                       (hevc == DecoderSupport::kHardware ? VIDEO_FORMAT_H265 : 0) |
                       (av1 == DecoderSupport::kHardware ? VIDEO_FORMAT_AV1_MAIN8 : 0);
  if (hardware != 0) {
    return hardware;
  }
  return (avc == DecoderSupport::kSoftware ? VIDEO_FORMAT_H264 : 0) |
         (hevc == DecoderSupport::kSoftware ? VIDEO_FORMAT_H265 : 0) |
         (av1 == DecoderSupport::kSoftware ? VIDEO_FORMAT_AV1_MAIN8 : 0);
}

int MoonlightVideoCodecSupport(int codec) {
  const char* mime = nullptr;
  switch (static_cast<MoonlightVideoPreference>(codec)) {
    case MoonlightVideoPreference::kH264:
      mime = "video/avc";
      break;
    case MoonlightVideoPreference::kHevc:
      mime = "video/hevc";
      break;
    case MoonlightVideoPreference::kAv1:
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
