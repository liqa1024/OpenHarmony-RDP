#ifndef HMRDP_VIDEO_CONSUMER_H
#define HMRDP_VIDEO_CONSUMER_H

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

// Forward-declared so this header stays free of the graphics SDK headers. The SDK spells these
// types with typedefs whose struct tags are the names below.
struct OH_NativeImage;
struct OH_NativeBuffer;
struct NativeWindow;
struct NativeWindowBuffer;

namespace hmrdp {

// Where a consumed frame goes. Implemented by the decode bridge, which owns the presenter's
// lifetime: the sink outlives the consumer, and the presenter call is guarded there (so tearing the
// session down cannot pull the presenter out from under a frame in flight).
class VideoFrameSink {
 public:
  virtual ~VideoFrameSink() = default;
  // One decoded frame; `buffer` is borrowed for the call only. `width`/`height` are the stream
  // (session) resolution the picture is presented at.
  virtual void PresentVideoFrame(const void* buffer, int width, int height) = 0;
};

// Moonlight 超分: the decoder renders into this offscreen consumer surface (surface mode), and this
// class drains it into the sink.
//
// The shape follows the official Vulkan External Memory guide: the frame-available callback only
// signals, a dedicated thread does the work (acquire the buffer, read its transform, hand it to the
// sink, return it), and the previously held buffer is returned when the next frame arrives
// (double-buffered return) so returning a buffer never waits on the frame being presented.
//
// The decoder already drops frames the panel cannot show (VideoDecoder::TakePresentSlot), so this
// thread presents what it is given — one gate, at the cheapest place.
class VideoFrameConsumer {
 public:
  VideoFrameConsumer() = default;
  ~VideoFrameConsumer();

  VideoFrameConsumer(const VideoFrameConsumer&) = delete;
  VideoFrameConsumer& operator=(const VideoFrameConsumer&) = delete;

  // Creates the consumer surface and starts the drain thread. `decodedWidth`/`decodedHeight` are the
  // stream (session) resolution the frames are presented at.
  bool Start(int decodedWidth, int decodedHeight, VideoFrameSink* sink);
  void Stop();

  // The producer-side window to hand to the decoder (OH_VideoDecoder_SetSurface); null before Start.
  void* decodeWindow() const { return window_; }

  // Frames consumed / frames whose acquire found nothing, for the session log.
  uint64_t framesPresented() const { return framesPresented_; }
  uint64_t framesMissed() const { return framesMissed_; }

 private:
  static void OnFrameAvailableThunk(void* context);
  // Runs on the consumer surface's own thread; only signals the drain thread.
  void OnFrameAvailable();
  void DrainLoop();
  // One frame: acquire -> transform -> sink -> keep the buffer for the next round.
  void PresentOne(VideoFrameSink* sink, int width, int height);
  // Returns the buffer kept from the previous frame (pairs its acquire with a release).
  void ReleaseHeld();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  bool running_ = false;
  int pending_ = 0;

  OH_NativeImage* image_ = nullptr;
  NativeWindow* window_ = nullptr;
  VideoFrameSink* sink_ = nullptr;
  int width_ = 0;
  int height_ = 0;

  // Held between frames (see ReleaseHeld); accessed by the drain thread only.
  NativeWindowBuffer* held_ = nullptr;
  int heldFenceFd_ = -1;

  uint64_t framesPresented_ = 0;
  uint64_t framesMissed_ = 0;
  bool geometryLogged_ = false;
};

}

#endif
