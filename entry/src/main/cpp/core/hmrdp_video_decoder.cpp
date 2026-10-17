#include "hmrdp_video_decoder.h"

#include <chrono>
#include <cstring>
#include <utility>

#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avbuffer_info.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>

#include "hmrdp_display_rate.h"
#include "hmrdp_log.h"

namespace hmrdp {

namespace {

// Hardware first; a null capability with `hardware == false` means the platform
// has no decoder.
struct PickedCapability {
  OH_AVCapability* capability = nullptr;
  bool hardware = false;
};

PickedCapability PickCapability(const std::string& mime) {
  OH_AVCapability* capability =
      OH_AVCodec_GetCapabilityByCategory(mime.c_str(), false, HARDWARE);
  if (capability != nullptr) {
    return {capability, true};
  }
  return {OH_AVCodec_GetCapabilityByCategory(mime.c_str(), false, SOFTWARE), false};
}

// Input latency budget: room to absorb a burst of arriving frames, but a ceiling on how far behind
// the decoder may fall before frames stop being worth showing. Scales with the frame period so it
// means the same at any stream rate.
constexpr int64_t kMinInputBudgetUs = 30000;
constexpr int64_t kMaxInputBudgetUs = 150000;
constexpr int64_t kUnknownFrameRateBudgetUs = 75000;
constexpr int64_t kBudgetFramePeriods = 3;

int64_t NowUs() {
  return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

int64_t BudgetForFrameRate(int frameRate) {
  if (frameRate <= 0) {
    return kUnknownFrameRateBudgetUs;
  }
  const int64_t budget = kBudgetFramePeriods * 1000000 / frameRate;
  if (budget < kMinInputBudgetUs) {
    return kMinInputBudgetUs;
  }
  return budget > kMaxInputBudgetUs ? kMaxInputBudgetUs : budget;
}

}  // namespace

DecoderSupport VideoDecoderSupport(const std::string& mime) {
  const PickedCapability picked = PickCapability(mime);
  if (picked.capability == nullptr) {
    return DecoderSupport::kNone;
  }
  return picked.hardware ? DecoderSupport::kHardware : DecoderSupport::kSoftware;
}

bool VideoDecoderAvailable(const std::string& mime) {
  return VideoDecoderSupport(mime) != DecoderSupport::kNone;
}

std::string VideoDecoderDescription(const std::string& mime) {
  const PickedCapability picked = PickCapability(mime);
  if (picked.capability == nullptr) {
    return std::string();
  }
  const char* name = OH_AVCapability_GetName(picked.capability);
  return (picked.hardware ? "hw:" : "sw:") + std::string(name != nullptr ? name : "?");
}

int VideoDecoderMaxFps(const std::string& mime, int width, int height) {
  if (width <= 0 || height <= 0) {
    return 0;
  }
  const PickedCapability picked = PickCapability(mime);
  if (picked.capability == nullptr) {
    return 0;
  }
  OH_AVRange range = {-1, -1};
  if (OH_AVCapability_GetVideoFrameRateRangeForSize(picked.capability, width, height, &range) !=
      AV_ERR_OK) {
    return 0;
  }
  return range.maxVal > 0 ? static_cast<int>(range.maxVal) : 0;
}

VideoDecoder::~VideoDecoder() {
  Stop();
}

void VideoDecoder::SetError(const std::string& message) {
  std::lock_guard<std::mutex> lock(errorMutex_);
  if (error_.empty()) {
    error_ = message;
  }
}

std::string VideoDecoder::lastError() const {
  std::lock_guard<std::mutex> lock(errorMutex_);
  return error_;
}

bool VideoDecoder::Start(const std::string& mime, void* window, int width, int height,
                         int frameRate, FrameReadyFn onFrame) {
  return StartInternal(mime, window, width, height, frameRate, std::move(onFrame), nullptr);
}

bool VideoDecoder::StartBuffered(const std::string& mime, int width, int height, int frameRate,
                                 FrameReadyFn onFrame, VideoFrameSink* sink) {
  if (sink == nullptr) {
    SetError("buffer mode needs a frame sink");
    return false;
  }
  return StartInternal(mime, nullptr, width, height, frameRate, std::move(onFrame), sink);
}

bool VideoDecoder::StartInternal(const std::string& mime, void* window, int width, int height,
                                 int frameRate, FrameReadyFn onFrame, VideoFrameSink* sink) {
  if (codec_ != nullptr) {
    return started_.load();
  }
  buffered_ = sink != nullptr;
  if (!buffered_ && window == nullptr) {
    SetError("decode output surface missing");
    return false;
  }
  sink_ = sink;
  onFrame_ = std::move(onFrame);
  inputBudgetUs_ = BudgetForFrameRate(frameRate);
  statPresented_.store(0);
  statPresentDropped_.store(0);
  statHandOffReplaced_.store(0);
  nativeBufferWarned_ = false;
  // Until the codec describes the stream, the frame layout is the requested size (the same reading
  // the output description will carry; it is only read by the hand-off).
  width_.store(width);
  height_.store(height);
  {
    std::lock_guard<std::mutex> lock(outputMutex_);
    geometry_.visibleWidth = width;
    geometry_.visibleHeight = height;
  }

  // Create the hardware decoder when there is one; software is the fallback the
  // caller warns about, never the silent default.
  const PickedCapability picked = PickCapability(mime);
  if (picked.capability == nullptr) {
    SetError("no decoder for " + mime);
    return false;
  }
  const char* name = OH_AVCapability_GetName(picked.capability);
  codec_ = name != nullptr ? OH_VideoDecoder_CreateByName(name)
                           : OH_VideoDecoder_CreateByMime(mime.c_str());
  if (codec_ == nullptr) {
    SetError("cannot create decoder for " + mime);
    return false;
  }

  OH_AVCodecCallback callback;
  callback.onError = OnError;
  callback.onStreamChanged = OnStreamChanged;
  callback.onNeedInputBuffer = OnNeedInput;
  callback.onNewOutputBuffer = OnNewOutput;
  if (OH_VideoDecoder_RegisterCallback(codec_, callback, this) != AV_ERR_OK) {
    SetError("RegisterCallback failed");
    Stop();
    return false;
  }

  int32_t configured = ConfigureCodec(width, height, frameRate);
  if (configured != AV_ERR_OK && frameRate > 0) {
    // A rate key the platform rejects (an out-of-range frame rate) must not cost the session:
    // retry with the required keys only.
    HMRDP_LOGW("video decoder: %{public}d fps rejected (%{public}d), configuring without it",
               frameRate, configured);
    configured = ConfigureCodec(width, height, 0);
  }
  if (configured != AV_ERR_OK) {
    SetError("Configure failed: " + std::to_string(configured));
    Stop();
    return false;
  }

  if (!buffered_ &&
      OH_VideoDecoder_SetSurface(codec_, reinterpret_cast<OHNativeWindow*>(window)) != AV_ERR_OK) {
    SetError("SetSurface failed");
    Stop();
    return false;
  }
  if (OH_VideoDecoder_Prepare(codec_) != AV_ERR_OK) {
    SetError("Prepare failed");
    Stop();
    return false;
  }
  if (OH_VideoDecoder_Start(codec_) != AV_ERR_OK) {
    SetError("Start failed");
    Stop();
    return false;
  }
  started_ = true;
  if (buffered_) {
    // Some platforms publish the byte layout only in the output description; read it now so the
    // first imported frame has a layout even if the stream-changed event is late.
    ReadOutputDescription();
    {
      std::lock_guard<std::mutex> lock(outputMutex_);
      handOffStop_ = false;
    }
    try {
      handOffThread_ = std::thread([this]() { HandOffLoop(); });
    } catch (...) {
      SetError("hand-off thread failed");
      Stop();
      return false;
    }
  }
  return true;
}

int32_t VideoDecoder::ConfigureCodec(int width, int height, int frameRate) {
  OH_AVFormat* format = OH_AVFormat_Create();
  if (format == nullptr) {
    SetError("OH_AVFormat_Create failed");
    return AV_ERR_NO_MEMORY;
  }
  // Only width/height are required; they preallocate for the stream (the real
  // picture size still comes from onStreamChanged).
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
  // Ask for the low-latency path when the platform has it (silently ignored
  // otherwise, per the SDK).
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
  if (buffered_) {
    // Buffer mode: name the channel layout the import addresses byte-wise (interleaved 4:2:0) rather
    // than finding out afterwards which one the codec picked.
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
  }
  if (frameRate > 0) {
    // The SDK declares the key as double while the decoder examples pass an int; the setter
    // verifies the type, so take whichever this platform accepts.
    if (!OH_AVFormat_SetDoubleValue(format, OH_MD_KEY_FRAME_RATE, static_cast<double>(frameRate))) {
      OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE, frameRate);
    }
  }
  const OH_AVErrCode configured = OH_VideoDecoder_Configure(codec_, format);
  OH_AVFormat_Destroy(format);
  return static_cast<int32_t>(configured);
}

bool VideoDecoder::TakePresentSlot() {
  if (slots_.Take()) {
    return true;
  }
  statPresentDropped_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

bool VideoDecoder::Submit(const uint8_t* data, size_t size, int64_t ptsUs) {
  if (!started_.load() || data == nullptr || size == 0) {
    return false;
  }
  {
    const int64_t nowUs = NowUs();
    std::lock_guard<std::mutex> lock(mutex_);
    Chunk chunk;
    chunk.data.assign(data, data + size);
    chunk.ptsUs = ptsUs;
    chunk.enqueuedUs = nowUs;
    pending_.push_back(std::move(chunk));
  }
  PumpInput();
  return true;
}

int64_t VideoDecoder::pendingAgeUs() const {
  const int64_t nowUs = NowUs();
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_.empty()) {
    return 0;
  }
  const int64_t age = nowUs - pending_.front().enqueuedUs;
  return age > 0 ? age : 0;
}

size_t VideoDecoder::DropPending() {
  std::lock_guard<std::mutex> lock(mutex_);
  const size_t dropped = pending_.size();
  pending_.clear();
  return dropped;
}

void VideoDecoder::Stop() {
  // The hand-off thread is holding a codec buffer and calling into the sink; it must be gone (and its
  // pictures returned) before the codec releases them.
  {
    std::lock_guard<std::mutex> lock(outputMutex_);
    handOffStop_ = true;
    outputCv_.notify_all();
  }
  if (handOffThread_.joinable()) {
    handOffThread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(outputMutex_);
    handOffStop_ = false;
  }
  sink_ = nullptr;
  ClearOutputs();
  if (codec_ == nullptr) {
    buffered_ = false;
    return;
  }
  started_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    idleInputs_.clear();
  }
  OH_VideoDecoder_Stop(codec_);
  OH_VideoDecoder_Destroy(codec_);
  codec_ = nullptr;
  buffered_ = false;
  const uint64_t dropped = statPresentDropped_.exchange(0);
  const uint64_t replaced = statHandOffReplaced_.exchange(0);
  if (dropped > 0 || replaced > 0) {
    HMRDP_LOGI("video decoder: %{public}llu frames outran the display, %{public}llu were replaced "
               "before the sink took them",
               static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(replaced));
  }
}

