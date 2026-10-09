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

// OHOS handles, kept as forward declarations so the header stays free of the
// media SDK headers.
struct OH_AVCodec;
struct OH_AVBuffer;
struct OH_AVFormat;

namespace hmrdp {

enum class VideoOutputMode {
  kSurface = 0,
  kBuffer = 1,
};

// One decoded frame as seen by the sink. In kBuffer mode `buffer` is the
// OH_AVBuffer* and is only valid for the duration of the callback; the sink must
// import/copy before it returns.
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

class VideoDecoder {
 public:
  // kSurface: called after the frame has been rendered into the window; the sink
  // samples it. kBuffer: called with the decoded buffer before it is released.
  using FrameReadyFn = std::function<void(int width, int height, int64_t ptsUs)>;
  using BufferReadyFn = std::function<void(const DecodedFrame& frame)>;

  VideoDecoder() = default;
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // `window` is required in kSurface mode and ignored in kBuffer mode. `fps` is
  // display pacing in surface mode; `onFrame`/`onBuffer` is the sink.
  bool Start(const std::string& mime, VideoOutputMode mode, void* window, int width, int height,
             int fps, FrameReadyFn onFrame, BufferReadyFn onBuffer);
  // Feeds one complete access unit (Annex-B, leading start code included): every
  // slice of a picture must arrive in this one call, and the call may carry
  // several NALs.
  bool Submit(const uint8_t* data, size_t size, int64_t ptsUs);
  void SignalEos();
  void Stop();

  int width() const { return width_.load(); }
  int height() const { return height_.load(); }
  bool eosReached() const { return eosReached_.load(); }
  size_t pendingBytes() const { return pendingBytes_.load(); }
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

  struct Chunk {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
  };

  OH_AVCodec* codec_ = nullptr;
  VideoOutputMode mode_ = VideoOutputMode::kSurface;
  void* window_ = nullptr;
  FrameReadyFn onFrame_;
  BufferReadyFn onBuffer_;
  // Surface mode display pacing: RenderOutputBufferAtTime is fed a steady_clock
  // schedule derived from fps_, so the decoder controls how fast frames appear
  // instead of the app blocking on a present. Per attempt.
  int fps_ = 0;
  int64_t renderBaseNs_ = 0;
  uint32_t frameIndex_ = 0;

  // `pending_` is the input still to be fed; `idleInputs_` the indices the
  // decoder has offered. PumpInput() matches them.
  std::mutex mutex_;
  std::deque<Chunk> pending_;
  // (index, buffer) the decoder has offered and not been given data for yet.
  std::deque<std::pair<uint32_t, OH_AVBuffer*>> idleInputs_;
  bool eosQueued_ = false;
  bool eosPushed_ = false;
  std::atomic<size_t> pendingBytes_{0};

  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
  std::atomic<bool> eosReached_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> hardware_{false};

  mutable std::mutex errorMutex_;
  std::string error_;
};

}  // namespace hmrdp

#endif  // HMRDP_VIDEO_DECODER_H
