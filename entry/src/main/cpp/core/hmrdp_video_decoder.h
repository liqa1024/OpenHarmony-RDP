#ifndef HMRDP_VIDEO_DECODER_H
#define HMRDP_VIDEO_DECODER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hmrdp_display_rate.h"
#include "hmrdp_video_frame.h"

// Forward-declared so this header stays free of the media SDK headers.
struct OH_AVCodec;
struct OH_AVBuffer;
struct OH_AVFormat;

namespace hmrdp {

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
  // One decoded picture, after it has left the codec. The picture itself is only handed out in
  // buffer mode (see StartBuffered); surface mode renders it into the window.
  using FrameReadyFn = std::function<void(int width, int height, int64_t ptsUs)>;

  VideoDecoder() = default;
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  // Surface mode: the codec writes every picture into `window` itself; the decode bridge only gates
  // (render or release) and never sees the pixels.
  // `window` is the decode output surface (required). `frameRate` is the negotiated stream rate: it
  // configures the decoder and sizes the input budget; 0 means unknown.
  bool Start(const std::string& mime, void* window, int width, int height, int frameRate,
             FrameReadyFn onFrame);

  // Buffer mode: no output surface; the codec hands out its own output buffers (`OH_AVBuffer` ->
  // `OH_AVBuffer_GetNativeBuffer`) and reports the frame layout in OnStreamChanged, which is what a
  // GPU import needs. Every picture the display can still show is handed to `sink` on a hand-off
  // thread; the buffer goes back to the codec as soon as the sink returns. At most one picture waits
  // for the hand-off, so an overloaded sink costs the *oldest* frame, not latency growth.
  bool StartBuffered(const std::string& mime, int width, int height, int frameRate,
                     FrameReadyFn onFrame, VideoFrameSink* sink);

  // Feeds one complete access unit (Annex-B, leading start code included): every slice of a picture
  // must arrive in this one call, which may carry several NALs.
  bool Submit(const uint8_t* data, size_t size, int64_t ptsUs);
  void Stop();

  // How long the oldest access unit that has not reached the codec yet has been waiting.
  int64_t pendingAgeUs() const;
  // Age past which a queued access unit is too late to be worth decoding.
  int64_t inputBudgetUs() const { return inputBudgetUs_; }
  // Drops every access unit still waiting for an input buffer. The dropped units break the
  // prediction chain, so the caller must force a keyframe before decoding can resume.
  size_t DropPending();
  std::string lastError() const;

 private:
  // One decoded picture waiting for the hand-off thread.
  struct OutputFrame {
    uint32_t index = 0;
    OH_AVBuffer* buffer = nullptr;
  };

  static void OnError(OH_AVCodec*, int32_t errorCode, void* userData);
  static void OnStreamChanged(OH_AVCodec*, OH_AVFormat* format, void* userData);
  static void OnNeedInput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData);
  static void OnNewOutput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData);

  bool StartInternal(const std::string& mime, void* window, int width, int height, int frameRate,
                     FrameReadyFn onFrame, VideoFrameSink* sink);
  void HandleNewOutput(uint32_t index, OH_AVBuffer* buffer);
  // Caller must not hold the lock (the codec calls below take it).
  void PumpInput();
  void SetError(const std::string& message);
  // Builds the Configure format and configures the codec. Returns an OH_AVErrCode (kept as int so
  // this header stays free of the media SDK headers).
  int32_t ConfigureCodec(int width, int height, int frameRate);
  // False when the display cannot show this frame before its next refresh.
  bool TakePresentSlot();
  // Hands one picture back to the codec; the one place a buffer is returned.
  void ReleaseOutput(uint32_t index);
  // Queues one picture for the hand-off thread, dropping (and returning) the one still waiting.
  void HandOff(uint32_t index, OH_AVBuffer* buffer);
  // The hand-off thread: sink -> return the buffer to the codec.
  void HandOffLoop();
  // Returns every picture still held for the hand-off thread; the caller must have stopped it.
  void ClearOutputs();

  struct Chunk {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    int64_t enqueuedUs = 0;
  };

  OH_AVCodec* codec_ = nullptr;
  FrameReadyFn onFrame_;
  int64_t inputBudgetUs_ = 0;

  // `pending_` is input to feed, `idleInputs_` the indices offered; PumpInput() matches them.
  // The thread that feeds and the thread that drains are different, so both take the mutex.
  mutable std::mutex mutex_;
  std::deque<Chunk> pending_;
  std::deque<std::pair<uint32_t, OH_AVBuffer*>> idleInputs_;

  PresentSlots slots_;
  std::atomic<uint64_t> statPresentDropped_{0};
  // Pictures replaced in the hand-off before the sink took them (the sink is behind the stream).
  std::atomic<uint64_t> statHandOffReplaced_{0};
  // One-shot: a buffer with no OH_NativeBuffer behind it would otherwise log at the stream rate.
  bool nativeBufferWarned_ = false;

  // Buffer mode hand-off. The codec's callback thread only queues or releases a picture; the sink
  // runs on `handOffThread_`, and the picture returns to the codec when it is done with it. The
  // layout the frames are handed off with is written once per stream change, so it lives here too.
  VideoFrameSink* sink_ = nullptr;
  bool buffered_ = false;
  std::thread handOffThread_;
  std::mutex outputMutex_;
  std::condition_variable outputCv_;
  // A picture waiting for the hand-off; an empty one means there is nothing to hand off.
  OutputFrame pendingOutput_;
  bool handOffStop_ = false;
  VideoFrameGeometry geometry_;

  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
  std::atomic<bool> started_{false};

  mutable std::mutex errorMutex_;
  std::string error_;
};

}

#endif
