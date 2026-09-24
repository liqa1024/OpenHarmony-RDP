/*
 * HmRdp - HarmonyOS RDP client
 * PCM playback for the FreeRDP rdpsnd sink (OHAudio, dynamically loaded).
 */
#include "hmrdp_audio.h"

#include <algorithm>
#include <dlfcn.h>
#include <chrono>
#include <cstring>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// Jitter buffer: playback does not start until this much PCM is queued, so a
// short network dip does not become an audible gap.
constexpr size_t kTargetLatencyMs = 150;
// Hard bound: the oldest whole packets are dropped above this, so latency can
// never grow without bound. Audio is delivered on the drdynvc thread, which also
// runs the frame pipeline, so a heavy frame's decode can hold delivery off for a
// few hundred ms; this is sized to ride that out. Keep in sync with
// HmrdpRdpsndBufferLatencyMs(), the value the rdpsnd backend reports so FreeRDP
// stops pre-dropping samples.
constexpr size_t kHighWaterLatencyMs = 500;
// While playback is running, underruns count as loss until this long after the
// last packet arrived. Keyed on packet arrival (not delivery): when audio is
// held off, delivery stops first - that is the stall to report, so it must not
// close the window. It is generous on purpose: with the frame-rate cap on, the
// audio channel shares the drdynvc thread with the frame decode and packets can
// be held off for hundreds of ms at a time.
constexpr uint64_t kLiveWindowUs = 2000 * 1000;
// A gap longer than this is a new bout of audio rather than network jitter, so
// the prebuffer target is rebuilt for it. Long enough that an ordinary jitter
// gap never triggers a re-prime (which would add silence of its own).
constexpr uint64_t kIdleResetUs = 1000 * 1000;

uint64_t AudioNowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

using CreateFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder**,
                                           OH_AudioStream_Type);
using DestroyFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*);
using SetSamplingRateFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*, int32_t);
using SetChannelCountFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*, int32_t);
using SetSampleFormatFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*,
                                                    OH_AudioStream_SampleFormat);
using SetEncodingTypeFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*,
                                                    OH_AudioStream_EncodingType);
using SetRendererInfoFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*,
                                                    OH_AudioStream_Usage);
using SetWriteDataFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*,
                                                 OH_AudioRenderer_OnWriteDataCallback, void*);
using SetInterruptFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*,
                                                 OH_AudioRenderer_OnInterruptCallback, void*);
using SetErrorFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*,
                                             OH_AudioRenderer_OnErrorCallback, void*);
using GenerateRendererFn = OH_AudioStream_Result (*)(OH_AudioStreamBuilder*,
                                                     OH_AudioRenderer**);
using StartFn = OH_AudioStream_Result (*)(OH_AudioRenderer*);
using StopFn = OH_AudioStream_Result (*)(OH_AudioRenderer*);
using ReleaseFn = OH_AudioStream_Result (*)(OH_AudioRenderer*);
using SetVolumeFn = OH_AudioStream_Result (*)(OH_AudioRenderer*, float);

struct OhaudioApi {
  bool ready = false;
  CreateFn create = nullptr;
  DestroyFn destroy = nullptr;
  SetSamplingRateFn setSamplingRate = nullptr;
  SetChannelCountFn setChannelCount = nullptr;
  SetSampleFormatFn setSampleFormat = nullptr;
  SetEncodingTypeFn setEncodingType = nullptr;
  SetRendererInfoFn setRendererInfo = nullptr;
  SetWriteDataFn setWriteData = nullptr;
  SetInterruptFn setInterrupt = nullptr;
  SetErrorFn setError = nullptr;
  GenerateRendererFn generate = nullptr;
  StartFn start = nullptr;
  StopFn stop = nullptr;
  ReleaseFn release = nullptr;
  // Optional: only used to pin the volume to full if the default ever changes.
  SetVolumeFn setVolume = nullptr;
};

template <typename T>
bool Resolve(void* handle, const char* name, T& target) {
  target = reinterpret_cast<T>(dlsym(handle, name));
  return target != nullptr;
}

