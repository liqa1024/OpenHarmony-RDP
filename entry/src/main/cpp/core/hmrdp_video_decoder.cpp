#include "hmrdp_video_decoder.h"

#include <chrono>
#include <cstring>
#include <utility>

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

bool VideoDecoder::Start(const std::string& mime, VideoOutputMode mode, void* window, int width,
                         int height, int frameRate, FrameReadyFn onFrame, BufferReadyFn onBuffer) {
  if (codec_ != nullptr) {
    return started_.load();
  }
  if (mode == VideoOutputMode::kSurface && window == nullptr) {
    SetError("surface mode needs a decode window");
    return false;
  }
  mode_ = mode;
  window_ = window;
  onFrame_ = std::move(onFrame);
  onBuffer_ = std::move(onBuffer);
  eosReached_.store(false);
  inputBudgetUs_ = BudgetForFrameRate(frameRate);

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
  hardware_.store(picked.hardware);

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

  if (mode_ == VideoOutputMode::kSurface) {
    if (OH_VideoDecoder_SetSurface(codec_, reinterpret_cast<OHNativeWindow*>(window_)) != AV_ERR_OK) {
      SetError("SetSurface failed");
      Stop();
      return false;
    }
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
  if (mode_ == VideoOutputMode::kBuffer) {
    // Surface mode converts to the window's format itself; only buffer mode picks
    // a pixel format, and NV12 is what the Vulkan importer expects.
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
    if (eosQueued_) {
      return false;
    }
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

void VideoDecoder::SignalEos() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    eosQueued_ = true;
  }
  PumpInput();
}

void VideoDecoder::Stop() {
  if (codec_ == nullptr) {
    return;
  }
  started_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    idleInputs_.clear();
    eosQueued_ = false;
    eosPushed_ = false;
  }
  OH_VideoDecoder_Stop(codec_);
  OH_VideoDecoder_Destroy(codec_);
  codec_ = nullptr;
  const uint64_t dropped = statPresentDropped_.exchange(0);
  if (dropped > 0) {
    HMRDP_LOGI("video decoder: %{public}llu frames outran the display and were dropped",
               static_cast<unsigned long long>(dropped));
  }
}

void VideoDecoder::PumpInput() {
  // Collect what to push under the lock, then call the codec without it: the
  // callbacks take the same mutex, so a codec call under it could deadlock.
  struct Feed {
    uint32_t index = 0;
    OH_AVBuffer* buffer = nullptr;
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    bool eos = false;
  };
  std::vector<Feed> feeds;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!idleInputs_.empty()) {
      if (!pending_.empty()) {
        Feed feed;
        feed.index = idleInputs_.front().first;
        feed.buffer = idleInputs_.front().second;
        Chunk chunk = std::move(pending_.front());
        pending_.pop_front();
        feed.data = std::move(chunk.data);
        feed.ptsUs = chunk.ptsUs;
        idleInputs_.pop_front();
        feeds.push_back(std::move(feed));
      } else if (eosQueued_ && !eosPushed_) {
        Feed feed;
        feed.index = idleInputs_.front().first;
        feed.buffer = idleInputs_.front().second;
        feed.eos = true;
        idleInputs_.pop_front();
        eosPushed_ = true;
        feeds.push_back(std::move(feed));
      } else {
        break;
      }
    }
  }

  for (Feed& feed : feeds) {
    if (feed.buffer == nullptr) {
      continue;
    }
    if (feed.eos) {
      OH_AVCodecBufferAttr attr = {};
      attr.offset = 0;
      attr.size = 0;
      attr.pts = 0;
      attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
      OH_AVBuffer_SetBufferAttr(feed.buffer, &attr);
      OH_VideoDecoder_PushInputBuffer(codec_, feed.index);
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
  if (self == nullptr || format == nullptr) {
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
  if (width > 0 && height > 0) {
    self->width_.store(width);
    self->height_.store(height);
    HMRDP_LOGI("video decoder: stream %{public}dx%{public}d", width, height);
  }
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
    eosReached_.store(true);
    OH_VideoDecoder_FreeOutputBuffer(codec_, index);
    return;
  }
  const int width = width_.load();
  const int height = height_.load();
  if (mode_ == VideoOutputMode::kSurface) {
    // Present as soon as the frame is decoded (the host paces the stream); a frame the display
    // cannot show this period is released unwatched.
    if (TakePresentSlot()) {
      OH_VideoDecoder_RenderOutputBuffer(codec_, index);
    } else {
      OH_VideoDecoder_FreeOutputBuffer(codec_, index);
    }
    if (onFrame_) {
      onFrame_(width, height, attr.pts);
    }
  } else {
    if (onBuffer_) {
      DecodedFrame frame;
      frame.buffer = buffer;
      frame.width = width;
      frame.height = height;
      frame.ptsUs = attr.pts;
      onBuffer_(frame);
    }
    OH_VideoDecoder_FreeOutputBuffer(codec_, index);
  }
}

void VideoDecoder::OnNewOutput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData) {
  VideoDecoder* self = static_cast<VideoDecoder*>(userData);
  if (self != nullptr) {
    self->HandleNewOutput(index, buffer);
  }
}

}  // namespace hmrdp
