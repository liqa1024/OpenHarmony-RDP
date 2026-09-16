/*
 * HmRdp - raw RDPGFX channel capture (see hmrdp_gfx_capture.h).
 */
#include "hmrdp_gfx_capture.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

std::mutex g_mutex;
bool g_enabled = false;
std::atomic<bool> g_replaying{false};
std::string g_dir;
FILE* g_file = nullptr;
uint64_t g_bytes = 0;
uint64_t g_chunks = 0;
uint64_t g_logMs = 0;

constexpr uint64_t kMaxBytes = 512ull * 1024 * 1024;
constexpr uint64_t kLogIntervalMs = 5 * 1000;

// File magic of the timestamped layout (version 1). Its first four bytes are
// 0x31584750 = 829M, far above any chunk length a version 0 file could carry
// (the cap is kMaxBytes), so sniffing the magic cannot mistake the two layouts.
constexpr uint8_t kMagic[8] = {'H', 'M', 'R', 'G', 'P', 'G', 'X', '1'};
constexpr size_t kHeaderSize = sizeof(kMagic);

uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Monotonic microseconds. Arrival times only ever need to be comparable to each
// other within one capture, so a steady clock is enough (and it cannot jump).
uint64_t NowUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

void GfxDumpConfigure(bool enabled, const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool wantEnabled = enabled && !dir.empty();
  if (g_enabled == wantEnabled && g_dir == dir) {
    return;  // idempotent: never truncate a running capture
  }
  if (g_file != nullptr) {
    fclose(g_file);
    g_file = nullptr;
  }
  g_dir = dir;
  g_bytes = 0;
  g_chunks = 0;
  g_logMs = NowMs();
  g_enabled = wantEnabled;
  HMRDP_LOGI("gfx capture: %{public}s", g_enabled ? "enabled" : "disabled");
}

void GfxDumpShutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != nullptr) {
    fclose(g_file);
    g_file = nullptr;
  }
  if (g_enabled) {
    HMRDP_LOGI("gfx capture: stopped chunks=%{public}llu bytes=%{public}llu",
               static_cast<unsigned long long>(g_chunks),
               static_cast<unsigned long long>(g_bytes));
  }
  g_enabled = false;
}

void GfxDumpFlush() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != nullptr) {
    fflush(g_file);
  }
}

void GfxDumpSetReplaying(bool replaying) {
  g_replaying.store(replaying);
}

void GfxDumpRaw(const void* data, uint32_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled || g_replaying.load() || g_bytes >= kMaxBytes) {
    return;
  }
  if (g_file == nullptr) {
    const std::string path = g_dir + "/hmrdp_gfx.bin";
    g_file = fopen(path.c_str(), "wb");
    if (g_file == nullptr) {
      HMRDP_LOGW("gfx capture: cannot open %{public}s", path.c_str());
      g_enabled = false;
      return;
    }
    fwrite(kMagic, 1, sizeof(kMagic), g_file);
    g_bytes = sizeof(kMagic);
    HMRDP_LOGI("gfx capture: writing %{public}s (v%{public}u, timestamps)",
               path.c_str(), static_cast<unsigned>(kMagic[7] - '0'));
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  // u32 little-endian length + u64 arrival time + the raw bytes.
  const uint8_t len[4] = {static_cast<uint8_t>(size & 0xFFu),
                          static_cast<uint8_t>((size >> 8) & 0xFFu),
                          static_cast<uint8_t>((size >> 16) & 0xFFu),
                          static_cast<uint8_t>((size >> 24) & 0xFFu)};
  const uint64_t nowUs = NowUs();
  const uint8_t ts[8] = {static_cast<uint8_t>(nowUs & 0xFFu),
                         static_cast<uint8_t>((nowUs >> 8) & 0xFFu),
                         static_cast<uint8_t>((nowUs >> 16) & 0xFFu),
                         static_cast<uint8_t>((nowUs >> 24) & 0xFFu),
                         static_cast<uint8_t>((nowUs >> 32) & 0xFFu),
                         static_cast<uint8_t>((nowUs >> 40) & 0xFFu),
                         static_cast<uint8_t>((nowUs >> 48) & 0xFFu),
                         static_cast<uint8_t>((nowUs >> 56) & 0xFFu)};
  fwrite(len, 1, sizeof(len), g_file);
  fwrite(ts, 1, sizeof(ts), g_file);
  fwrite(p, 1, size, g_file);
  g_bytes += sizeof(len) + sizeof(ts) + size;
  g_chunks++;
  const uint64_t now = NowMs();
  if (now - g_logMs >= kLogIntervalMs) {
    g_logMs = now;
    HMRDP_LOGI("gfx capture: chunks=%{public}llu bytes=%{public}llu",
               static_cast<unsigned long long>(g_chunks),
               static_cast<unsigned long long>(g_bytes));
  }
}

bool GfxCaptureHasTimestamps(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  uint8_t head[kHeaderSize] = {0};
  f.read(reinterpret_cast<char*>(head), sizeof(head));
  return f.gcount() == static_cast<std::streamsize>(sizeof(head)) &&
         memcmp(head, kMagic, sizeof(kMagic)) == 0;
}

bool GfxRawCapture::Open(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  if (n <= 0) {
    return false;
  }
  data_.resize(static_cast<size_t>(n));
  f.read(reinterpret_cast<char*>(data_.data()), n);
  pos_ = 0;
  version_ = 0;
  timestampUs_ = 0;
  if (data_.size() >= kHeaderSize && memcmp(data_.data(), kMagic, sizeof(kMagic)) == 0) {
    version_ = 1;
    pos_ = kHeaderSize;
  }
  return true;
}

bool GfxRawCapture::Next(const uint8_t** data, uint32_t* size) {
  const size_t headerBytes = version_ >= 1 ? 4 + 8 : 4;
  if (data == nullptr || size == nullptr || pos_ + headerBytes > data_.size()) {
    return false;
  }
  const uint32_t n = static_cast<uint32_t>(data_[pos_]) |
                     (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                     (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                     (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
  if (pos_ + headerBytes + n > data_.size()) {
    return false;
  }
  timestampUs_ = 0;
  if (version_ >= 1) {
    const uint8_t* ts = &data_[pos_ + 4];
    for (uint32_t i = 0; i < 8; ++i) {
      timestampUs_ |= static_cast<uint64_t>(ts[i]) << (8 * i);
    }
  }
  *data = &data_[pos_ + headerBytes];
  *size = n;
  pos_ += headerBytes + n;
  return true;
}

}  // namespace hmrdp
