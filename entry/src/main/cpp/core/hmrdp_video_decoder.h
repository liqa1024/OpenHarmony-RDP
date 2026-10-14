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

class VideoDecoder {
 public:
  // kSurface: after the frame is rendered into the window; kBuffer: with the decoded buffer before release.
  using FrameReadyFn = std::function<void(int width, int height, int64_t ptsUs)>;
  using BufferReadyFn = std::function<void(const DecodedFrame& frame)>;

  VideoDecoder() = default;
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // `window` is required in kSurface mode.
  bool Start(const std::string& mime, VideoOutputMode mode, void* window, int width, int height,
             FrameReadyFn onFrame, BufferReadyFn onBuffer);
  // Feeds one complete access unit (Annex-B, leading start code included): every slice of a picture
  // must arrive in this one call, which may carry several NALs.
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

  // `pending_` is input to feed, `idleInputs_` the indices offered; PumpInput() matches them.
  std::mutex mutex_;
  std::deque<Chunk> pending_;
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

}

#endif
