/*
 * HmRdp - HarmonyOS AVCodec video decoder for the Moonlight stream.
 *
 * One instance wraps one hardware decoder. Decoded frames leave it either as a
 * surface (it renders into a window the presenter owns) or as buffers (the sink
 * imports them); the presenter backend picks which, and the rationale for the
 * split is in doc_agent/moonlight-native.md §2.
 */
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
// media SDK headers (only the .cpp needs them).
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
// import/copy before it returns (the decoder frees it right after).
struct DecodedFrame {
  void* buffer = nullptr;
  int width = 0;
  int height = 0;
  int64_t ptsUs = 0;
};

// Best decoder the platform offers for `mime`: hardware preferred, then
// software, then none.
enum class DecoderSupport {
  kNone = 0,
  kSoftware = 1,
  kHardware = 2,
};
DecoderSupport VideoDecoderSupport(const std::string& mime);
// Whether the platform has any decoder for `mime` (hardware or software).
bool VideoDecoderAvailable(const std::string& mime);
// Short description of the decoder that would run ("hw:<name>" / "sw:<name>"), or
// an empty string when there is none.
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

  // `mime` is one of OH_AVCODEC_MIMETYPE_VIDEO_* ("video/avc" / "video/hevc").
  // `window` is required in kSurface mode and ignored in kBuffer mode (owned by
  // the caller). `width`/`height` are the buffer-preallocation hint (the stream
  // resolution, or a little above; the real picture size is reported per frame).
  // `fps` is display pacing in surface mode; `onFrame`/`onBuffer` is the sink.
  bool Start(const std::string& mime, VideoOutputMode mode, void* window, int width, int height,
             int fps, FrameReadyFn onFrame, BufferReadyFn onBuffer);
  // Feeds one complete access unit (Annex-B, leading start code included): every
  // slice of a picture must arrive in this one call (AVCodec requires it) and the
  // call may carry several NALs (e.g. SPS/PPS/SEI before the frame). See
  // doc_agent/moonlight-native.md §2.
  bool Submit(const uint8_t* data, size_t size, int64_t ptsUs);
  // No more input: the decoder drains its remaining frames, then stays idle.
  void SignalEos();
  // Stops and destroys the decoder. Idempotent.
  void Stop();

  int width() const { return width_.load(); }
  int height() const { return height_.load(); }
  bool eosReached() const { return eosReached_.load(); }
  // Bytes submitted but not yet handed to the codec, for input backpressure.
  size_t pendingBytes() const { return pendingBytes_.load(); }
  std::string lastError() const;
  // Whether the running decoder is the hardware one (false also before Start).
  bool hardware() const { return hardware_.load(); }

 private:
  static void OnError(OH_AVCodec*, int32_t errorCode, void* userData);
  static void OnStreamChanged(OH_AVCodec*, OH_AVFormat* format, void* userData);
  static void OnNeedInput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData);
  static void OnNewOutput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData);

  void HandleNewOutput(uint32_t index, OH_AVBuffer* buffer);
  // Pushes as many queued chunks as there are idle input buffers, without
  // holding the lock across the codec calls. Caller must not hold the lock.
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
  // Surface mode display pacing (see HandleNewOutput): RenderOutputBufferAtTime is
  // fed a steady_clock schedule derived from fps_, so the decoder controls how
  // fast frames appear instead of the app blocking on a present. Per attempt.
  int fps_ = 0;
  int64_t renderBaseNs_ = 0;
  uint32_t frameIndex_ = 0;

  // Only the callback-side state that the async input model needs. `pending_` is
  // the input still to be fed and `idleInputs_` the indices the decoder has
  // offered; PumpInput() matches them.
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
