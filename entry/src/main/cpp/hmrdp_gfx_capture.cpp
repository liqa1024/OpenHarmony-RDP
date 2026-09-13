/*
 * HmRdp - captured RDPGFX data: recording, reading and offline comparison
 * (see hmrdp_gfx_capture.h).
 */
#include "hmrdp_gfx_capture.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <utility>

#include "hmrdp_log.h"

namespace hmrdp {

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

namespace {

std::mutex g_mutex;
bool g_enabled = false;
std::string g_dir;
FILE* g_cmdFile = nullptr;
uint64_t g_cmdBytes = 0;
FILE* g_surfFile = nullptr;
uint64_t g_surfBytes = 0;
uint32_t g_recordIndex = 0;
uint64_t g_hist[0x20] = {0};
uint64_t g_histLogMs = 0;

constexpr uint64_t kCmdMaxBytes = 256ull * 1024 * 1024;
constexpr uint64_t kSurfMaxBytes = 512ull * 1024 * 1024;
constexpr uint64_t kHistLogIntervalMs = 5 * 1000;

// RDPGFX_CMDID_* names, indexed by the low 5 bits of the command id.
const char* const kCmdNames[0x20] = {
    /*0x00*/ "UNUSED_0000",
    /*0x01*/ "WIRETOSURFACE_1",
    /*0x02*/ "WIRETOSURFACE_2",
    /*0x03*/ "DELETEENCODINGCONTEXT",
    /*0x04*/ "SOLIDFILL",
    /*0x05*/ "SURFACETOSURFACE",
    /*0x06*/ "SURFACETOCACHE",
    /*0x07*/ "CACHETOSURFACE",
    /*0x08*/ "EVICTCACHEENTRY",
    /*0x09*/ "CREATESURFACE",
    /*0x0A*/ "DELETESURFACE",
    /*0x0B*/ "STARTFRAME",
    /*0x0C*/ "ENDFRAME",
    /*0x0D*/ "FRAMEACKNOWLEDGE",
    /*0x0E*/ "RESETGRAPHICS",
    /*0x0F*/ "MAPSURFACETOOUTPUT",
    /*0x10*/ "CACHEIMPORTOFFER",
    /*0x11*/ "CACHEIMPORTREPLY",
    /*0x12*/ "CAPSADVERTISE",
    /*0x13*/ "CAPSCONFIRM",
    /*0x14*/ "UNUSED_0014",
    /*0x15*/ "MAPSURFACETOWINDOW",
    /*0x16*/ "QOEFRAMEACKNOWLEDGE",
    /*0x17*/ "MAPSURFACETOSCALEDOUTPUT",
    /*0x18*/ "MAPSURFACETOSCALEDWINDOW",
};

const char* CmdName(uint16_t cmdId) {
  return cmdId < 0x20 ? kCmdNames[cmdId] : "UNKNOWN";
}

uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Writes `count` little-endian u32 values without relying on struct packing.
void WriteU32s(FILE* f, const uint32_t* values, size_t count) {
  uint8_t buf[4];
  for (size_t i = 0; i < count; ++i) {
    const uint32_t v = values[i];
    buf[0] = static_cast<uint8_t>(v & 0xFFu);
    buf[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    fwrite(buf, 1, sizeof(buf), f);
  }
}

void LogHistogramLocked(bool final) {
  std::string out;
  uint64_t total = 0;
  for (size_t i = 0; i < 0x20; ++i) {
    if (g_hist[i] == 0) {
      continue;
    }
    if (!out.empty()) {
      out += " ";
    }
    out += CmdName(static_cast<uint16_t>(i));
    out += "=";
    out += std::to_string(g_hist[i]);
    total += g_hist[i];
  }
  if (total == 0) {
    return;
  }
  HMRDP_LOGI("gfx dump histogram%s: records=%{public}llu %{public}s",
             final ? " (final)" : "", static_cast<unsigned long long>(total), out.c_str());
}

void MaybeLogHistogramLocked() {
  const uint64_t now = NowMs();
  if (now - g_histLogMs < kHistLogIntervalMs) {
    return;
  }
  g_histLogMs = now;
  LogHistogramLocked(false);
}

FILE* OpenFileLocked(FILE*& file, const char* name) {
  if (file != nullptr) {
    return file;
  }
  const std::string path = g_dir + "/" + name;
  file = fopen(path.c_str(), "wb");
  if (file == nullptr) {
    HMRDP_LOGW("gfx dump: cannot open %{public}s", path.c_str());
    g_enabled = false;
    return nullptr;
  }
  HMRDP_LOGI("gfx dump: writing %{public}s", path.c_str());
  return file;
}

}  // namespace

void GfxDumpConfigure(bool enabled, const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool wantEnabled = enabled && !dir.empty();
  // Idempotent: re-applying the same value must not truncate a running capture.
  if (g_enabled == wantEnabled && g_dir == dir) {
    return;
  }
  if (g_cmdFile != nullptr) {
    fclose(g_cmdFile);
    g_cmdFile = nullptr;
  }
  if (g_surfFile != nullptr) {
    fclose(g_surfFile);
    g_surfFile = nullptr;
  }
  g_dir = dir;
  g_cmdBytes = 0;
  g_surfBytes = 0;
  g_recordIndex = 0;
  std::memset(g_hist, 0, sizeof(g_hist));
  g_histLogMs = NowMs();
  g_enabled = wantEnabled;
  HMRDP_LOGI("gfx dump: %{public}s", g_enabled ? "enabled" : "disabled");
}

void GfxDumpFlush() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_cmdFile != nullptr) {
    fflush(g_cmdFile);
  }
  if (g_surfFile != nullptr) {
    fflush(g_surfFile);
  }
}