OhaudioApi LoadApi() {
  OhaudioApi api;
  void* handle = dlopen("libohaudio.so", RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    HMRDP_LOGW("audio: libohaudio.so unavailable, audio disabled");
    return api;
  }
  const bool ok =
      Resolve(handle, "OH_AudioStreamBuilder_Create", api.create) &&
      Resolve(handle, "OH_AudioStreamBuilder_Destroy", api.destroy) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetSamplingRate", api.setSamplingRate) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetChannelCount", api.setChannelCount) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetSampleFormat", api.setSampleFormat) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetEncodingType", api.setEncodingType) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetRendererInfo", api.setRendererInfo) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetRendererWriteDataCallback", api.setWriteData) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetRendererInterruptCallback", api.setInterrupt) &&
      Resolve(handle, "OH_AudioStreamBuilder_SetRendererErrorCallback", api.setError) &&
      Resolve(handle, "OH_AudioStreamBuilder_GenerateRenderer", api.generate) &&
      Resolve(handle, "OH_AudioRenderer_Start", api.start) &&
      Resolve(handle, "OH_AudioRenderer_Stop", api.stop) &&
      Resolve(handle, "OH_AudioRenderer_Release", api.release);
  if (!ok) {
    HMRDP_LOGW("audio: libohaudio.so is missing required entry points, audio disabled");
    dlclose(handle);
    return api;
  }
  Resolve(handle, "OH_AudioRenderer_SetVolume", api.setVolume);
  api.ready = true;
  HMRDP_LOGI("audio: libohaudio.so loaded");
  return api;
}

OhaudioApi& Api() {
  // Function-local static: resolved once, thread-safe under C++11.
  static OhaudioApi api = LoadApi();
  return api;
}

}  // namespace

AudioOutput::~AudioOutput() {
  Close();
}

bool AudioOutput::Supported() {
  return Api().ready;
}

void AudioOutput::Write(const void* data, size_t size, int sampleRate, int channels) {
  if (data == nullptr || size == 0 || sampleRate <= 0 || channels <= 0) {
    return;
  }
  if (!Supported()) {
    return;
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data);

  if (failed_.exchange(false)) {
    // The renderer died (device removed, service restart, ...). Rebuild lazily
    // rather than keep writing into a dead stream.
    HMRDP_LOGW("audio: renderer failed, rebuilding");
    Close();
  }

  bool needOpen = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    needOpen = (renderer_ == nullptr) || rate_ != sampleRate || channels_ != channels;
  }
  if (needOpen) {
    if (!OpenRenderer(sampleRate, channels)) {
      return;
    }
  } else if (resumeRequested_.exchange(false)) {
    StartRenderer();
  }

  static std::atomic<int> loggedPackets{0};
  if (loggedPackets.fetch_add(1) == 0) {
    HMRDP_LOGI("audio: first PCM packet %{public}d bytes rate=%{public}d channels=%{public}d",
               static_cast<int>(size), sampleRate, channels);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  EnqueueLocked(bytes, size);
}

void AudioOutput::Close() {
  std::lock_guard<std::mutex> lock(lifecycle_);
  DetachAndReleaseLocked();
}

void AudioOutput::DetachAndReleaseLocked() {
  const OhaudioApi& api = Api();
  OH_AudioRenderer* renderer = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer = renderer_;
    renderer_ = nullptr;
    rate_ = 0;
    channels_ = 0;
    packets_.clear();
    bufferedBytes_ = 0;
    targetBytes_ = 0;
    highWaterBytes_ = 0;
    primed_ = false;
    lastPacketUs_ = 0;
    resumeRequested_.store(false);
    failed_.store(false);
  }
  // Never hold mutex_ here: Stop/Release wait for the write callback, which
  // needs mutex_ to drain the queue.
  if (renderer != nullptr) {
    api.stop(renderer);
    api.release(renderer);
  }
}

