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

#include "hmrdp_display_rate.h"
#include "hmrdp_hdr_dynamic.h"
#include "hmrdp_log.h"
#include "hmrdp_presenter.h"
#include "hmrdp_video_decoder.h"
#include "hmrdp_video_frame.h"

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

// A run of refused presentations this long confirms the video path is not presenting at all: a surface
// rebind can refuse a frame or two, which is not a broken path.
constexpr int kPresentFailuresToConfirm = 4;

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

  // VideoFrameSink: the decoder's hand-off thread calls this once per frame it hands out. The
  // presenter is called under the hand-off lock, so DetachPresenter cannot return while a frame is
  // being presented. The warning is written after that lock is released (the presenter mutex must
  // never be held while taking `mutex_`).
  int PresentVideoFrame(const void* buffer, const VideoFrameGeometry& geometry) override {
    int token = -1;
    bool srRequested = false;
    bool srActive = false;
    bool colorMismatch = false;
    {
      std::lock_guard<std::mutex> lock(presenterMutex_);
      if (presenter_ == nullptr) {
        return -1;
      }
      token = presenter_->PresentVideo(buffer, geometry);
      if (token >= 0) {
        srRequested = presenter_->superResolutionRequested();
        srActive = presenter_->superResolutionActive();
        colorMismatch = presenter_->videoColorMismatch();
      }
    }
    if (token < 0) {
      ++presentFailures_;
      // A surface rebind can refuse a frame or two; only a run of failures names a broken path.
      if (presentFailures_ >= kPresentFailuresToConfirm && !presentWarned_) {
        presentWarned_ = true;
        SetWarning("video-present");
        HMRDP_LOGW("moonlight video: presenter refused frames; the video path is not presenting");
      }
      return -1;
    }
    presentFailures_ = 0;
    if (presentWarned_) {
      presentWarned_ = false;
      ClearWarning("video-present");
    }
    statPresented_.fetch_add(1, std::memory_order_relaxed);
    if (srRequested && !srActive && !srWarned_) {
      srWarned_ = true;
      SetWarning("sr-inactive");
      HMRDP_LOGW("moonlight video: 超分 requested but the presenter is showing the input as-is");
    } else if (srRequested && srActive && srWarned_) {
      srWarned_ = false;
      ClearWarning("sr-inactive");
    }
    if (colorMismatch && !colorWarned_) {
      // The buffers' colour declaration is fixed for the session, so this is stated once and not
      // cleared: the picture may then differ from what the host encoded (see the import log line for
      // both readings).
      colorWarned_ = true;
      SetWarning("color-mismatch");
      HMRDP_LOGW("moonlight video: the buffer colour space disagrees with the one negotiated with the "
                 "host; the picture may differ from what it encoded");
    }
    return token;
  }

  void WaitVideoFrame(int token) override {
    std::lock_guard<std::mutex> lock(presenterMutex_);
    if (presenter_ != nullptr) {
      presenter_->WaitVideoFrame(token);
    }
  }

  void SetContext(const MoonlightVideoContext* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = context != nullptr ? context->window : nullptr;
  }

  void SetSurface(void* window, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = window;
    std::lock_guard<std::mutex> plock(presenterMutex_);
    if (presenter_ != nullptr) {
      if (window != nullptr) {
        presenter_->SetSurface(window, width, height);
      } else {
        presenter_->DestroySurface();
      }
    }
  }

  bool UsesBufferMode() {
    std::lock_guard<std::mutex> lock(presenterMutex_);
    return presenter_ != nullptr;
  }

  void SetSuperResolutionRequested(bool requested) { srRequested_.store(requested); }

  // The stream's encoding, in the pipeline's own terms. Rec.2020 is the 10-bit HDR stream the session
  // asked the host for; `hdrBase` picks its transfer (2 = HLG, otherwise HDR10/PQ). Everything else
  // is the negotiated SDR encoder CSC with the SDR YUV transfer. The decoder is told this (see
  // VideoDecoder::Start*) and the presenter is driven from it (see SetHdrMode).
  void SetStreamColorSpace(int colorSpace, int colorRange, int hdrBase) {
    if (colorSpace == COLORSPACE_REC_2020) {
      hdrBase_ = hdrBase == 2 ? 2 : 1;
      streamColor_ = hdrBase_ == 2 ? HlgBt2020Limited() : Hdr10Bt2020PqLimited();
      return;
    }
    hdrBase_ = 0;
    ColorDescription color;
    const bool rec601 = colorSpace == COLORSPACE_REC_601;
    color.primaries = rec601 ? ColorDescription::Primaries::kBt601SmpteC
                             : ColorDescription::Primaries::kBt709;
    color.transfer = ColorDescription::Transfer::kGamma22;
    color.matrix = rec601 ? ColorDescription::Matrix::kBt601 : ColorDescription::Matrix::kBt709;
    color.range = colorRange == COLOR_RANGE_FULL ? ColorDescription::Range::kFull
                                                : ColorDescription::Range::kLimited;
    color.bitDepth = 8;
    streamColor_ = color;
  }

  // HDR: `enabled` is what the host answered on the control stream, and it is the only HDR fact this
  // side has - the picture is presented by the platform's own decode path, so there is no output stage
  // of ours to drive (see moonlight-video.md §2).
  void SetHdrMode(bool enabled) {
    hdrActive_.store(enabled);
    HMRDP_LOGI("moonlight video: host HDR mode %{public}s", enabled ? "on" : "off");
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
    vividActive_ = false;
    hdr10PlusDropped_ = false;
    vividCurveStripped_ = false;
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
    // The decoder half of the HDR readout: the GPU half is the Vulkan verdict and the presenter's
    // surface observation. One line per session each.
    HMRDP_LOGI("moonlight video: hdr decoder: hevc10=%{public}d av1=%{public}d",
               VideoDecoderTenBit("video/hevc") ? 1 : 0, VideoDecoderTenBit("video/av1") ? 1 : 0);
    return 0;
  }

  std::string Codec() {
    std::lock_guard<std::mutex> lock(mutex_);
    return codec_;
  }

  // The HDR format the session really presents: the host's own answer, because the platform's decode
  // path presents the HDR picture and this side never touches it - nothing is claimed that the host did
  // not state. The dynamic layer is named from this side's own scan of the stream. The name is the base
  // transfer plus that layer, so the toolbar says which pair is on screen (HDR10 or HLG, with Vivid
  // when it is there).
  std::string Hdr() {
    if (!hdrActive_.load()) {
      return std::string();
    }
    const std::string base = hdrBase_.load() == 2 ? "HLG" : "HDR10";
    return vividActive_.load() ? base + " Vivid" : base;
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
    // In buffer mode the presenter is the authority on what reached the screen; in surface mode the
    // decoder's display-slot counter is.
    telemetry.presentedFrames = decoder_.presentedFrames();
    telemetry.bytes = statBytes_.load();
    telemetry.submittedFrames = statSubmitted_.load();
    telemetry.droppedFrames = statDropped_.load();
    telemetry.presentDroppedFrames = decoder_.presentDroppedFrames();
    telemetry.handoffReplacedFrames = decoder_.handOffReplacedFrames();
    telemetry.refreshDroppedFrames = decoder_.refreshDroppedFrames() + statRefreshDropped_.load();
    telemetry.displayRefreshRateHz = DisplayRefreshRateHz();
    telemetry.hostLatencyTenthMs = statHostLatencySum_.load();
    telemetry.hostLatencyFrames = statHostLatencyCount_.load();
    telemetry.decodeRoundTripUs = statDecodeRtSumUs_.load();
    telemetry.decodeRoundTripFrames = statDecodeRtCount_.load();
    {
      // Same lock order as Start: session mutex, then the presenter it borrows.
      std::lock_guard<std::mutex> plock(presenterMutex_);
      if (presenter_ != nullptr) {
        telemetry.presentedFrames = statPresented_.load();
        presenter_->TakeGpuTimings(&telemetry.gpuCopyUs, &telemetry.gpuSrUs,
                                   &telemetry.gpuBlitUs);
        telemetry.presentWaitUs = presenter_->TakePresentWaitUs();
      }
    }
    return telemetry;
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    // The presenter is built only for 超分 (see hmrdp_moonlight_session.cpp), so its presence is
    // exactly "the buffer mode": every decoded picture goes to it as a codec buffer, and 超分 is a flag
    // on that same object. Without it - every non-SR session, HDR included - the decoder renders into
    // the window itself, the platform's own video surface.
    const bool bufferMode = HasPresenter();
    if (srRequested_.load() && !bufferMode) {
      // Keep a codec warning that Setup already recorded; the two are independent session facts.
      if (warning_.find("sr-unavailable") == std::string::npos) {
        if (!warning_.empty()) {
          warning_ += ",";
        }
        warning_ += "sr-unavailable";
      }
    }
    presentWarned_ = false;
    srWarned_ = false;
    colorWarned_ = false;
    presentFailures_ = 0;
    overBudgetSinceNs_ = 0;
    lateBacklogLogged_ = false;
    // The library's HDR flag is process-global and survives a session, so a new session starts from
    // "not HDR" and only the host's own report turns it on.
    hdrActive_.store(false);
    if (mime_.empty()) {
      HMRDP_LOGE("moonlight video: start needs a setup codec");
      return;
    }
    ResetStats();
    // The frame callback pairs each decoded output with its submit time; surface
    // mode otherwise has no per-frame hook.
    VideoDecoder::FrameReadyFn onFrame = [this](int, int, int64_t ptsUs) {
      NoteDecoded(ptsUs);
    };
    // The presenter owns the real window, so the decoder hands out its own buffers instead (the
    // layout a byte-wise import needs is only reported in buffer mode, and the window never sees a
    // decoded frame); without one (an HDR session, or the fallback) the decoder renders straight into
    // the window - the platform's own HDR display path, which declares the surface itself (see
    // moonlight-video.md §2).
    const bool started = bufferMode
        ? decoder_.StartBuffered(mime_, width_, height_, redrawRate_, streamColor_, onFrame, this)
        : decoder_.Start(mime_, window_, width_, height_, redrawRate_, streamColor_, onFrame);
    if (!started) {
      HMRDP_LOGE("moonlight video: decoder start failed (%{public}s)",
                 decoder_.lastError().c_str());
      return;
    }
    running_ = true;
    HMRDP_LOGI("moonlight video: started (%{public}s, stream %{public}s)",
               bufferMode ? "buffer" : "surface",
               ToString(streamColor_).c_str());
  }

  void Stop() {
    bool wasRunning = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wasRunning = running_;
      running_ = false;
    }
    // Do not hold `mutex_` while joining the hand-off thread: a refused frame there reports its
    // warning through the same mutex, and Stop would wait for the thread that waits for the lock.
    // Idempotent, and ordered by the decoder itself: its hand-off thread stops (and returns every
    // buffer) before the codec is destroyed.
    decoder_.Stop();
    if (wasRunning) {
      HMRDP_LOGI("moonlight video: stopped");
    }
  }

  int Submit(PDECODE_UNIT decodeUnit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || decodeUnit == nullptr) {
      return DR_NEED_IDR;
    }
    // A backlog is not by itself the decoder falling behind: a network burst piles frames up too, and
    // those frames are still in the reference chain, so giving them up would cost a keyframe for a
    // delay the display gate already handles (moonlight-video.md §1).
    const int64_t ageUs = decoder_.pendingAgeUs();
    if (BacklogIsLosingGround(ageUs)) {
      const size_t dropped = decoder_.DropPending() + 1;
      statDropped_.fetch_add(dropped, std::memory_order_relaxed);
      // The queue only grows when the decoder is not draining it, and with the gear below the stream
      // rate the drain is the screen's pace — so this backlog is the refresh rate's doing too.
      if (DisplayBelowFrameRate(redrawRate_)) {
        statRefreshDropped_.fetch_add(dropped, std::memory_order_relaxed);
      }
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
    // The base version puts HDR10+ and a per-frame tone-mapping curve into what it sends; this side
    // keeps HDR10 + Vivid, so both are dropped from this copy before the codec sees it (see
    // hmrdp_hdr_dynamic.h). Both change the unit's length, so the buffer is rebuilt here.
    if (hdrBase_.load() != 0) {
      Hdr10PlusRemoval removal;
      if (PlanHdr10PlusRemoval(buffer_.data(), buffer_.size(), mime_.c_str(), &removal)) {
        const auto at = buffer_.begin() + static_cast<std::ptrdiff_t>(removal.offset);
        buffer_.erase(at, at + static_cast<std::ptrdiff_t>(removal.length));
        if (!hdr10PlusDropped_.exchange(true)) {
          HMRDP_LOGI("moonlight video: dropped the host's HDR10+ SEI (%{public}zu bytes); HDR Vivid is "
                     "this side's dynamic layer",
                     removal.length);
        }
      }
      VividCurveStrip strip;
      if (PlanVividCurveStrip(buffer_.data(), buffer_.size(), mime_.c_str(), &strip)) {
        const auto at = buffer_.begin() + static_cast<std::ptrdiff_t>(strip.offset);
        buffer_.erase(at, at + static_cast<std::ptrdiff_t>(strip.length));
        buffer_.insert(at, strip.payload, strip.payload + strip.payloadSize);
        if (!vividCurveStripped_.exchange(true)) {
          HMRDP_LOGI("moonlight video: dropped the host's Vivid tone-mapping curve (%{public}zu -> "
                     "%{public}zu bytes); the payload is statistics-only now",
                     strip.length, strip.payloadSize);
        }
      }
    }
    // The host carries HDR Vivid as a registered T.35 SEI inside the same access unit whenever its HDR
    // luminance analysis runs. The scan is this side's own reading of the dynamic layer, so it is what
    // the toolbar names the stream by (`HDR10 Vivid` / `HLG Vivid`); the platform's own decode path
    // presents the layer itself, so nothing here is forwarded anywhere.
    if (hdrActive_.load()) {
      const VividMetadata vivid = ScanVividMetadata(buffer_.data(), buffer_.size(), mime_.c_str());
      if (vivid.size != 0) {
        if (!vividActive_.load()) {
          HMRDP_LOGI("moonlight video: HDR Vivid detected (%{public}zu bytes)", vivid.size);
        }
        vividActive_.store(true);
      } else if (vividActive_.load()) {
        vividActive_.store(false);
      }
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
  // True once the backlog has stayed past the input budget for another budget's worth of wall time:
  // only that is the decoder losing ground (see Submit).
  bool BacklogIsLosingGround(int64_t ageUs) {
    if (ageUs <= decoder_.inputBudgetUs()) {
      overBudgetSinceNs_ = 0;
      lateBacklogLogged_ = false;
      return false;
    }
    const int64_t nowNs = NowNs();
    if (overBudgetSinceNs_ == 0) {
      overBudgetSinceNs_ = nowNs;
      return false;
    }
    if (nowNs - overBudgetSinceNs_ < decoder_.inputBudgetUs() * 1000) {
      if (!lateBacklogLogged_) {
        lateBacklogLogged_ = true;
        HMRDP_LOGI("moonlight video: input %{public}lldms behind budget but still draining; keeping it",
                   static_cast<long long>(ageUs / 1000));
      }
      return false;
    }
    overBudgetSinceNs_ = 0;
    lateBacklogLogged_ = false;
    return true;
  }

  void ResetStats() {
    statDecoded_.store(0);
    statPresented_.store(0);
    statSubmitted_.store(0);
    statDropped_.store(0);
    statRefreshDropped_.store(0);
    statBytes_.store(0);
    statHostLatencySum_.store(0);
    statHostLatencyCount_.store(0);
    statDecodeRtSumUs_.store(0);
    statDecodeRtCount_.store(0);
    std::lock_guard<std::mutex> lock(submitMutex_);
    submitNsByPts_.clear();
  }

  // The surface path's `video-present`: it has no sink to report a refused picture, so the codec's own
  // run of refused renders is the reading. The counter only moves on that path (a buffer-mode session
  // never renders into the surface), so the two reporters cannot fight over the token.
  void RefreshPresentWarning() {
    const bool failing = decoder_.surfacePresentFailures() >= kPresentFailuresToConfirm;
    if (failing == presentFailedSurface_.load()) {
      return;
    }
    presentFailedSurface_.store(failing);
    if (failing) {
      SetWarning("video-present");
      HMRDP_LOGW("moonlight video: the output surface refuses decoded pictures; the video path is not "
                 "presenting");
    } else {
      ClearWarning("video-present");
    }
  }

  // Runs once per decoded picture on the codec callback thread; pairs with the
  // submit time recorded in Submit() by presentation time.
  void NoteDecoded(int64_t ptsUs) {
    statDecoded_.fetch_add(1, std::memory_order_relaxed);
    RefreshPresentWarning();
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

  void SetWarning(const char* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (warning_.find(token) == std::string::npos) {
      if (!warning_.empty()) {
        warning_ += ",";
      }
      warning_ += token;
    }
  }

  void ClearWarning(const char* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string remaining;
    size_t start = 0;
    while (start <= warning_.size()) {
      const size_t comma = warning_.find(',', start);
      const std::string part = warning_.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start);
      if (!part.empty() && part.rfind(token, 0) != 0) {
        if (!remaining.empty()) {
          remaining += ',';
        }
        remaining += part;
      }
      if (comma == std::string::npos) {
        break;
      }
      start = comma + 1;
    }
    warning_ = remaining;
  }

  std::mutex mutex_;
  // Guards `presenter_` and every call into it; a detach therefore waits out a present in flight.
  std::mutex presenterMutex_;
  bool presentWarned_ = false;
  bool srWarned_ = false;
  bool colorWarned_ = false;
  int presentFailures_ = 0;
  // Input budget (BacklogIsLosingGround): when the backlog went past it, 0 while inside; and the
  // one-shot log flag for a backlog being kept.
  int64_t overBudgetSinceNs_ = 0;
  bool lateBacklogLogged_ = false;
  VideoDecoder decoder_;
  std::string mime_;
  std::string codec_;
  std::string warning_;
  int width_ = 0;
  int height_ = 0;
  int redrawRate_ = 0;
  void* window_ = nullptr;
  // 超分 presenter (session-owned, borrowed here); non-null switches the decode path to buffer mode
  // and the frames go through it. See AttachPresenter.
  FramePresenter* presenter_ = nullptr;
  std::atomic<bool> srRequested_{false};
  // HDR: the host reported its output is really in HDR. The only HDR fact this side has (see the
  // header); a session that asked for HDR but did not get it is simply an SDR session.
  std::atomic<bool> hdrActive_{false};
  // The base transfer of this session's HDR stream (0 SDR / 1 HDR10 PQ / 2 HLG), from the negotiated
  // colour space; set by SetStreamColorSpace and used to name the format the toolbar reports.
  std::atomic<int> hdrBase_{0};
  // The stream's encoding as the connection negotiated it (see SetStreamColorSpace); the decoder is
  // told this when it starts. Defaults to the SDR Rec.709 contract an unstated stream means.
  ColorDescription streamColor_;
  bool running_ = false;
  std::vector<uint8_t> buffer_;
  // Whether the stream is carrying HDR Vivid. Read by the toolbar and by the dynamic-metadata
  // hand-off; the payload itself changes every frame and is only re-declared when it does.
  std::atomic<bool> vividActive_{false};
  // Whether this session has already reported dropping the host's HDR10+ SEI (one line per session).
  std::atomic<bool> hdr10PlusDropped_{false};
  // Likewise for the Vivid tone-mapping curve the host writes beside its statistics.
  std::atomic<bool> vividCurveStripped_{false};
  // The output surface refused a run of pictures - the surface path's `video-present`
  // (see RefreshPresentWarning).
  std::atomic<bool> presentFailedSurface_{false};

  // frames = decoded pictures out of the codec; presented = frames the sink took and presented.
  std::atomic<uint64_t> statDecoded_{0};
  std::atomic<uint64_t> statPresented_{0};
  std::atomic<uint64_t> statSubmitted_{0};
  std::atomic<uint64_t> statDropped_{0};
  // Input-budget drops given up while the display's gear was below the stream rate.
  std::atomic<uint64_t> statRefreshDropped_{0};
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

// Resolved from the device rather than the negotiated stream: LiStartConnection reads these bits for
// its SDP before setup() learns the codec, so they cover every codec family this device might use.
// RFI per family = that family's hardware decoder; DIRECT_SUBMIT stays unset. See moonlight-native.md §2.
int RecoveryCapabilities() {
  int capabilities = CAPABILITY_PRESERVE_HEVC_SEI;
  if (VideoDecoderSupport("video/avc") == DecoderSupport::kHardware) {
    capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC;
  }
  if (VideoDecoderSupport("video/hevc") == DecoderSupport::kHardware) {
    capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
  }
  if (VideoDecoderSupport("video/av1") == DecoderSupport::kHardware) {
    capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
  }
  return capabilities;
}

// Capabilities are filled in here (see RecoveryCapabilities).
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
  telemetry.idrRequests = LiGetIdrRequestCount();
  return telemetry;
}

int MoonlightVideoFormats(MoonlightVideoCodecChoice choice, int serverCodecModeSupport, bool hdr) {
  const DecoderSupport avc = VideoDecoderSupport("video/avc");
  const DecoderSupport hevc = VideoDecoderSupport("video/hevc");
  const DecoderSupport av1 = VideoDecoderSupport("video/av1");

  // H.264 needs no host check (the library falls back to it unconditionally);
  // AV1 / HEVC need the host's SCM_* family, gated the same as its DESCRIBE ad.
  const bool hostHevc = (serverCodecModeSupport & SCM_MASK_HEVC) != 0;
  const bool hostAv1 = (serverCodecModeSupport & SCM_MASK_AV1) != 0;
  // A 10-bit stream needs the profile-specific bit, which is stricter than the family: Sunshine
  // advertises HEVC Main10 / AV1 Main10 separately (SCM_MASK_10BIT).
  const bool hostHevc10 = (serverCodecModeSupport & SCM_HEVC_MAIN10) != 0;
  const bool hostAv110 = (serverCodecModeSupport & SCM_AV1_MAIN10) != 0;
  const bool hevc10 = hevc != DecoderSupport::kNone && VideoDecoderTenBit("video/hevc");
  const bool av110 = av1 != DecoderSupport::kNone && VideoDecoderTenBit("video/av1");

  int decided = 0;
  if (hdr) {
    // HDR is carried by 10-bit profiles only, so H.264 is never a candidate here and a device or
    // host without one leaves the session with no codec (the caller fails before connecting rather
    // than falling back to an SDR codec the user did not ask for).
    if (choice != MoonlightVideoCodecChoice::kAuto) {
      if (choice == MoonlightVideoCodecChoice::kHevc) {
        decided = (hevc10 && hostHevc10) ? VIDEO_FORMAT_H265_MAIN10 : 0;
      } else if (choice == MoonlightVideoCodecChoice::kAv1) {
        decided = (av110 && hostAv110) ? VIDEO_FORMAT_AV1_MAIN10 : 0;
      }
      if (decided == 0) {
        HMRDP_LOGW("moonlight video: forced codec %{public}d cannot carry HDR here",
                   static_cast<int>(choice));
      }
    } else if (hevc10 && hostHevc10) {
      decided = VIDEO_FORMAT_H265_MAIN10;
    } else if (av110 && hostAv110) {
      decided = VIDEO_FORMAT_AV1_MAIN10;
    }
    g_decidedVideoFormat.store(decided);
    return decided;
  }

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
    // HEVC before AV1: this platform's AV1 support is the weaker of the two, and HEVC is also the
    // only carrier for HDR Vivid (the standard defines no AV1 carriage for it), so the same order
    // serves both HDR and SDR.
    if (hevc == DecoderSupport::kHardware && hostHevc) {
      decided = VIDEO_FORMAT_H265;
    } else if (av1 == DecoderSupport::kHardware && hostAv1) {
      decided = VIDEO_FORMAT_AV1_MAIN8;
    } else if (avc == DecoderSupport::kHardware) {
      decided = VIDEO_FORMAT_H264;
    } else if (hevc == DecoderSupport::kSoftware && hostHevc) {
      decided = VIDEO_FORMAT_H265;
    } else if (av1 == DecoderSupport::kSoftware && hostAv1) {
      decided = VIDEO_FORMAT_AV1_MAIN8;
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

std::string MoonlightVideoHdr() {
  return MoonlightVideoRenderer::Instance().Hdr();
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

void MoonlightVideoSetSuperResolutionRequested(bool requested) {
  MoonlightVideoRenderer::Instance().SetSuperResolutionRequested(requested);
}

void MoonlightVideoSetStreamColorSpace(int colorSpace, int colorRange, int hdrMode) {
  MoonlightVideoRenderer::Instance().SetStreamColorSpace(colorSpace, colorRange, hdrMode);
}

void MoonlightVideoSetHdrMode(bool enabled) {
  MoonlightVideoRenderer::Instance().SetHdrMode(enabled);
}

bool MoonlightVideoBufferMode() {
  return MoonlightVideoRenderer::Instance().UsesBufferMode();
}

void MoonlightVideoSetSurface(void* window, int width, int height) {
  MoonlightVideoRenderer::Instance().SetSurface(window, width, height);
}

void MoonlightVideoClearSurface() {
  MoonlightVideoRenderer::Instance().SetSurface(nullptr, 0, 0);
}

void MoonlightVideoResizeSurface(int width, int height) {
  MoonlightVideoRenderer::Instance().ResizePresentSurface(width, height);
}

void* MoonlightVideoCallbacks() {
  g_videoCallbacks.capabilities = RecoveryCapabilities();
  HMRDP_LOGI("moonlight video: declaring renderer capabilities 0x%{public}x",
             g_videoCallbacks.capabilities);
  return &g_videoCallbacks;
}

}  // namespace hmrdp