void VideoDecoder::ReleaseOutput(uint32_t index) {
  if (codec_ != nullptr) {
    OH_VideoDecoder_FreeOutputBuffer(codec_, index);
  }
}

void VideoDecoder::HandOff(uint32_t index, OH_AVBuffer* buffer) {
  OutputFrame dropped;
  {
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (handOffStop_) {
      dropped.index = index;
      dropped.buffer = buffer;
    } else {
      // The sink is behind: keep the newest picture only, so an overload costs one frame instead of
      // a queue's worth of latency.
      dropped = pendingOutput_;
      if (dropped.buffer != nullptr) {
        statHandOffReplaced_.fetch_add(1, std::memory_order_relaxed);
      }
      pendingOutput_.index = index;
      pendingOutput_.buffer = buffer;
      outputCv_.notify_one();
    }
  }
  if (dropped.buffer != nullptr) {
    ReleaseOutput(dropped.index);
  }
}

void VideoDecoder::HandOffLoop() {
  const auto releasePending = [this](PendingPresented& pending) {
    if (pending.native != nullptr) {
      OH_NativeBuffer_Unreference(static_cast<OH_NativeBuffer*>(pending.native));
    }
    ReleaseOutput(pending.index);
  };
  for (;;) {
    OutputFrame frame;
    VideoFrameGeometry geometry;
    {
      std::unique_lock<std::mutex> lock(outputMutex_);
      outputCv_.wait(lock, [this]() { return handOffStop_ || pendingOutput_.buffer != nullptr; });
      if (handOffStop_) {
        break;
      }
      frame = pendingOutput_;
      pendingOutput_ = OutputFrame();
      geometry = geometry_;
    }
    if (frame.buffer == nullptr) {
      continue;
    }
    // Keep at most two pictures in the sink; the oldest one's GPU work is finished before the
    // buffer goes back to the codec, so decode and present overlap.
    if (pendingPresented_.size() >= kMaxPendingPresented) {
      PendingPresented oldest = pendingPresented_.front();
      pendingPresented_.pop_front();
      if (sink_ != nullptr) {
        sink_->WaitVideoFrame(oldest.token);
      }
      releasePending(oldest);
    }
    OH_NativeBuffer* native = OH_AVBuffer_GetNativeBuffer(frame.buffer);
    VideoFrameSink* sink = sink_;
    int token = -1;
    if (native != nullptr && sink != nullptr) {
      token = sink->PresentVideoFrame(native, geometry);
    } else if (native == nullptr && !nativeBufferWarned_) {
      nativeBufferWarned_ = true;
      HMRDP_LOGW("video decoder: output buffer has no OH_NativeBuffer; dropping frames");
    }
    if (token >= 0 && native != nullptr) {
      PendingPresented pending;
      pending.index = frame.index;
      pending.native = native;
      pending.token = token;
      pendingPresented_.push_back(pending);
    } else {
      if (native != nullptr) {
        OH_NativeBuffer_Unreference(native);
      }
      ReleaseOutput(frame.index);
    }
  }
  // The sink is still attached while Stop joins this thread, so the fences can be waited here.
  for (PendingPresented& pending : pendingPresented_) {
    if (sink_ != nullptr) {
      sink_->WaitVideoFrame(pending.token);
    }
    releasePending(pending);
  }
  pendingPresented_.clear();
}

