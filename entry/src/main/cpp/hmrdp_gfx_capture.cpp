/*
 * HmRdp - raw RDPGFX channel capture (see hmrdp_gfx_capture.h).
 */
#include "hmrdp_gfx_capture.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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

uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
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
    HMRDP_LOGI("gfx capture: writing %{public}s", path.c_str());
  }
  const uint8_t* p = static_cast<const uint8_t*>(data);
  // u32 little-endian length + the raw bytes.
  const uint8_t len[4] = {static_cast<uint8_t>(size & 0xFFu),
                          static_cast<uint8_t>((size >> 8) & 0xFFu),
                          static_cast<uint8_t>((size >> 16) & 0xFFu),
                          static_cast<uint8_t>((size >> 24) & 0xFFu)};
  fwrite(len, 1, sizeof(len), g_file);
  fwrite(p, 1, size, g_file);
  g_bytes += sizeof(len) + size;
  g_chunks++;
  const uint64_t now = NowMs();
  if (now - g_logMs >= kLogIntervalMs) {
    g_logMs = now;
    HMRDP_LOGI("gfx capture: chunks=%{public}llu bytes=%{public}llu",
               static_cast<unsigned long long>(g_chunks),
               static_cast<unsigned long long>(g_bytes));
  }
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
  return true;
}

bool GfxRawCapture::Next(const uint8_t** data, uint32_t* size) {
  if (data == nullptr || size == nullptr || pos_ + 4 > data_.size()) {
    return false;
  }
  const uint32_t n = static_cast<uint32_t>(data_[pos_]) |
                     (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                     (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                     (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
  if (pos_ + 4 + n > data_.size()) {
    return false;
  }
  *data = &data_[pos_ + 4];
  *size = n;
  pos_ += 4 + n;
  return true;
}

}  // namespace hmrdp
