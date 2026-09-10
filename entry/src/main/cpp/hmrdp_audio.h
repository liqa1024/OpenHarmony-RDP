/*
 * HmRdp - HarmonyOS RDP client
 * PCM playback for the FreeRDP rdpsnd sink (OHAudio, dynamically loaded).
 *
 * libohaudio.so is resolved with dlopen/dlsym instead of being linked: a device
 * whose system image has no audio subsystem then simply reports unsupported and
 * the feature is disabled, instead of the whole app failing to load.
 */
#ifndef HMRDP_AUDIO_H
#define HMRDP_AUDIO_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>

namespace hmrdp {

// Plays the 16-bit PCM stream produced by the rdpsnd sink. One instance per
// session; the server's format (rate/channels) is honoured and a change
// transparently rebuilds the renderer.
//
// Locking: `mutex_` only guards the ring buffer and the renderer handle, and is
// never held while calling into OHAudio. The renderer's write callback runs on
// the audio thread and takes `mutex_`, so holding it across
// OH_AudioRenderer_Stop/Release (which wait for that callback) would deadlock.
// `lifecycle_` serialises open/close and is only ever taken outside `mutex_`.
class AudioOutput {
 public:
  // True when libohaudio.so and every required entry point are available.
  static bool Supported();

  AudioOutput() = default;
  ~AudioOutput();

  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;

  // Queues one decoded PCM packet. Safe to call from the RDP thread.
  void Write(const void* data, size_t size, int sampleRate, int channels);

  // Stops playback and frees the stream. Safe to call repeatedly.
  void Close();

 private:
  bool OpenRenderer(int sampleRate, int channels);
  void StartRenderer();
  // Detaches and releases the renderer without holding `mutex_`; the caller
  // must hold `lifecycle_`.
  void DetachAndReleaseLocked();
  void EnqueueLocked(const uint8_t* data, size_t size);

  // OHAudio callbacks, invoked on the audio thread.
  static OH_AudioData_Callback_Result OnWrite(OH_AudioRenderer* renderer, void* userData,
                                              void* buffer, int32_t size);
  static void OnInterrupt(OH_AudioRenderer* renderer, void* userData,
                          OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint);
  static void OnError(OH_AudioRenderer* renderer, void* userData,
                      OH_AudioStream_Result error);

  std::mutex lifecycle_;
  std::mutex mutex_;
  OH_AudioRenderer* renderer_ = nullptr;
  int rate_ = 0;
  int channels_ = 0;
  // Set by OnError / OnInterrupt, consumed by the next Write on the RDP thread.
  std::atomic<bool> failed_{false};
  std::atomic<bool> resumeRequested_{false};

  // Bounded PCM ring; the oldest data is dropped when the server outruns
  // playback so latency can never grow without bound.
  std::vector<uint8_t> pending_;
  size_t head_ = 0;
  size_t count_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_AUDIO_H