void VideoDecoder::ClearOutputs() {
  OutputFrame held;
  {
    std::lock_guard<std::mutex> lock(outputMutex_);
    held = pendingOutput_;
    pendingOutput_ = OutputFrame();
  }
  if (held.buffer != nullptr) {
    ReleaseOutput(held.index);
  }
  // The hand-off thread drains these before exiting; this is the belt-and-braces path.
  for (PendingPresented& pending : pendingPresented_) {
    if (pending.native != nullptr) {
      OH_NativeBuffer_Unreference(static_cast<OH_NativeBuffer*>(pending.native));
    }
    ReleaseOutput(pending.index);
  }
  pendingPresented_.clear();
}

void VideoDecoder::PumpInput() {
  // Collect what to push under the lock, then call the codec without it: the
  // callbacks take the same mutex, so a codec call under it could deadlock.
  struct Feed {
    uint32_t index = 0;
    OH_AVBuffer* buffer = nullptr;
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
  };
  std::vector<Feed> feeds;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!idleInputs_.empty() && !pending_.empty()) {
      Feed feed;
      feed.index = idleInputs_.front().first;
      feed.buffer = idleInputs_.front().second;
      Chunk chunk = std::move(pending_.front());
      pending_.pop_front();
      feed.data = std::move(chunk.data);
      feed.ptsUs = chunk.ptsUs;
      idleInputs_.pop_front();
      feeds.push_back(std::move(feed));
    }
  }

  for (Feed& feed : feeds) {
    if (feed.buffer == nullptr) {
      continue;
    }
    const int32_t capacity = OH_AVBuffer_GetCapacity(feed.buffer);
    if (capacity < 0 || static_cast<size_t>(capacity) < feed.data.size()) {
      SetError("input chunk larger than the codec buffer");
      continue;
    }
    uint8_t* address = OH_AVBuffer_GetAddr(feed.buffer);
    if (address == nullptr) {
      continue;
    }
    std::memcpy(address, feed.data.data(), feed.data.size());
    OH_AVCodecBufferAttr attr = {};
    attr.offset = 0;
    attr.size = static_cast<int32_t>(feed.data.size());
    attr.pts = feed.ptsUs;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(feed.buffer, &attr);
    OH_VideoDecoder_PushInputBuffer(codec_, feed.index);
  }
}

