#ifndef HMRDP_VIDEO_DECODER_H
#define HMRDP_VIDEO_DECODER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Forward-declared so this header stays free of the media SDK headers.
struct OH_AVCodec;
struct OH_AVBuffer;
struct OH_AVFormat;

namespace hmrdp {

enum class VideoOutputMode {
  kSurface = 0,
  kBuffer = 1,
};

// In kBuffer mode `buffer` is the OH_AVBuffer* and is valid only for the callback; the sink must
// import/copy before returning.
struct DecodedFrame {
  void* buffer = nullptr;
  int width = 0;
  int height = 0;
  int64_t ptsUs = 0;
};

enum class DecoderSupport {
  kNone = 0,
  kSoftware = 1,
  kHardware = 2,
};
DecoderSupport VideoDecoderSupport(const std::string& mime);
bool VideoDecoderAvailable(const std::string& mime);
std::string VideoDecoderDescription(const std::string& mime);
// Highest frame rate this decoder reports for a picture size, 0 when the platform states none.
// Frames are limited by macroblocks (H.264) / luma samples (HEVC) per second, so a size can be
// decodable and still fall short of the requested rate.
int VideoDecoderMaxFps(const std::string& mime, int width, int height);

class VideoDecoder {
 public:
  // kSurface: after the frame is rendered into the window; kBuffer: with the decoded buffer before release.
  using FrameReadyFn = std::function<void(int width, int height, int64_t ptsUs)>;
  using BufferReadyFn = std::function<void(const DecodedFrame& frame)>;

  VideoDecoder() = default;
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // `window` is required in kSurface mode. `frameRate` is the negotiated stream rate: it configures
  // the decoder (and the video variable refresh rate path, surface mode only) and sizes the input
  // budget; 0 means unknown.
  bool Start(const std::string& mime, VideoOutputMode mode, void* window, int width, int height,
             int frameRate, FrameReadyFn onFrame, BufferReadyFn onBuffer);
  // Feeds one complete access unit (Annex-B, leading start code included): every slice of a picture
  // must arrive in this one call, which may carry several NALs.
  bool Submit(const uint8_t* data, size_t size, int64_t ptsUs);
  void SignalEos();
  void Stop();

  int width() const { return width_.load(); }
  int height() const { return height_.load(); }
  bool eosReached() const { return eosReached_.load(); }
  // How long the oldest access unit that has not reached the codec yet has been waiting.
  int64_t pendingAgeUs() const;
  // Age past which a queued access unit is too late to be worth decoding.
  int64_t inputBudgetUs() const { return inputBudgetUs_; }
  // Drops every access unit still waiting for an input buffer. The dropped units break the
  // prediction chain, so the caller must force a keyframe before decoding can resume.
  size_t DropPending();
  std::string lastError() const;
  bool hardware() const { return hardware_.load(); }

 private:
  static void OnError(OH_AVCodec*, int32_t errorCode, void* userData);
  static void OnStreamChanged(OH_AVCodec*, OH_AVFormat* format, void* userData);
  static void OnNeedInput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData);
  static void OnNewOutput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData);

  void HandleNewOutput(uint32_t index, OH_AVBuffer* buffer);
  // Caller must not hold the lock (the codec calls below take it).
  void PumpInput();
  void SetError(const std::string& message);
  // Builds the Configure format and configures the codec; `frameRate` > 0 also asks for the
  // frame-rate-tied keys (VRR in surface mode). Returns an OH_AVErrCode (kept as int so this
  // header stays free of the media SDK headers).
  int32_t ConfigureCodec(int width, int height, int frameRate);

  struct Chunk {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    int64_t enqueuedUs = 0;
  };

  OH_AVCodec* codec_ = nullptr;
  VideoOutputMode mode_ = VideoOutputMode::kSurface;
  void* window_ = nullptr;
  FrameReadyFn onFrame_;
  BufferReadyFn onBuffer_;
  int64_t inputBudgetUs_ = 0;

  // `pending_` is input to feed, `idleInputs_` the indices offered; PumpInput() matches them.
  // The thread that feeds and the thread that drains are different, so both take the mutex.
  mutable std::mutex mutex_;
  std::deque<Chunk> pending_;
  std::deque<std::pair<uint32_t, OH_AVBuffer*>> idleInputs_;
  bool eosQueued_ = false;
  bool eosPushed_ = false;

  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
  std::atomic<bool> eosReached_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> hardware_{false};

  mutable std::mutex errorMutex_;
  std::string error_;
};

}

#endif
