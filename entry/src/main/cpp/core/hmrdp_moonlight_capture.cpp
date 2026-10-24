#include "hmrdp_moonlight_capture.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include <Limelight.h>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_enabled{false};
std::string g_dir;
FILE* g_file = nullptr;
uint64_t g_bytes = 0;
uint64_t g_frames = 0;
uint64_t g_logMs = 0;

constexpr uint64_t kMaxBytes = 512ull * 1024 * 1024;
constexpr uint64_t kLogIntervalMs = 5 * 1000;

// v1 layout: 8-byte magic, then per access unit a fixed little-endian header followed by the
// Annex-B payload. The last magic byte is the version digit.
constexpr uint8_t kMagic[8] = {'H', 'M', 'R', 'D', 'M', 'L', '0', '1'};
constexpr size_t kMagicSize = sizeof(kMagic);

uint64_t NowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t NowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

void PutU64(std::vector<uint8_t>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

// Runs on the library's decoder thread (via LiSetVideoCapture); the disabled path must stay cheap.
void CaptureFrame(const void* unit) {
  if (unit == nullptr || !g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  const DECODE_UNIT* decodeUnit = static_cast<const DECODE_UNIT*>(unit);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled.load(std::memory_order_relaxed) || g_bytes >= kMaxBytes) {
    return;
  }
  if (g_file == nullptr) {
    const std::string path = g_dir + "/hmrdp_ml.bin";
    g_file = fopen(path.c_str(), "wb");
    if (g_file == nullptr) {
      HMRDP_LOGW("moonlight capture: cannot open %{public}s", path.c_str());
      g_enabled.store(false, std::memory_order_relaxed);
      return;
    }
    fwrite(kMagic, 1, kMagicSize, g_file);
    g_bytes = kMagicSize;
    HMRDP_LOGI("moonlight capture: writing %{public}s (v%{public}u)", path.c_str(),
               static_cast<unsigned>(kMagic[kMagicSize - 1] - '0'));
  }
  // The per-frame header is exactly the depacketizer's own account of the access unit, so the file
  // can be replayed/analyzed without re-deriving timing or the HDR/colour facts from the bitstream.
  const uint32_t payloadLength =
      decodeUnit->fullLength > 0 ? static_cast<uint32_t>(decodeUnit->fullLength) : 0;
  std::vector<uint8_t> header;
  header.reserve(64);
  PutU32(header, payloadLength);
  PutU32(header, static_cast<uint32_t>(decodeUnit->frameType));
  PutU32(header, static_cast<uint32_t>(decodeUnit->frameNumber));
  PutU32(header, decodeUnit->rtpTimestamp);
  PutU32(header, static_cast<uint32_t>(decodeUnit->frameHostProcessingLatency));
  header.push_back(decodeUnit->hdrActive ? 1u : 0u);
  header.push_back(decodeUnit->colorspace);
  header.push_back(0);
  header.push_back(0);
  PutU64(header, decodeUnit->receiveTimeUs);
  PutU64(header, decodeUnit->enqueueTimeUs);
  PutU64(header, decodeUnit->presentationTimeUs);
  PutU64(header, NowUs());
  fwrite(header.data(), 1, header.size(), g_file);
  uint64_t written = header.size();
  for (const LENTRY* entry = decodeUnit->bufferList; entry != nullptr; entry = entry->next) {
    if (entry->data != nullptr && entry->length > 0) {
      fwrite(entry->data, 1, static_cast<size_t>(entry->length), g_file);
      written += static_cast<uint64_t>(entry->length);
    }
  }
  g_bytes += written;
  g_frames++;
  const uint64_t now = NowMs();
  if (now - g_logMs >= kLogIntervalMs) {
    g_logMs = now;
    fflush(g_file);
    HMRDP_LOGI("moonlight capture: frames=%{public}llu bytes=%{public}llu",
               static_cast<unsigned long long>(g_frames),
               static_cast<unsigned long long>(g_bytes));
  }
}

}  // namespace

void MoonlightCaptureConfigure(bool enabled, const std::string& dir) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool wantEnabled = enabled && !dir.empty();
    if (g_enabled.load() == wantEnabled && g_dir == dir) {
      return;  // idempotent: never truncate a running capture
    }
    if (g_file != nullptr) {
      fclose(g_file);
      g_file = nullptr;
    }
    g_dir = dir;
    g_bytes = 0;
    g_frames = 0;
    g_logMs = NowMs();
    g_enabled.store(wantEnabled);
  }
  // Installed once and left in place: a stable function pointer cannot race the library's decoder
  // thread, and the disabled path is a single relaxed atomic load.
  LiSetVideoCapture(&CaptureFrame);
  HMRDP_LOGI("moonlight capture: %{public}s", enabled ? "enabled" : "disabled");
}

void MoonlightCaptureFlush() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != nullptr) {
    fflush(g_file);
  }
}

void MoonlightCaptureShutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != nullptr) {
    fclose(g_file);
    g_file = nullptr;
  }
  if (g_enabled.load()) {
    HMRDP_LOGI("moonlight capture: stopped frames=%{public}llu bytes=%{public}llu",
               static_cast<unsigned long long>(g_frames),
               static_cast<unsigned long long>(g_bytes));
  }
  g_enabled.store(false);
}

}  // namespace hmrdp