void VideoDecoder::OnError(OH_AVCodec*, int32_t errorCode, void* userData) {
  VideoDecoder* self = static_cast<VideoDecoder*>(userData);
  if (self != nullptr) {
    self->SetError("codec error " + std::to_string(errorCode));
  }
}

void VideoDecoder::OnStreamChanged(OH_AVCodec*, OH_AVFormat* format, void* userData) {
  VideoDecoder* self = static_cast<VideoDecoder*>(userData);
  if (self != nullptr) {
    self->UpdateGeometryFromFormat(format);
  }
}

void VideoDecoder::UpdateGeometryFromFormat(OH_AVFormat* format) {
  if (format == nullptr) {
    return;
  }
  // The visible (cropped) size is the picture, not the coded size the configure
  // hint carried; fall back to the plain dimensions when it is absent.
  int32_t width = 0;
  int32_t height = 0;
  if (!OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_WIDTH, &width) ||
      width <= 0) {
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &width);
  }
  if (!OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_HEIGHT, &height) ||
      height <= 0) {
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &height);
  }
  if (width <= 0 || height <= 0) {
    return;
  }
  width_.store(width);
  height_.store(height);
  // Buffer mode adds the frame's layout to the same description: the byte row pitch, the rows
  // allocated per plane (the chroma plane starts at their product) and the crop rectangle. That is
  // exactly what a GPU import addresses, and it is only obtainable in this mode — an output surface
  // hides the layout behind its own allocation.
  int32_t stride = 0;
  int32_t sliceHeight = 0;
  int32_t cropLeft = 0;
  int32_t cropTop = 0;
  int32_t cropRight = 0;
  int32_t cropBottom = 0;
  int32_t pixelFormat = 0;
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_STRIDE, &stride);
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &sliceHeight);
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_LEFT, &cropLeft);
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_TOP, &cropTop);
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_RIGHT, &cropRight);
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_CROP_BOTTOM, &cropBottom);
  OH_AVFormat_GetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, &pixelFormat);
  {
    std::lock_guard<std::mutex> lock(outputMutex_);
    VideoFrameGeometry& geometry = geometry_;
    geometry = VideoFrameGeometry();
    geometry.stride = stride > 0 ? stride : 0;
    geometry.sliceHeight = sliceHeight > 0 ? sliceHeight : 0;
    // The crop rectangle is inclusive; an all-zero or full-frame rectangle means "no crop".
    const bool cropped = cropRight > cropLeft && cropBottom > cropTop && cropRight < width &&
                         cropBottom < height;
    geometry.visibleWidth = cropped ? cropRight - cropLeft + 1 : width;
    geometry.visibleHeight = cropped ? cropBottom - cropTop + 1 : height;
    geometry.originX = cropped ? cropLeft : 0;
    geometry.originY = cropped ? cropTop : 0;
    geometry.pixelFormat = pixelFormat;
    geometry.cbFirst = pixelFormat != AV_PIXEL_FORMAT_NV21;
  }
  HMRDP_LOGI("video decoder: stream %{public}dx%{public}d stride=%{public}d slice=%{public}d "
             "crop=%{public}d,%{public}d-%{public}d,%{public}d pixel=%{public}d cbFirst=%{public}d",
             width, height, stride, sliceHeight, cropLeft, cropTop, cropRight, cropBottom,
             pixelFormat, pixelFormat != AV_PIXEL_FORMAT_NV21 ? 1 : 0);
}