void GfxDumpShutdown() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled && g_cmdFile == nullptr && g_surfFile == nullptr) {
    return;
  }
  LogHistogramLocked(true);
  if (g_cmdFile != nullptr) {
    fclose(g_cmdFile);
    g_cmdFile = nullptr;
  }
  if (g_surfFile != nullptr) {
    fclose(g_surfFile);
    g_surfFile = nullptr;
  }
  g_enabled = false;
}

uint32_t GfxDumpCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                        const void* params, uint32_t paramsLen, const void* payload,
                        uint32_t payloadLen) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled) {
    return 0;
  }
  const uint32_t index = ++g_recordIndex;
  // Fold unknown command ids into the last slot so the histogram stays bounded.
  const uint16_t slot = cmdId < 0x20 ? cmdId : 0x1F;
  g_hist[slot]++;
  MaybeLogHistogramLocked();

  if (g_cmdBytes >= kCmdMaxBytes) {
    return index;
  }
  if (OpenFileLocked(g_cmdFile, "hmrdp_gfx.bin") == nullptr) {
    return index;
  }
  const uint32_t header[10] = {0x31584647u /* 'GFX1' */, index, cmdId, surfaceId,
                               scalars != nullptr ? scalars[0] : 0,
                               scalars != nullptr ? scalars[1] : 0,
                               scalars != nullptr ? scalars[2] : 0,
                               scalars != nullptr ? scalars[3] : 0,
                               paramsLen, payloadLen};
  WriteU32s(g_cmdFile, header, 10);
  if (params != nullptr && paramsLen > 0) {
    fwrite(params, 1, paramsLen, g_cmdFile);
  }
  if (payload != nullptr && payloadLen > 0) {
    fwrite(payload, 1, payloadLen, g_cmdFile);
  }
  g_cmdBytes += sizeof(header) + paramsLen + payloadLen;
  return index;
}

