/*
 * HmRdp - shared reader for captured RDPGFX data (see hmrdp_gfx_capture.h).
 */
#include "hmrdp_gfx_capture.h"

#include <cstring>
#include <fstream>
#include <utility>

namespace hmrdp {
namespace {

constexpr uint32_t kGfxMagic = 0x31584647u;      // 'GFX1'
constexpr uint32_t kFrameHashMagic = 0x31484647u;  // 'GFH1'
constexpr uint32_t kFullSurfaceMagic = 0x31534647u;  // 'GFS1'
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