void VideoDecoder::ReadOutputDescription() {
  if (codec_ == nullptr) {
    return;
  }
  OH_AVFormat* description = OH_VideoDecoder_GetOutputDescription(codec_);
  if (description == nullptr) {
    return;
  }
  UpdateGeometryFromFormat(description);
  OH_AVFormat_Destroy(description);
}

void VideoDecoder::OnNeedInput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData) {
  VideoDecoder* self = static_cast<VideoDecoder*>(userData);
  if (self == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->idleInputs_.push_back({index, buffer});
  }
  self->PumpInput();
}

void VideoDecoder::HandleNewOutput(uint32_t index, OH_AVBuffer* buffer) {
  OH_AVCodecBufferAttr attr = {};
  if (buffer != nullptr) {
    OH_AVBuffer_GetBufferAttr(buffer, &attr);
  }
  if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
    ReleaseOutput(index);
    return;
  }
  const int width = width_.load();
  const int height = height_.load();
  // Present as soon as the frame is decoded (the host paces the stream); a frame the display cannot
  // show this period is released unwatched — this is the one place that decides it, for both output
  // modes. In buffer mode "presenting" means handing the picture to the sink, which returns it.
  if (TakePresentSlot()) {
    statPresented_.fetch_add(1, std::memory_order_relaxed);
    if (buffered_) {
      HandOff(index, buffer);
    } else {
      OH_VideoDecoder_RenderOutputBuffer(codec_, index);
    }
  } else {
    ReleaseOutput(index);
  }
  if (onFrame_) {
    onFrame_(width, height, attr.pts);
  }
}

void VideoDecoder::OnNewOutput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData) {
  VideoDecoder* self = static_cast<VideoDecoder*>(userData);
  if (self != nullptr) {
    self->HandleNewOutput(index, buffer);
  }
}

}  // namespace hmrdp
