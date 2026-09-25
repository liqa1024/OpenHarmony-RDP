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
// This is the single owner of client-side audio buffering. FreeRDP hands over
// every decoded packet and never pre-drops (its overrun guard is neutralised for
// this backend, see native/scripts/patch-steps/35-rdpsnd-no-pre-drop.ps1), so
// there is exactly one drop/start policy - this class.
//
// Packets are queued whole and every watermark is expressed in playback time,
// so the latency bound is the same number of milliseconds whatever the format
// is. A new bout of audio is prebuffered before playback starts (that start-up
// silence is not loss); a mid-stream jitter gap is *not* re-primed, because
// re-priming would add a silence gap on top of the one the network just caused.
// The only drop point is the queue growing past its high-water mark (the server
// or its clock running ahead): the oldest whole packets are dropped, which keeps
// 16-bit sample alignment. TakeLossStats reports underrun silence plus those
// drops, so the toolbar glitch rate tells whether the path is actually healthy.
//
// The same queue depth is what Write() returns as the current render latency, so
// rdpsnd can report it to the server in the Wave Confirm (a device that buffers
// internally is expected to answer with its render latency).
//
// Locking: `mutex_` only guards the queue and the renderer handle, and is never
// held while calling into OHAudio. The renderer's write callback runs on the
// audio thread and takes `mutex_`, so holding it across
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

  // Queues one decoded PCM packet. Called on FreeRDP's rdpsnd playback thread,
  // never on the shared drdynvc dispatch thread, so opening a renderer here does
  // not stall the other dynamic channels. Returns the playback time currently
  // queued in ms (the render latency to report to the server), or 0 when nothing
  // is queued / no renderer is open.
  int Write(const void* data, size_t size, int sampleRate, int channels);

  // Stops playback and frees the stream. Safe to call repeatedly.
  void Close();

  // Drains the audio loss statistics since the previous call: bytes silence-
  // filled on an underrun plus whole packets dropped on overflow, over the total
  // bytes consumed while the stream is live. Returns the sample rate in Hz, or 0
  // when no renderer is open. Underruns count only while packets are still
  // arriving (see kLiveWindowUs): a short gap is a glitch, a longer one is the
  // stream pausing and is not reported as loss.
  int TakeLossStats(uint64_t* lostBytes, uint64_t* totalBytes);

 private:
  // One queued PCM packet. `offset` is how much has already been consumed, so a
  // callback that asks for more than one packet can span packets without ever
  // splitting one on a drop boundary.
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
  // Drops the oldest whole packets until the queue is at or below its high-water
  // mark. Caller holds `mutex_`.
  void DropToHighWaterLocked();
  // Playback time queued right now, in ms. Caller holds `mutex_`.
  int LatencyLocked() const;

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
  // Set by OnError / OnInterrupt, consumed by the next Write on the playback
  // thread.
  std::atomic<bool> failed_{false};
  std::atomic<bool> resumeRequested_{false};

  // Audio loss accounting: overflow drops are written on the playback thread,
  // underrun silence on the audio thread; the metrics sampler drains both.
  std::atomic<uint64_t> lostBytes_{0};
  std::atomic<uint64_t> totalBytes_{0};

  // Bounded jitter buffer of whole packets. All watermarks are derived from
  // `bytesPerSec_` so they mean the same thing at any sample rate / channel
  // count, and the oldest whole packets are dropped past `highWaterBytes_`.
  std::deque<Packet> packets_;
  size_t bufferedBytes_ = 0;
  size_t bytesPerSec_ = 0;
  size_t targetBytes_ = 0;
  size_t highWaterBytes_ = 0;
  // False until the queue first reaches targetBytes_; reset when a new bout of
  // audio starts after a long idle gap, so each bout is prebuffered once while a
  // mid-stream jitter gap is not.
  bool primed_ = false;
  // Arrival stamp of the newest packet; a gap longer than kIdleResetUs means a
  // new bout rather than network jitter.
  uint64_t lastPacketUs_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_AUDIO_H
