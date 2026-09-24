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
#include <deque>
#include <mutex>
#include <vector>

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>

namespace hmrdp {

// Plays the 16-bit PCM stream produced by the rdpsnd sink. One instance per
// session; the server's format (rate/channels) is honoured and a change
// transparently rebuilds the renderer.
//
// Buffering (hmrdp_audio.cpp): decoded PCM is queued as whole packets. Playback
// starts only after a short prebuffer, so network jitter - or the video frame
// acknowledge the frame-rate cap holds back - cannot punch a hole in the stream.
// While it runs the queue absorbs bursts; when it grows past a high-water mark
// the oldest whole packets are dropped so latency stays bounded. This is the
// sink's only drop policy: the FreeRDP rdpsnd overrun guard is disabled for this
// backend (see HmrdpRdpsndBufferLatencyMs in the rdpsnd backend patch), so it
// never pre-drops a sample the queue could have played.
//
// Locking: `mutex_` only guards the packet queue and the renderer handle, and is
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

  // Drains the audio loss statistics since the previous call: bytes that were
  // silence-filled on an underrun plus whole packets dropped on overflow, over
  // the total bytes the stream asked for. Returns the sample rate in Hz, or 0
  // when no renderer is open. Measurements count while playback has started and
  // packets are still arriving: a stall mid-stream is reported as it happens
  // (delivery stopping is the symptom, so it must not close the window), and
  // only a stream that really ended - no packets for kLiveWindowUs - stops
  // contributing.
  int TakeLossStats(uint64_t* lostBytes, uint64_t* totalBytes);

 private:
  // One queued PCM packet. `offset` is how much has already been consumed, so a
  // callback that requests more than one packet's worth can span packets without
  // ever splitting one on a drop boundary.
  struct Packet {
    std::vector<uint8_t> data;
    size_t offset = 0;
    size_t remaining() const { return data.size() - offset; }
  };

  bool OpenRenderer(int sampleRate, int channels);
  void StartRenderer();
  // Detaches and releases the renderer without holding `mutex_`; the caller
  // must hold `lifecycle_`.
  void DetachAndReleaseLocked();
  void EnqueueLocked(const uint8_t* data, size_t size);
  // Drops the oldest whole packets until the queue is at or below its
  // high-water mark. Caller holds `mutex_`.
  void DropToHighWaterLocked();

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

  // Audio loss accounting, filled on the audio thread and drained by the RDP
  // thread. Only counted while the stream is live (playback started and packets
  // are still arriving - see kLiveWindowUs).
  std::atomic<uint64_t> lostBytes_{0};
  std::atomic<uint64_t> totalBytes_{0};

  // Bounded packet queue. The oldest whole packets are dropped when it grows
  // past highWaterBytes_, so latency can never grow without bound.
  std::deque<Packet> packets_;
  size_t bufferedBytes_ = 0;
  size_t targetBytes_ = 0;
  size_t highWaterBytes_ = 0;
  // False until the queue first reaches targetBytes_; reset when a new bout of
  // audio starts after a long idle gap, so each bout is prebuffered once. A
  // mid-stream jitter gap must NOT re-prime: prebuffering there would add a
  // silence gap on top of the one the network just caused.
  bool primed_ = false;
  // Arrival stamp of the newest packet; a gap longer than the idle reset means a
  // new bout of audio rather than network jitter.
  uint64_t lastPacketUs_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_AUDIO_H
