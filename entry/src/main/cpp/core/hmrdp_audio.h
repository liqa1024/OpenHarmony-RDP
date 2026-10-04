/*
 * HmRdp - HarmonyOS RDP client
 * PCM handoff ring for the FreeRDP rdpsnd sink (the renderer lives in ArkTS).
 *
 * FreeRDP decodes and forwards 16-bit PCM through a C sink callback that must
 * return synchronously (the render latency the server puts in the Wave Confirm),
 * so that adapter and its handoff ring stay native
 * (doc_agent/architecture.md §3.1 criterion 2). The ring is the single owner of
 * the client-side buffering policy - prebuffer, overflow drop, underrun silence,
 * loss accounting - so the ArkTS AudioRenderer that pulls from it only has to
 * service the callback. Policy and rationale: doc_agent/native-libraries.md §5.
 *
 * Pull model: the audio worker's writeData callback hands us a buffer; Read()
 * fills it with queued audio plus silence (never refuses, so the callback can
 * always report a full buffer). Write() runs on FreeRDP's rdpsnd playback
 * thread. All state is guarded by `mutex_`; no long call is made under it.
 */
#ifndef HMRDP_AUDIO_H
#define HMRDP_AUDIO_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace hmrdp {

class AudioSink {
 public:
  AudioSink() = default;
  ~AudioSink() = default;

  AudioSink(const AudioSink&) = delete;
  AudioSink& operator=(const AudioSink&) = delete;

  // Sink entry (rdpsnd playback thread). Queues one decoded PCM packet and
  // returns the playback time currently queued in ms (the render latency for the
  // Wave Confirm), or 0 when nothing is queued. A rate/channel change drops the
  // old queue and flags a format change for TakeFormatChange().
  int Write(const void* data, size_t size, int sampleRate, int channels);

  // Pull entry for the ArkTS AudioRenderer (audio worker thread). Copies up to
  // `want` bytes of queued PCM into `dest`, filling the remainder with silence,
  // and returns the number of real bytes copied. Never allocates; may span whole
  // packets without ever splitting one on a drop boundary.
  size_t Read(uint8_t* dest, size_t want);

  // Consumes a pending format change (the rate/channels of the newest packet), so
  // the session can ask ArkTS to (re)build its AudioRenderer. Returns false when
  // the format has not changed since the previous call.
  bool TakeFormatChange(int* sampleRate, int* channels);

  // The current format (0/0 before the first packet), so a renderer that starts
  // after the format event can seed itself instead of staying silent.
  void Format(int* sampleRate, int* channels);

  // Drains the loss statistics since the previous call: bytes silence-filled on
  // an underrun plus whole packets dropped on overflow, over the bytes consumed
  // while the stream is live. Returns the current sample rate, or 0 when no
  // format is known. A short gap is a glitch; a longer one is the stream pausing
  // and is not reported as loss (see kLiveWindowUs).
  int TakeLossStats(uint64_t* lostBytes, uint64_t* totalBytes);

  // Drops the queue and forgets the format (session teardown).
  void Reset();

 private:
  // One queued PCM packet. `offset` is how much has already been consumed, so a
  // read that spans several packets never splits one on a drop boundary.
  struct Packet {
    std::vector<uint8_t> data;
    size_t offset = 0;
    size_t remaining() const { return data.size() - offset; }
  };

  // Caller holds `mutex_`.
  void EnqueueLocked(const uint8_t* data, size_t size);
  void DropToHighWaterLocked();
  int LatencyLocked() const;

  std::mutex mutex_;
  int rate_ = 0;
  int channels_ = 0;
  // Set on a rate/channel change, consumed by TakeFormatChange().
  bool formatChanged_ = false;

  // Bounded jitter buffer of whole packets. Watermarks derive from `bytesPerSec_`
  // so they mean the same playback time at any format; the oldest whole packets
  // are dropped past `highWaterBytes_`.
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

  // Loss accounting: overflow drops are written on the playback thread, underrun
  // silence on the audio thread; the telemetry tick drains both.
  std::atomic<uint64_t> lostBytes_{0};
  std::atomic<uint64_t> totalBytes_{0};
};

}  // namespace hmrdp

#endif  // HMRDP_AUDIO_H