bool AudioOutput::OpenRenderer(int sampleRate, int channels) {
  const OhaudioApi& api = Api();
  if (!api.ready) {
    return false;
  }
  std::lock_guard<std::mutex> lifecycle(lifecycle_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderer_ != nullptr && rate_ == sampleRate && channels_ == channels) {
      return true;
    }
  }
  DetachAndReleaseLocked();

  OH_AudioStreamBuilder* builder = nullptr;
  if (api.create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS ||
      builder == nullptr) {
    HMRDP_LOGW("audio: create builder failed");
    return false;
  }
  const bool configured =
      (api.setSamplingRate(builder, sampleRate) == AUDIOSTREAM_SUCCESS) &&
      (api.setChannelCount(builder, channels) == AUDIOSTREAM_SUCCESS) &&
      (api.setSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE) == AUDIOSTREAM_SUCCESS) &&
      (api.setEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW) == AUDIOSTREAM_SUCCESS) &&
      (api.setRendererInfo(builder, AUDIOSTREAM_USAGE_MUSIC) == AUDIOSTREAM_SUCCESS) &&
      (api.setWriteData(builder, &AudioOutput::OnWrite, this) == AUDIOSTREAM_SUCCESS) &&
      (api.setInterrupt(builder, &AudioOutput::OnInterrupt, this) == AUDIOSTREAM_SUCCESS) &&
      (api.setError(builder, &AudioOutput::OnError, this) == AUDIOSTREAM_SUCCESS);
  OH_AudioRenderer* renderer = nullptr;
  const bool generated = configured &&
                         (api.generate(builder, &renderer) == AUDIOSTREAM_SUCCESS) &&
                         renderer != nullptr;
  api.destroy(builder);
  if (!generated) {
    if (renderer != nullptr) {
      api.release(renderer);
    }
    HMRDP_LOGW("audio: renderer open failed (rate=%{public}d channels=%{public}d)",
               sampleRate, channels);
    return false;
  }
  if (api.setVolume != nullptr) {
    api.setVolume(renderer, 1.0f);
  }

  const size_t bytesPerSec =
      static_cast<size_t>(sampleRate) * static_cast<size_t>(channels) * sizeof(int16_t);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer_ = renderer;
    rate_ = sampleRate;
    channels_ = channels;
    packets_.clear();
    bufferedBytes_ = 0;
    targetBytes_ = bytesPerSec * kTargetLatencyMs / 1000;
    highWaterBytes_ = bytesPerSec * kHighWaterLatencyMs / 1000;
    primed_ = false;
    lastPacketUs_ = 0;
    failed_.store(false);
    resumeRequested_.store(false);
  }
  // Start outside mutex_: the first write callback may fire immediately and
  // takes mutex_ to drain the queue.
  if (api.start(renderer) != AUDIOSTREAM_SUCCESS) {
    HMRDP_LOGW("audio: renderer start failed");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      renderer_ = nullptr;
      rate_ = 0;
      channels_ = 0;
      packets_.clear();
      bufferedBytes_ = 0;
    }
    api.stop(renderer);
    api.release(renderer);
    return false;
  }
  HMRDP_LOGI("audio: renderer started rate=%{public}d channels=%{public}d", sampleRate,
             channels);
  return true;
}

void AudioOutput::StartRenderer() {
  const OhaudioApi& api = Api();
  OH_AudioRenderer* renderer = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer = renderer_;
  }
  if (renderer != nullptr) {
    api.start(renderer);
  }
}

void AudioOutput::EnqueueLocked(const uint8_t* data, size_t size) {
  const uint64_t now = AudioNowUs();
  // A packet after a long idle gap starts a new bout: prebuffer it again. A
  // short gap is jitter and must not re-prime (see kIdleResetUs).
  if (lastPacketUs_ != 0 && now - lastPacketUs_ > kIdleResetUs) {
    primed_ = false;
  }
  lastPacketUs_ = now;

  Packet& packet = packets_.emplace_back();
  packet.data.assign(data, data + size);
  packet.offset = 0;
  bufferedBytes_ += size;
  DropToHighWaterLocked();
}

void AudioOutput::DropToHighWaterLocked() {
  // Drop the oldest whole packets, never a partial one: trimming a packet to the
  // byte would shift the 16-bit sample alignment and click. Keep at least the
  // newest packet so a single oversized packet is still played.
  while (bufferedBytes_ > highWaterBytes_ && packets_.size() > 1) {
    const size_t dropped = packets_.front().remaining();
    packets_.pop_front();
    bufferedBytes_ -= dropped;
    lostBytes_.fetch_add(dropped);
    totalBytes_.fetch_add(dropped);
  }
}

