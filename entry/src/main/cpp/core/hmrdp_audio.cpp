#include "hmrdp_audio.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// A gap longer than this is a new bout of audio, not jitter, so the prebuffer
// runs again.
constexpr uint64_t kIdleResetUs = 1000 * 1000;
// Underruns count as loss only while packets are still arriving; a longer gap is
// the stream pausing rather than a glitch.
constexpr uint64_t kLiveWindowUs = 500 * 1000;

uint64_t AudioNowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

int AudioSink::Write(const void* data, size_t size, int sampleRate, int channels) {
  if (data == nullptr || size == 0 || sampleRate <= 0 || channels <= 0) {
    return 0;
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data);

  std::lock_guard<std::mutex> lock(mutex_);
  if (rate_ != sampleRate || channels_ != channels) {
    packets_.clear();
    bufferedBytes_ = 0;
    primed_ = false;
    lastPacketUs_ = 0;
    rate_ = sampleRate;
    channels_ = channels;
    bytesPerSec_ = static_cast<size_t>(sampleRate) * static_cast<size_t>(channels) *
                   sizeof(int16_t);
    ApplyBudgetLocked();
    formatChanged_ = true;
    HMRDP_LOGI("audio: format rate=%{public}d channels=%{public}d", sampleRate, channels);
  }

  EnqueueLocked(bytes, size);
  return LatencyLocked();
}

size_t AudioSink::Read(uint8_t* dest, size_t want) {
  if (dest == nullptr || want == 0) {
    return 0;
  }
  const uint64_t now = AudioNowUs();
  size_t copied = 0;
  bool live = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytesPerSec_ != 0) {
      if (!primed_ && bufferedBytes_ >= targetBytes_) {
        primed_ = true;
      }
      if (primed_) {
        // Live is keyed on arrival, not delivery: when delivery stalls, that is
        // exactly what has to be reported.
        live = lastPacketUs_ != 0 && (now - lastPacketUs_ < kLiveWindowUs);
        while (copied < want && !packets_.empty()) {
          Packet& packet = packets_.front();
          const size_t chunk = std::min(packet.remaining(), want - copied);
          memcpy(dest + copied, packet.data.data() + packet.offset, chunk);
          packet.offset += chunk;
          bufferedBytes_ -= chunk;
          copied += chunk;
          if (packet.remaining() == 0) {
            packets_.pop_front();
          }
        }
      }
      // Not primed yet: leave the buffer silent so the device stays fed while the
      // queue fills; a stream start, not a dropped stream, so not counted.
    }
  }
  if (copied < want) {
    memset(dest + copied, 0, want - copied);
  }
  if (live) {
    totalBytes_.fetch_add(want);
    if (copied < want) {
      lostBytes_.fetch_add(want - copied);
    }
  }
  return copied;
}

bool AudioSink::TakeFormatChange(int* sampleRate, int* channels) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!formatChanged_) {
    return false;
  }
  formatChanged_ = false;
  if (sampleRate != nullptr) {
    *sampleRate = rate_;
  }
  if (channels != nullptr) {
    *channels = channels_;
  }
  return true;
}

void AudioSink::Format(int* sampleRate, int* channels) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sampleRate != nullptr) {
    *sampleRate = rate_;
  }
  if (channels != nullptr) {
    *channels = channels_;
  }
}

int AudioSink::TakeLossStats(uint64_t* lostBytes, uint64_t* totalBytes) {
  const uint64_t lost = lostBytes_.exchange(0);
  const uint64_t total = totalBytes_.exchange(0);
  if (lostBytes != nullptr) {
    *lostBytes = lost;
  }
  if (totalBytes != nullptr) {
    *totalBytes = total;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return rate_;
}

void AudioSink::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  packets_.clear();
  bufferedBytes_ = 0;
  rate_ = 0;
  channels_ = 0;
  bytesPerSec_ = 0;
  targetBytes_ = 0;
  highWaterBytes_ = 0;
  primed_ = false;
  lastPacketUs_ = 0;
  formatChanged_ = false;
  lostBytes_.store(0);
  totalBytes_.store(0);
}

void AudioSink::SetLatencyBudget(int targetMs, int highWaterMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  targetLatencyMs_ = targetMs > 0 ? targetMs : 0;
  // Never let the drop bound fall below the prefill, or every write would trim
  // back to the target.
  const int minHighWater = targetLatencyMs_ > 0 ? targetLatencyMs_ : 1;
  highWaterLatencyMs_ = highWaterMs > minHighWater ? highWaterMs : minHighWater;
  ApplyBudgetLocked();
}

void AudioSink::ApplyBudgetLocked() {
  if (bytesPerSec_ == 0) {
    return;
  }
  targetBytes_ = bytesPerSec_ * static_cast<size_t>(targetLatencyMs_) / 1000;
  highWaterBytes_ = bytesPerSec_ * static_cast<size_t>(highWaterLatencyMs_) / 1000;
}

void AudioSink::EnqueueLocked(const uint8_t* data, size_t size) {
  if (bytesPerSec_ == 0) {
    return;
  }
  const uint64_t now = AudioNowUs();
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

void AudioSink::DropToHighWaterLocked() {
  // Drop the oldest whole packets, never a partial one: trimming breaks 16-bit
  // sample alignment; keep at least the newest so a lone oversized packet still plays.
  while (bufferedBytes_ > highWaterBytes_ && packets_.size() > 1) {
    const size_t dropped = packets_.front().remaining();
    packets_.pop_front();
    bufferedBytes_ -= dropped;
    lostBytes_.fetch_add(dropped);
    totalBytes_.fetch_add(dropped);
  }
}

int AudioSink::LatencyLocked() const {
  if (bytesPerSec_ == 0) {
    return 0;
  }
  return static_cast<int>(bufferedBytes_ * 1000 / bytesPerSec_);
}

}  // namespace hmrdp