void GfxDumpSurface(uint32_t recordIndex, uint32_t surfaceId, uint32_t width, uint32_t height,
                    uint32_t stride, uint32_t format, const void* data) {
  if (recordIndex == 0 || data == nullptr || width == 0 || height == 0 || stride == 0) {
    return;
  }
  const uint64_t bytes = static_cast<uint64_t>(stride) * height;
  // FNV-1a 64 over the whole surface: written for every frame so the offline
  // replay can validate the entire capture, not just the frames that fit in the
  // full-baseline size cap.
  uint64_t hash = 1469598103934665603ull;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (uint64_t i = 0; i < bytes; ++i) {
    hash ^= p[i];
    hash *= 1099511628211ull;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_enabled) {
    return;
  }
  if (OpenFileLocked(g_surfFile, "hmrdp_gfx_surface.bin") == nullptr) {
    return;
  }
  const uint32_t hashHeader[8] = {0x31484647u /* 'GFH1' */, recordIndex, surfaceId, width,
                                  height, stride, static_cast<uint32_t>(hash),
                                  static_cast<uint32_t>(hash >> 32)};
  WriteU32s(g_surfFile, hashHeader, 8);
  if (g_surfBytes >= kSurfMaxBytes) {
    return;
  }
  const uint32_t header[8] = {0x31534647u /* 'GFS1' */, recordIndex, surfaceId, width,
                              height, stride, format, 0};
  WriteU32s(g_surfFile, header, 8);
  fwrite(data, 1, static_cast<size_t>(bytes), g_surfFile);
  g_surfBytes += sizeof(header) + bytes;
}

std::string GfxDumpHistogramSummary() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string out;
  for (size_t i = 0; i < 0x20; ++i) {
    if (g_hist[i] == 0) {
      continue;
    }
    if (!out.empty()) {
      out += " ";
    }
    out += CmdName(static_cast<uint16_t>(i));
    out += "=";
    out += std::to_string(g_hist[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kGfxMagic = 0x31584647u;         // 'GFX1'
constexpr uint32_t kFrameHashMagic = 0x31484647u;   // 'GFH1'
constexpr uint32_t kFullSurfaceMagic = 0x31534647u; // 'GFS1'
constexpr size_t kRecordHeaderBytes = 10 * sizeof(uint32_t);
constexpr size_t kBaselineHeaderBytes = 8 * sizeof(uint32_t);

uint32_t Rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return {};
  }
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> b(static_cast<size_t>(n));
  f.read(reinterpret_cast<char*>(b.data()), n);
  return b;
}

}  // namespace

bool GfxCapture::Open(const std::string& path) {
  data_ = ReadFile(path);
  pos_ = 0;
  recordsRead_ = 0;
  path_ = path;
  return !data_.empty();
}

bool GfxCapture::Next(GfxCaptureRecord* out) {
  if (out == nullptr || pos_ + kRecordHeaderBytes > data_.size() ||
      Rd32(&data_[pos_]) != kGfxMagic) {
    return false;
  }
  const uint32_t paramsLen = Rd32(&data_[pos_ + 32]);
  const uint32_t payloadLen = Rd32(&data_[pos_ + 36]);
  if (pos_ + kRecordHeaderBytes + paramsLen + payloadLen > data_.size()) {
    return false;
  }
  out->index = Rd32(&data_[pos_ + 4]);
  out->cmdId = static_cast<uint16_t>(Rd32(&data_[pos_ + 8]));
  out->surfaceId = Rd32(&data_[pos_ + 12]);
  for (int i = 0; i < 4; ++i) {
    out->scalars[i] = Rd32(&data_[pos_ + 16 + i * 4]);
  }
  out->paramsLen = paramsLen;
  out->payloadLen = payloadLen;
  out->params = paramsLen > 0 ? &data_[pos_ + kRecordHeaderBytes] : nullptr;
  out->payload = payloadLen > 0 ? &data_[pos_ + kRecordHeaderBytes + paramsLen] : nullptr;
  pos_ += kRecordHeaderBytes + paramsLen + payloadLen;
  recordsRead_++;
  return true;
}