int AudioOutput::TakeLossStats(uint64_t* lostBytes, uint64_t* totalBytes) {
  const uint64_t lost = lostBytes_.exchange(0);
  const uint64_t total = totalBytes_.exchange(0);
  if (lostBytes != nullptr) {
    *lostBytes = lost;
  }
  if (totalBytes != nullptr) {
    *totalBytes = total;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return renderer_ != nullptr ? rate_ : 0;
}

// Pull model: the audio thread hands us a buffer to fill. Playback begins only
// once the queue holds the prebuffer target; an underrun after that is filled
// with silence so the callback cadence never stalls. While the stream is live
// (see kLiveWindowUs), the requested bytes count towards the total and any
// silence-filled tail towards the loss, so an underrun shows up as a glitch rate.
OH_AudioData_Callback_Result AudioOutput::OnWrite(OH_AudioRenderer*, void* userData,
                                                  void* buffer, int32_t size) {
  AudioOutput* self = static_cast<AudioOutput*>(userData);
  if (self == nullptr || buffer == nullptr || size <= 0) {
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
  }
  uint8_t* out = static_cast<uint8_t*>(buffer);
  const size_t want = static_cast<size_t>(size);
  const uint64_t now = AudioNowUs();
  size_t copied = 0;
  bool priming = false;
  bool live = false;
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (!self->primed_) {
      if (self->bufferedBytes_ < self->targetBytes_) {
        // Prebuffer: keep the device fed with silence until the queue is deep
        // enough. This is a stream start, not a dropped stream, so it is not
        // counted as loss.
        priming = true;
      } else {
        self->primed_ = true;
      }
    }
    // Live = playback has started and packets are still (or were just)
    // arriving. Deliberately not keyed on delivery: when the frame pipeline
    // holds audio off, delivery stops - and that stall is exactly what has to be
    // reported, not hidden.
    live = self->primed_ && self->lastPacketUs_ != 0 &&
           (now - self->lastPacketUs_ < kLiveWindowUs);
    while (!priming && copied < want && !self->packets_.empty()) {
      Packet& packet = self->packets_.front();
      const size_t chunk = std::min(packet.remaining(), want - copied);
      memcpy(out + copied, packet.data.data() + packet.offset, chunk);
      packet.offset += chunk;
      self->bufferedBytes_ -= chunk;
      copied += chunk;
      if (packet.remaining() == 0) {
        self->packets_.pop_front();
      }
    }
  }
  if (copied < want) {
    memset(out + copied, 0, want - copied);
  }
  if (live) {
    self->totalBytes_.fetch_add(want);
    if (!priming && copied < want) {
      self->lostBytes_.fetch_add(want - copied);
    }
  }
  return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

void AudioOutput::OnInterrupt(OH_AudioRenderer*, void* userData,
                              OH_AudioInterrupt_ForceType, OH_AudioInterrupt_Hint hint) {
  AudioOutput* self = static_cast<AudioOutput*>(userData);
  if (self == nullptr) {
    return;
  }
  // A system-driven pause (another app taking audio focus, output device change,
  // ...) shows up here and would be heard as a gap; log it so an externally
  // caused stutter is not mistaken for one on our side.
  HMRDP_LOGI("audio: interrupt hint=%{public}d", static_cast<int>(hint));
  // Only a shared resume needs an explicit restart; forced pause/stop is done
  // by the system. Restarting happens on the RDP thread in Write().
  if (hint == AUDIOSTREAM_INTERRUPT_HINT_RESUME) {
    self->resumeRequested_.store(true);
  }
}

void AudioOutput::OnError(OH_AudioRenderer*, void* userData, OH_AudioStream_Result error) {
  AudioOutput* self = static_cast<AudioOutput*>(userData);
  if (self != nullptr) {
    self->failed_.store(true);
  }
  HMRDP_LOGW("audio: renderer error %{public}d", static_cast<int>(error));
}

}  // namespace hmrdp
