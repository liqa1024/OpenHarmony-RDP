#include "hmrdp_moonlight_audio.h"

#include <mutex>
#include <vector>

#include <Limelight.h>
#include <opus/opus_multistream.h>

#include "hmrdp_audio.h"
#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// Process-global: moonlight-common-c runs one connection at a time and its audio
// callbacks carry no per-call user data, the same reason the video renderer is a
// singleton. The ring is owned here; ArkTS only pulls from it.
class MoonlightAudioRenderer {
 public:
  static MoonlightAudioRenderer& Instance() {
    static MoonlightAudioRenderer renderer;
    return renderer;
  }

  int Init(const POPUS_MULTISTREAM_CONFIGURATION opusConfig) {
    std::lock_guard<std::mutex> lock(mutex_);
    DestroyLocked();
    if (opusConfig == nullptr || opusConfig->sampleRate <= 0 || opusConfig->channelCount <= 0 ||
        opusConfig->streams <= 0 || opusConfig->samplesPerFrame <= 0) {
      HMRDP_LOGE("moonlight audio: invalid opus configuration");
      return -1;
    }
    int error = OPUS_OK;
    decoder_ = opus_multistream_decoder_create(opusConfig->sampleRate, opusConfig->channelCount,
                                               opusConfig->streams, opusConfig->coupledStreams,
                                               opusConfig->mapping, &error);
    if (decoder_ == nullptr || error != OPUS_OK) {
      decoder_ = nullptr;
      HMRDP_LOGE("moonlight audio: opus decoder create failed (%{public}d)", error);
      return -1;
    }
    sampleRate_ = opusConfig->sampleRate;
    channels_ = opusConfig->channelCount;
    samplesPerFrame_ = opusConfig->samplesPerFrame;
    pcm_.assign(static_cast<size_t>(samplesPerFrame_) * static_cast<size_t>(channels_), 0);
    ring_.Reset();
    HMRDP_LOGI("moonlight audio: opus %{public}d Hz %{public}d ch %{public}d samples/frame",
               sampleRate_, channels_, samplesPerFrame_);
    return 0;
  }

  void Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    ring_.Reset();
  }

  void Cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    DestroyLocked();
    ring_.Reset();
  }

  void Decode(const char* data, int length) {
    if (data == nullptr || length <= 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || decoder_ == nullptr || pcm_.empty()) {
      return;
    }
    const int decoded = opus_multistream_decode(decoder_,
        reinterpret_cast<const unsigned char*>(data), length, pcm_.data(), samplesPerFrame_, 0);
    // A damaged or lost packet decodes to a negative code; the decoder's own
    // concealment keeps the timeline, so there is simply nothing to queue.
    if (decoded <= 0) {
      return;
    }
    const size_t bytes =
        static_cast<size_t>(decoded) * static_cast<size_t>(channels_) * sizeof(int16_t);
    ring_.Write(pcm_.data(), bytes, sampleRate_, channels_);
  }

  size_t Read(uint8_t* dest, size_t size) {
    return ring_.Read(dest, size);
  }

  void Format(int* sampleRate, int* channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sampleRate != nullptr) {
      *sampleRate = sampleRate_;
    }
    if (channels != nullptr) {
      *channels = channels_;
    }
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    DestroyLocked();
    ring_.Reset();
  }

 private:
  void DestroyLocked() {
    if (decoder_ != nullptr) {
      opus_multistream_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    sampleRate_ = 0;
    channels_ = 0;
    samplesPerFrame_ = 0;
    pcm_.clear();
  }

  std::mutex mutex_;
  OpusMSDecoder* decoder_ = nullptr;
  int sampleRate_ = 0;
  int channels_ = 0;
  int samplesPerFrame_ = 0;
  std::vector<int16_t> pcm_;
  bool running_ = false;
  AudioSink ring_;
};

int AudioInit(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
              void* context, int arFlags) {
  (void)audioConfiguration;
  (void)context;
  (void)arFlags;
  return MoonlightAudioRenderer::Instance().Init(opusConfig);
}

void AudioStart() {
  MoonlightAudioRenderer::Instance().Start();
}

void AudioStop() {
  MoonlightAudioRenderer::Instance().Stop();
}

void AudioCleanup() {
  MoonlightAudioRenderer::Instance().Cleanup();
}

void AudioDecode(char* sampleData, int sampleLength) {
  MoonlightAudioRenderer::Instance().Decode(sampleData, sampleLength);
}

// CAPABILITY_DIRECT_SUBMIT stays unset, keeping decodeAndPlaySample on Moonlight's
// own audio decoder thread instead of the RTP receive thread.
AUDIO_RENDERER_CALLBACKS g_audioCallbacks = {
    AudioInit, AudioStart, AudioStop, AudioCleanup, AudioDecode, 0,
};

}  // namespace

void* MoonlightAudioCallbacks() {
  return &g_audioCallbacks;
}

size_t MoonlightAudioRead(uint8_t* dest, size_t size) {
  return MoonlightAudioRenderer::Instance().Read(dest, size);
}

void MoonlightAudioFormat(int* sampleRate, int* channels) {
  MoonlightAudioRenderer::Instance().Format(sampleRate, channels);
}

void MoonlightAudioReset() {
  MoonlightAudioRenderer::Instance().Reset();
}

}  // namespace hmrdp
