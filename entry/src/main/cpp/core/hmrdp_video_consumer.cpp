#include "hmrdp_video_consumer.h"

#include <poll.h>
#include <unistd.h>

#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>

#include "hmrdp_log.h"

namespace hmrdp {

namespace {

// A lost acquire fence must not stall the consumer; the frame is still sampled (the producer wrote
// it before the buffer became available).
constexpr int kFenceWaitMs = 20;

}

VideoFrameConsumer::~VideoFrameConsumer() {
  Stop();
}

bool VideoFrameConsumer::Start(int decodedWidth, int decodedHeight, VideoFrameSink* sink) {
  if (image_ != nullptr || sink == nullptr || decodedWidth <= 0 || decodedHeight <= 0) {
    return false;
  }
  // A consumer surface only rotates buffers; nothing here renders into it (the convert pass does).
  image_ = OH_ConsumerSurface_Create();
  if (image_ == nullptr) {
    HMRDP_LOGE("moonlight video: OH_ConsumerSurface_Create failed");
    return false;
  }
  // Declare how these buffers will be consumed *before* the producer requests any: the presenter
  // samples them with the GPU, and it reads them as a *linear byte image* (see hmrdp_vk_video). A
  // consumer that declares neither can be handed a layout shaped for another consumer — a compressed
  // or otherwise opaque one — that byte-wise sampling cannot describe.
  const uint64_t kConsumerUsage = NATIVEBUFFER_USAGE_HW_TEXTURE | NATIVEBUFFER_USAGE_MEM_DMA |
                                  NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_READ_OFTEN;
  if (OH_ConsumerSurface_SetDefaultUsage(image_, kConsumerUsage) != 0) {
    HMRDP_LOGW("moonlight video: consumer surface usage not declared; the import may get a "
               "non-texture buffer layout");
  }
  OH_OnFrameAvailableListener listener{};
  listener.context = this;
  listener.onFrameAvailable = &VideoFrameConsumer::OnFrameAvailableThunk;
  if (OH_NativeImage_SetOnFrameAvailableListener(image_, listener) != 0) {
    HMRDP_LOGE("moonlight video: SetOnFrameAvailableListener failed");
    Stop();
    return false;
  }
  window_ = OH_NativeImage_AcquireNativeWindow(image_);
  if (window_ == nullptr) {
    HMRDP_LOGE("moonlight video: AcquireNativeWindow failed");
    Stop();
    return false;
  }
  width_ = decodedWidth;
  height_ = decodedHeight;
  sink_ = sink;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
  }
  try {
    thread_ = std::thread([this]() { DrainLoop(); });
  } catch (...) {
    HMRDP_LOGE("moonlight video: consumer thread failed");
    Stop();
    return false;
  }
  HMRDP_LOGI("moonlight video: consumer surface ready (超分 imports its buffers)");
  return true;
}

void VideoFrameConsumer::Stop() {
  const bool started = image_ != nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    sink_ = nullptr;
    pending_ = 0;
    cv_.notify_all();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  // The drain thread is gone: nothing else touches the surface, so the held buffer can go back.
  ReleaseHeld();
  if (image_ != nullptr) {
    OH_NativeImage_UnsetOnFrameAvailableListener(image_);
    // The window is owned by the image; Destroy releases that reference.
    window_ = nullptr;
    OH_NativeImage_Destroy(&image_);
    image_ = nullptr;
  }
  if (started) {
    HMRDP_LOGI("moonlight video: consumer stopped (presented %{public}llu, missed %{public}llu)",
               static_cast<unsigned long long>(framesPresented_),
               static_cast<unsigned long long>(framesMissed_));
  }
}

void VideoFrameConsumer::OnFrameAvailableThunk(void* context) {
  if (context != nullptr) {
    static_cast<VideoFrameConsumer*>(context)->OnFrameAvailable();
  }
}

void VideoFrameConsumer::OnFrameAvailable() {
  // The callback's only job is to wake the drain thread (the module must not be re-entered here).
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    ++pending_;
  }
  cv_.notify_one();
}

void VideoFrameConsumer::DrainLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    cv_.wait(lock, [this]() { return !running_ || pending_ > 0; });
    if (!running_) {
      return;
    }
    while (pending_ > 0 && running_) {
      --pending_;
      VideoFrameSink* sink = sink_;
      const int width = width_;
      const int height = height_;
      lock.unlock();
      if (sink != nullptr) {
        PresentOne(sink, width, height);
      }
      lock.lock();
    }
  }
}

void VideoFrameConsumer::PresentOne(VideoFrameSink* sink, int width, int height) {
  OHNativeWindowBuffer* buffer = nullptr;
  int fenceFd = -1;
  if (OH_NativeImage_AcquireNativeWindowBuffer(image_, &buffer, &fenceFd) != 0 ||
      buffer == nullptr) {
    // NATIVE_ERROR_NO_BUFFER: the signal outran the buffer (or the producer gave it up first).
    ++framesMissed_;
    return;
  }
  // The acquire contract: hold a reference for as long as we use the buffer, and pair the acquire
  // with a release (Release closes the fence it is given, so the acquire's is passed along).
  OH_NativeWindow_NativeObjectReference(buffer);
  // The acquire fence signals when the producer finished writing; sampling before it fires would
  // read a half-written frame. Bounded, so a lost fence cannot stall the path.
  if (fenceFd >= 0) {
    struct pollfd waitFd = {};
    waitFd.fd = fenceFd;
    waitFd.events = POLLIN;
    ::poll(&waitFd, 1, kFenceWaitMs);
  }
  OH_NativeBuffer* native = nullptr;
  if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native) != 0 || native == nullptr) {
    OH_NativeWindow_NativeObjectUnreference(buffer);
    OH_NativeImage_ReleaseNativeWindowBuffer(image_, buffer, fenceFd);
    return;
  }
  if (!geometryLogged_) {
    // One-shot: the first frame's buffer is the one whose geometry explains a mis-import.
    geometryLogged_ = true;
    OH_NativeBuffer_Config config{};
    OH_NativeBuffer_GetConfig(native, &config);
    HMRDP_LOGI("moonlight video: frame buffer %{public}dx%{public}d stride=%{public}d "
               "fmt=%{public}d usage=0x%{public}x",
               config.width, config.height, config.stride, config.format, config.usage);
  }
  // Blocking: the sink is done with the buffer when this returns (it waits its own GPU fence), so
  // the buffer may be handed back below.
  sink->PresentVideoFrame(native, width, height);
  ++framesPresented_;
  // Return the previous frame now; this frame is kept until the next one arrives.
  ReleaseHeld();
  held_ = buffer;
  heldFenceFd_ = fenceFd;
}

void VideoFrameConsumer::ReleaseHeld() {
  if (held_ == nullptr) {
    return;
  }
  OH_NativeWindow_NativeObjectUnreference(held_);
  if (OH_NativeImage_ReleaseNativeWindowBuffer(image_, held_, heldFenceFd_) != 0) {
    // Release is the reference the acquire took; when it does not take the buffer back, dropping the
    // extra one keeps the count from leaking.
    OH_NativeWindow_NativeObjectUnreference(held_);
  }
  held_ = nullptr;
  heldFenceFd_ = -1;
}

}
