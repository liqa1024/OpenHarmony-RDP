/*
 * HmRdp - HarmonyOS RDP client
 * PCM playback for the FreeRDP rdpsnd sink (OHAudio, dynamically loaded).
 */
#include "hmrdp_audio.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dlfcn.h>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// Prebuffer before playback starts, and the hard latency bound above which the
// oldest whole packets are dropped. Both in playback ms, so the bound does not
// change with the format. The prebuffer rides out delivery jitter; the high
// water caps how far latency may grow when the server (or its clock) runs ahead.
constexpr size_t kTargetLatencyMs = 120;
constexpr size_t kHighWaterLatencyMs = 300;
// A gap longer than this is a new bout of audio (the server paused), not jitter,
// so the prebuffer runs again; an ordinary jitter gap does not re-prime.
constexpr uint64_t kIdleResetUs = 1000 * 1000;
// Underruns count as loss only while packets are still arriving. A gap longer
// than this is the stream pausing rather than a glitch, so it stops contributing.
constexpr uint64_t kLiveWindowUs = 500 * 1000;

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

int AudioOutput::Write(const void* data, size_t size, int sampleRate, int channels) {
  if (data == nullptr || size == 0 || sampleRate <= 0 || channels <= 0) {
    return 0;
  }
  if (!Supported()) {
    return 0;
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
      return 0;
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
  return LatencyLocked();
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
    bytesPerSec_ = 0;
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

  {
    std::lock_guard<std::mutex> lock(mutex_);
    renderer_ = renderer;
    rate_ = sampleRate;
    channels_ = channels;
    bytesPerSec_ = static_cast<size_t>(sampleRate) * static_cast<size_t>(channels) *
                   sizeof(int16_t);
    targetBytes_ = bytesPerSec_ * kTargetLatencyMs / 1000;
    highWaterBytes_ = bytesPerSec_ * kHighWaterLatencyMs / 1000;
    packets_.clear();
    bufferedBytes_ = 0;
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
      bytesPerSec_ = 0;
      targetBytes_ = 0;
      highWaterBytes_ = 0;
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
  if (bytesPerSec_ == 0) {
    return;
  }
  const uint64_t now = AudioNowUs();
  // A packet after a long idle gap starts a new bout: prebuffer it again. A
  // short gap is jitter and must not re-prime (that would add silence of its
  // own).
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
  // byte would break 16-bit sample alignment and click. Keep at least the newest
  // packet so a single oversized packet is still played rather than discarded.
  while (bufferedBytes_ > highWaterBytes_ && packets_.size() > 1) {
    const size_t dropped = packets_.front().remaining();
    packets_.pop_front();
    bufferedBytes_ -= dropped;
    lostBytes_.fetch_add(dropped);
    totalBytes_.fetch_add(dropped);
  }
}

int AudioOutput::LatencyLocked() const {
  if (bytesPerSec_ == 0) {
    return 0;
  }
  return static_cast<int>(bufferedBytes_ * 1000 / bytesPerSec_);
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

// Pull model: the audio thread hands us a buffer to fill. Playback only starts
// once the queue holds the prebuffer target; an underrun after that is filled
// with silence so the callback cadence never stalls. While the stream is live
// (see kLiveWindowUs) the requested bytes count towards the total and the
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
  bool live = false;
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (!self->primed_ && self->bufferedBytes_ >= self->targetBytes_) {
      self->primed_ = true;
    }
    if (self->primed_) {
      // Live = playback started and packets are still (or were just) arriving.
      // Deliberately keyed on arrival, not delivery: when delivery stalls, that
      // is exactly what has to be reported.
      live = self->lastPacketUs_ != 0 && (now - self->lastPacketUs_ < kLiveWindowUs);
      while (copied < want && !self->packets_.empty()) {
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
    // Not primed yet: leave the buffer as silence so the device stays fed while
    // the queue fills. A stream start, not a dropped stream, so not counted.
  }
  if (copied < want) {
    memset(out + copied, 0, want - copied);
  }
  if (live) {
    self->totalBytes_.fetch_add(want);
    if (copied < want) {
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
  // Only a shared resume needs an explicit restart; forced pause/stop is done
  // by the system. Restarting happens on the playback thread in Write().
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