bool GfxSurfaceBaselines::Load(const std::string& path) {
  const std::vector<uint8_t> bytes = ReadFile(path);
  size_t pos = 0;
  while (pos + kBaselineHeaderBytes <= bytes.size()) {
    const uint32_t magic = Rd32(&bytes[pos]);
    if (magic == kFrameHashMagic) {
      const uint32_t index = Rd32(&bytes[pos + 4]);
      const uint32_t sid = Rd32(&bytes[pos + 8]);
      const uint64_t hash = static_cast<uint64_t>(Rd32(&bytes[pos + 24])) |
                            (static_cast<uint64_t>(Rd32(&bytes[pos + 28])) << 32);
      hashes[index][sid] = hash;
      pos += kBaselineHeaderBytes;
      continue;
    }
    if (magic == kFullSurfaceMagic) {
      GfxSurfaceBaseline s;
      const uint32_t index = Rd32(&bytes[pos + 4]);
      s.id = Rd32(&bytes[pos + 8]);
      s.width = Rd32(&bytes[pos + 12]);
      s.height = Rd32(&bytes[pos + 16]);
      s.stride = Rd32(&bytes[pos + 20]);
      s.format = Rd32(&bytes[pos + 24]);
      const size_t bytesPerSurface = static_cast<size_t>(s.stride) * s.height;
      if (pos + kBaselineHeaderBytes + bytesPerSurface > bytes.size()) {
        break;
      }
      s.data.assign(bytes.begin() + pos + kBaselineHeaderBytes,
                    bytes.begin() + pos + kBaselineHeaderBytes + bytesPerSurface);
      full[index].push_back(std::move(s));
      pos += kBaselineHeaderBytes + bytesPerSurface;
      continue;
    }
    break;
  }
  return !empty();
}

const GfxSurfaceBaseline* GfxSurfaceBaselines::FindFull(uint32_t recordIndex,
                                                        uint32_t surfaceId) const {
  const auto it = full.find(recordIndex);
  if (it == full.end()) {
    return nullptr;
  }
  for (const GfxSurfaceBaseline& s : it->second) {
    if (s.id == surfaceId) {
      return &s;
    }
  }
  return nullptr;
}

uint64_t GfxCaptureHash(const uint8_t* data, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

GfxFrameComparison GfxCompareFrame(uint32_t recordIndex, const GfxSurfaceBaselines& baselines,
                                   const GfxSurfaceReader& read) {
  GfxFrameComparison cmp;
  const auto hashIt = baselines.hashes.find(recordIndex);
  const auto fullIt = baselines.full.find(recordIndex);
  if (hashIt == baselines.hashes.end() && fullIt == baselines.full.end()) {
    return cmp;
  }
  cmp.hadBaseline = true;

  auto compareOne = [&](uint32_t sid, bool haveHash, uint64_t hash,
                        const GfxSurfaceBaseline* full) {
    cmp.surfaceCount++;
    GfxSurfacePixels px;
    if (!read(sid, &px) || px.data == nullptr) {
      cmp.mismatch++;  // surface missing from the engine
      return;
    }
    if (full != nullptr) {
      if (px.width != full->width || px.height != full->height || px.stride != full->stride ||
          full->data.size() != static_cast<size_t>(px.stride) * px.height) {
        cmp.mismatch++;  // dimension/stride disagreement
        return;
      }
      const size_t bytesPerSurface = full->data.size();
      cmp.surfacesCompared++;
      cmp.comparedBytes += bytesPerSurface;
      if (std::memcmp(px.data, full->data.data(), bytesPerSurface) != 0) {
        for (size_t i = 0; i < bytesPerSurface; ++i) {
          const int d = static_cast<int>(px.data[i]) - static_cast<int>(full->data[i]);
          cmp.sumAbs += (d < 0 ? -d : d);
          if (d != 0) {
            cmp.mismatch++;
          }
        }
      }
      return;
    }
    if (haveHash) {
      cmp.surfacesHashed++;
      const uint64_t actual =
          GfxCaptureHash(px.data, static_cast<size_t>(px.stride) * px.height);
      if (actual != hash) {
        cmp.hashMismatch++;
        cmp.mismatch++;
      }
    }
  };

  if (hashIt != baselines.hashes.end()) {
    for (const auto& kv : hashIt->second) {
      compareOne(kv.first, true, kv.second, baselines.FindFull(recordIndex, kv.first));
    }
  } else {
    for (const GfxSurfaceBaseline& ref : fullIt->second) {
      compareOne(ref.id, false, 0, &ref);
    }
  }
  return cmp;
}

}  // namespace hmrdp
