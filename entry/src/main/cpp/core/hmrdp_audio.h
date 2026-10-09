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

  // Sink entry (rdpsnd playback thread): queues one PCM packet and returns the
  // queued playback time in ms for the Wave Confirm. A rate/channel change drops
  // the old queue and flags a format change for TakeFormatChange().
  int Write(const void* data, size_t size, int sampleRate, int channels);

  // Pull entry (audio worker thread): copies queued PCM into dest, silence-fills
  // the remainder, and returns the number of real bytes copied.
  size_t Read(uint8_t* dest, size_t want);

  // Consumes a pending format change so the session can (re)build its renderer.
  bool TakeFormatChange(int* sampleRate, int* channels);

  // Current format (0/0 before the first packet), so a renderer that starts after
  // the format event can seed itself.
  void Format(int* sampleRate, int* channels);

  // Drains loss stats since the previous call (underrun silence + whole packets
  // dropped on overflow, over bytes consumed while live); returns the sample rate.
  int TakeLossStats(uint64_t* lostBytes, uint64_t* totalBytes);

  void Reset();

 private:
  // `offset` is how much has been consumed, so a read spanning several packets
  // never splits one on a drop boundary.
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
  bool formatChanged_ = false;

  // Bounded jitter buffer of whole packets; watermarks derive from `bytesPerSec_`
  // so they mean the same playback time at any format.
  std::deque<Packet> packets_;
  size_t bufferedBytes_ = 0;
  size_t bytesPerSec_ = 0;
  size_t targetBytes_ = 0;
  size_t highWaterBytes_ = 0;
  // False until the queue first reaches targetBytes_; reset after a long idle gap
  // so each bout is prebuffered once while a mid-stream jitter gap is not.
  bool primed_ = false;
  // Arrival stamp of the newest packet; a gap longer than kIdleResetUs means a
  // new bout rather than network jitter.
  uint64_t lastPacketUs_ = 0;

  std::atomic<uint64_t> lostBytes_{0};
  std::atomic<uint64_t> totalBytes_{0};
};

}  // namespace hmrdp

#endif  // HMRDP_AUDIO_H
