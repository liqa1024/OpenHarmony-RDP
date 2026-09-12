/*
 * HmRdp - CPU reference GFX desktop / surface model (PERF-TODO §2.5 "B1").
 * See hmrdp_gfx_desktop.h for the design notes and alignment rules.
 */
#include "hmrdp_gfx_desktop.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace hmrdp {
namespace {

constexpr uint16_t kCmdWireToSurface = 0x0001;
constexpr uint16_t kCmdDeleteEncodingContext = 0x0003;
constexpr uint16_t kCmdSolidFill = 0x0004;
constexpr uint16_t kCmdSurfaceToSurface = 0x0005;
constexpr uint16_t kCmdSurfaceToCache = 0x0006;
constexpr uint16_t kCmdCacheToSurface = 0x0007;
constexpr uint16_t kCmdEvictCacheEntry = 0x0008;
constexpr uint16_t kCmdCreateSurface = 0x0009;
constexpr uint16_t kCmdDeleteSurface = 0x000A;
constexpr uint16_t kCmdStartFrame = 0x000B;
constexpr uint16_t kCmdEndFrame = 0x000C;
constexpr uint16_t kCmdResetGraphics = 0x000E;
constexpr uint16_t kCmdMapSurfaceToOutput = 0x000F;

constexpr uint32_t kCodecUncompressed = 0x0000;
constexpr uint32_t kCodecRemoteFx = 0x0003;
constexpr uint32_t kCodecClearCodec = 0x0008;
constexpr uint32_t kCodecCaprogressive = 0x0009;
constexpr uint32_t kCodecCaprogressiveV2 = 0x000D;

// Rounds up to a multiple of `alignment` (mirrors gfx_align_scanline).
int Align(int value, int alignment) {
  const int pad = alignment - (value % alignment);
  return (pad == alignment) ? value : value + pad;
}

uint32_t Rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t Rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
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

// Copies a w x h region, handling overlapping source/destination in the same
// buffer by choosing the row direction (FreeRDP uses freerdp_image_copy).
void CopyRegion(uint8_t* dst, int dstStride, int dstX, int dstY, const uint8_t* src, int srcStride,
                int srcX, int srcY, int w, int h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const bool sameBuffer = (dst == src);
  const bool overlap = sameBuffer && dstY > srcY && dstY < srcY + h;
  if (overlap) {
    for (int row = h - 1; row >= 0; --row) {
      std::memmove(dst + static_cast<size_t>(dstY + row) * dstStride + dstX * 4,
                   src + static_cast<size_t>(srcY + row) * srcStride + srcX * 4,
                   static_cast<size_t>(w) * 4);
    }
  } else {
    for (int row = 0; row < h; ++row) {
      std::memmove(dst + static_cast<size_t>(dstY + row) * dstStride + dstX * 4,
                   src + static_cast<size_t>(srcY + row) * srcStride + srcX * 4,
                   static_cast<size_t>(w) * 4);
    }
  }
}

// Clips [x, x+w) x [y, y+h) to [0, limitW) x [0, limitH); returns false when the
// intersection is empty. `x`/`y` are updated in place.
bool ClipRect(int* x, int* y, int* w, int* h, int limitW, int limitH) {
  if (*x < 0) {
    *w += *x;
    *x = 0;
  }
  if (*y < 0) {
    *h += *y;
    *y = 0;
  }
  if (*x + *w > limitW) {
    *w = limitW - *x;
  }
  if (*y + *h > limitH) {
    *h = limitH - *y;
  }
  return *w > 0 && *h > 0;
}

}  // namespace

GfxDesktop::GfxDesktop(GfxClearDecoder* clearDecoder) : clear_(clearDecoder) {}
GfxDesktop::~GfxDesktop() = default;

void GfxDesktop::Reset() {
  surfaces_.clear();
  cache_.clear();
  stats_ = GfxDesktopStats();
}

const GfxSurface* GfxDesktop::FindSurface(uint32_t id) const {
  const auto it = surfaces_.find(id);
  return it == surfaces_.end() ? nullptr : &it->second;
}

GfxSurface* GfxDesktop::EnsureSurface(uint32_t id) {
  const auto it = surfaces_.find(id);
  return it == surfaces_.end() ? nullptr : &it->second;
}

RfxTileState* GfxDesktop::TileState(GfxSurface* surface, int xIdx, int yIdx) {
  if (surface == nullptr || xIdx < 0 || yIdx < 0 || xIdx >= surface->gridW ||
      yIdx >= surface->gridH) {
    return nullptr;
  }
  const size_t index = static_cast<size_t>(yIdx) * surface->gridW + xIdx;
  if (surface->tiles.size() != static_cast<size_t>(surface->gridW) * surface->gridH) {
    surface->tiles.resize(static_cast<size_t>(surface->gridW) * surface->gridH);
  }
  if (surface->tiles[index] == nullptr) {
    surface->tiles[index].reset(new RfxTileState());
  }
  return surface->tiles[index].get();
}

void GfxDesktop::FillRects(GfxSurface& surface, uint32_t pixel, const uint8_t* params,
                           uint32_t rectCount) {
  const uint8_t b = static_cast<uint8_t>(pixel & 0xFFu);
  const uint8_t g = static_cast<uint8_t>((pixel >> 8) & 0xFFu);
  const uint8_t r = static_cast<uint8_t>((pixel >> 16) & 0xFFu);
  for (uint32_t i = 0; i < rectCount; ++i) {
    const uint8_t* rect = params + static_cast<size_t>(i) * 8;
    int left = Rd16(rect);
    int top = Rd16(rect + 2);
    int right = Rd16(rect + 4);
    int bottom = Rd16(rect + 6);
    if (right > surface.width) {
      right = surface.width;
    }
    if (bottom > surface.height) {
      bottom = surface.height;
    }
    for (int y = top; y < bottom; ++y) {
      uint8_t* row = surface.data.data() + static_cast<size_t>(y) * surface.stride + left * 4;
      for (int x = left; x < right; ++x) {
        row[0] = b;
        row[1] = g;
        row[2] = r;
        row[3] = 0xFF;
        row += 4;
      }
    }
  }
}

void GfxDesktop::Blit(const GfxSurface& src, int srcX, int srcY, GfxSurface& dst, int dstX,
                      int dstY, int width, int height) {
  int x = dstX;
  int y = dstY;
  int w = width;
  int h = height;
  if (!ClipRect(&x, &y, &w, &h, dst.width, dst.height)) {
    return;
  }
  // The source offset follows the same clipped amount.
  const int sx = srcX + (x - dstX);
  const int sy = srcY + (y - dstY);
  if (sx < 0 || sy < 0 || sx + w > src.width || sy + h > src.height) {
    return;
  }
  CopyRegion(dst.data.data(), dst.stride, x, y, src.data.data(), src.stride, sx, sy, w, h);
}

void GfxDesktop::ApplyCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                              const uint8_t* params, uint32_t paramsLen, const uint8_t* payload,
                              uint32_t payloadLen) {
  stats_.records++;
  const int previousStoreSlot = lastCacheStoreSlot_;
  lastCacheStoreSlot_ = -1;
  switch (cmdId) {
    case kCmdCreateSurface: {
      if (scalars == nullptr) {
        break;
      }
      GfxSurface surface;
      surface.id = surfaceId;
      surface.width = Align(static_cast<int>(scalars[0]), 16);
      surface.height = Align(static_cast<int>(scalars[1]), 16);
      surface.stride = Align(surface.width * 4, 16);
      surface.format = (scalars[2] == 0x20u) ? kPixelFormatBgrx32 : kPixelFormatBgra32;
      surface.data.assign(static_cast<size_t>(surface.stride) * surface.height, 0xFF);
      surface.gridW = (surface.width + 63) / 64;
      surface.gridH = (surface.height + 63) / 64;
      surfaces_[surfaceId] = std::move(surface);
      stats_.created++;
      break;
    }
    case kCmdDeleteSurface:
      surfaces_.erase(surfaceId);
      stats_.deleted++;
      break;
    case kCmdSolidFill: {
      GfxSurface* surface = EnsureSurface(surfaceId);
      if (surface != nullptr && scalars != nullptr && params != nullptr) {
        FillRects(*surface, scalars[0], params, scalars[1]);
        stats_.solidFill++;
      }
      break;
    }
    case kCmdSurfaceToSurface: {
      if (scalars == nullptr || params == nullptr || paramsLen < 8) {
        break;
      }
      GfxSurface* dst = EnsureSurface(surfaceId);
      GfxSurface* src = EnsureSurface(scalars[0]);
      if (dst == nullptr || src == nullptr) {
        break;
      }
      const int sx = Rd16(params);
      const int sy = Rd16(params + 2);
      const int w = Rd16(params + 4) - sx;
      const int h = Rd16(params + 6) - sy;
      uint32_t count = scalars[1];
      if ((paramsLen - 8) / 4 < count) {
        count = (paramsLen - 8) / 4;
      }
      for (uint32_t i = 0; i < count; ++i) {
        const int px = Rd16(params + 8 + static_cast<size_t>(i) * 4);
        const int py = Rd16(params + 8 + static_cast<size_t>(i) * 4 + 2);
        Blit(*src, sx, sy, *dst, px, py, w, h);
      }
      stats_.surfaceToSurface++;
      break;
    }
    case kCmdSurfaceToCache: {
      if (scalars == nullptr || params == nullptr || paramsLen < 16) {
        break;
      }
      GfxSurface* src = EnsureSurface(surfaceId);
      if (src == nullptr) {
        break;
      }
      const int sx = Rd16(params + 8);
      const int sy = Rd16(params + 10);
      const int w = Rd16(params + 12) - sx;
      const int h = Rd16(params + 14) - sy;
      if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || sx + w > src->width ||
          sy + h > src->height) {
        break;
      }
      CacheEntry entry;
      entry.width = w;
      entry.height = h;
      entry.stride = Align(w * 4, 16);
      entry.data.assign(static_cast<size_t>(entry.stride) * h, 0);
      for (int row = 0; row < h; ++row) {
        std::memcpy(entry.data.data() + static_cast<size_t>(row) * entry.stride,
                    src->data.data() + static_cast<size_t>(sy + row) * src->stride + sx * 4,
                    static_cast<size_t>(w) * 4);
      }
      cache_[scalars[0]] = std::move(entry);
      lastCacheStoreSlot_ = static_cast<int>(scalars[0]);
      stats_.surfaceToCache++;
      break;
    }
    case kCmdCacheToSurface: {
      if (scalars == nullptr || params == nullptr) {
        break;
      }
      GfxSurface* dst = EnsureSurface(surfaceId);
      const auto it = cache_.find(scalars[0]);
      if (dst == nullptr || it == cache_.end()) {
        break;
      }
      const CacheEntry& entry = it->second;
      uint32_t count = scalars[1];
      if (paramsLen / 4 < count) {
        count = paramsLen / 4;
      }
      for (uint32_t i = 0; i < count; ++i) {
        const int px = Rd16(params + static_cast<size_t>(i) * 4);
        const int py = Rd16(params + static_cast<size_t>(i) * 4 + 2);
        int x = px;
        int y = py;
        int w = entry.width;
        int h = entry.height;
        if (!ClipRect(&x, &y, &w, &h, dst->width, dst->height)) {
          continue;
        }
        for (int row = 0; row < h; ++row) {
          std::memcpy(dst->data.data() + static_cast<size_t>(y + row) * dst->stride + x * 4,
                      entry.data.data() + static_cast<size_t>(y - py + row) * entry.stride +
                          (x - px) * 4,
                      static_cast<size_t>(w) * 4);
        }
      }
      stats_.cacheToSurface++;
      break;
    }
    case kCmdEvictCacheEntry:
      if (scalars != nullptr) {
        // An EvictCacheEntry emitted by SurfaceToCache's internal call must not
        // drop the entry that command just stored.
        if (static_cast<int>(scalars[0]) != previousStoreSlot) {
          cache_.erase(scalars[0]);
          stats_.evicted++;
        }
      }
      break;
    case kCmdMapSurfaceToOutput: {
      GfxSurface* surface = EnsureSurface(surfaceId);
      if (surface != nullptr && scalars != nullptr) {
        surface->mapped = true;
        surface->outputX = scalars[0];
        surface->outputY = scalars[1];
      }
      break;
    }
    case kCmdWireToSurface:
      if (params != nullptr && paramsLen >= 32 && scalars != nullptr) {
        ApplyWireToSurface(surfaceId, params, scalars[0], payload, payloadLen);
      }
      break;
    case kCmdDeleteEncodingContext:
    case kCmdStartFrame:
    case kCmdEndFrame:
    case kCmdResetGraphics:
    default:
      break;
  }
}

void GfxDesktop::ApplyWireToSurface(uint32_t surfaceId, const uint8_t* params, uint32_t codecId,
                                    const uint8_t* payload, uint32_t payloadLen) {
  GfxSurface* surface = EnsureSurface(surfaceId);
  if (surface == nullptr) {
    stats_.unsupportedCodecs++;
    return;
  }
  const uint32_t format = Rd32(params + 4);
  const int left = static_cast<int>(Rd32(params + 8));
  const int top = static_cast<int>(Rd32(params + 12));
  const int width = static_cast<int>(Rd32(params + 24));
  const int height = static_cast<int>(Rd32(params + 28));

  switch (codecId) {
    case kCodecCaprogressive:
    case kCodecCaprogressiveV2:
      stats_.progressive++;
      ApplyProgressive(surface, payload, payloadLen);
      break;
    case kCodecClearCodec:
      stats_.clearCodec++;
      if (clear_ != nullptr && payload != nullptr) {
        clear_->Decode(payload, payloadLen, width, height, surface->format, surface->data.data(),
                       surface->stride, left, top, surface->width, surface->height);
      }
      break;
    case kCodecUncompressed:
      stats_.uncompressed++;
      ApplyUncompressed(surface, format, left, top, width, height, payload, payloadLen);
      break;
    case kCodecRemoteFx:
    default:
      stats_.unsupportedCodecs++;
      break;
  }
}

void GfxDesktop::ApplyUncompressed(GfxSurface* surface, uint32_t srcFormat, int left, int top,
                                   int width, int height, const uint8_t* payload,
                                   uint32_t payloadLen) {
  if (surface == nullptr || payload == nullptr || width <= 0 || height <= 0) {
    return;
  }
  const uint32_t bpp = srcFormat >> 24;
  int x = left;
  int y = top;
  int w = width;
  int h = height;
  if (!ClipRect(&x, &y, &w, &h, surface->width, surface->height)) {
    return;
  }
  const int srcBpp = (bpp == 24) ? 3 : 4;
  if (static_cast<uint64_t>(srcBpp) * width * height > payloadLen) {
    return;
  }
  for (int row = 0; row < h; ++row) {
    const uint8_t* srcRow =
        payload + static_cast<size_t>(y - top + row) * width * srcBpp + (x - left) * srcBpp;
    uint8_t* dstRow = surface->data.data() + static_cast<size_t>(y + row) * surface->stride + x * 4;
    if (srcBpp == 4) {
      std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
    } else {
      for (int col = 0; col < w; ++col) {
        dstRow[col * 4] = srcRow[col * 3];
        dstRow[col * 4 + 1] = srcRow[col * 3 + 1];
        dstRow[col * 4 + 2] = srcRow[col * 3 + 2];
        dstRow[col * 4 + 3] = 0xFF;
      }
    }
  }
}

void GfxDesktop::ApplyProgressive(GfxSurface* surface, const uint8_t* payload, uint32_t payloadLen) {
  if (surface == nullptr || payload == nullptr || payloadLen == 0) {
    return;
  }
  uint8_t tile[64 * 64 * 4];
  RfxParseStats parseStats;
  ParseRfxProgressive(
      payload, payloadLen,
      [&](const RfxTileRef& t) {
        stats_.progressiveTiles++;
        if (t.type == RfxTileType::kSimple) {
          // Not supported yet (PERF-TODO §2.7 #1); the captured server does not
          // use SIMPLE tiles.
          stats_.progressiveSimple++;
          return;
        }
        RfxTileState* state = TileState(surface, t.xIdx, t.yIdx);
        const bool ok = (t.type == RfxTileType::kUpgrade)
                            ? DecodeTileUpgrade(t, tile, 64 * 4, state)
                            : DecodeTileFirst(t, tile, 64 * 4, state);
        if (!ok) {
          stats_.errors++;
          return;
        }
        // Composite only inside the region's clip rects (FreeRDP writes
        // rect ∩ tile; the rest of the tile keeps the previous surface).
        const int ox = t.xIdx * 64;
        const int oy = t.yIdx * 64;
        auto copyRect = [&](int rl, int rt, int rr, int rb) {
          int x = rl;
          int y = rt;
          int w = rr - rl;
          int h = rb - rt;
          if (!ClipRect(&x, &y, &w, &h, surface->width, surface->height)) {
            return;
          }
          for (int row = 0; row < h; ++row) {
            std::memcpy(surface->data.data() + static_cast<size_t>(y + row) * surface->stride +
                            x * 4,
                        tile + static_cast<size_t>(y - oy + row) * (64 * 4) + (x - ox) * 4,
                        static_cast<size_t>(w) * 4);
          }
        };
        if (t.numRects == 0) {
          copyRect(ox, oy, ox + 64, oy + 64);
        } else {
          for (uint16_t i = 0; i < t.numRects; ++i) {
            const RfxRect& r = t.rects[i];
            const int rl = std::max<int>(r.x, ox);
            const int rt = std::max<int>(r.y, oy);
            const int rr = std::min<int>(r.x + r.width, ox + 64);
            const int rb = std::min<int>(r.y + r.height, oy + 64);
            if (rr > rl && rb > rt) {
              copyRect(rl, rt, rr, rb);
            }
          }
        }
      },
      &parseStats);
  if (parseStats.errors != 0) {
    stats_.errors += parseStats.errors;
  }
}

// ---------------------------------------------------------------------------
// Offline replay / alignment
// ---------------------------------------------------------------------------

namespace {

struct BaselineSurface {
  uint32_t id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t format = 0;
  std::vector<uint8_t> data;
};

// Must match hmrdp_gfx_dump.cpp's per-frame hash (FNV-1a 64).
uint64_t HashBytes(const uint8_t* data, size_t size) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

GfxDesktopSelfTestResult ReplayGfxCapture(GfxDesktop* desktop, const std::string& gfxPath,
                                          const std::string& surfacePath) {
  GfxDesktopSelfTestResult res;
  if (desktop == nullptr) {
    res.log = "no desktop";
    return res;
  }
  const std::vector<uint8_t> gfx = ReadFile(gfxPath);
  if (gfx.empty()) {
    res.log = "cannot read gfx capture";
    return res;
  }
  // Baselines are only needed for comparison; a missing file still replays.
  const std::vector<uint8_t> surfaces = ReadFile(surfacePath);
  std::map<uint32_t, std::vector<BaselineSurface>> baselines;
  std::map<uint32_t, std::map<uint32_t, uint64_t>> hashes;
  size_t pos = 0;
  while (pos + 32 <= surfaces.size()) {
    const uint32_t magic = Rd32(&surfaces[pos]);
    if (magic == 0x31484647u /* 'GFH1' */) {
      const uint32_t index = Rd32(&surfaces[pos + 4]);
      const uint32_t sid = Rd32(&surfaces[pos + 8]);
      const uint64_t hash = static_cast<uint64_t>(Rd32(&surfaces[pos + 24])) |
                            (static_cast<uint64_t>(Rd32(&surfaces[pos + 28])) << 32);
      hashes[index][sid] = hash;
      pos += 32;
      continue;
    }
    if (magic == 0x31534647u /* 'GFS1' */) {
      BaselineSurface s;
      const uint32_t index = Rd32(&surfaces[pos + 4]);
      s.id = Rd32(&surfaces[pos + 8]);
      s.width = Rd32(&surfaces[pos + 12]);
      s.height = Rd32(&surfaces[pos + 16]);
      s.stride = Rd32(&surfaces[pos + 20]);
      s.format = Rd32(&surfaces[pos + 24]);
      const size_t bytes = static_cast<size_t>(s.stride) * s.height;
      if (pos + 32 + bytes > surfaces.size()) {
        break;
      }
      s.data.assign(surfaces.begin() + pos + 32, surfaces.begin() + pos + 32 + bytes);
      baselines[index].push_back(std::move(s));
      pos += 32 + bytes;
      continue;
    }
    break;
  }

  desktop->Reset();
  res.ran = true;
  pos = 0;
  int64_t totalAbs = 0;
  char line[256];
  while (pos + 40 <= gfx.size() && Rd32(&gfx[pos]) == 0x31584647u /* 'GFX1' */) {
    const uint32_t index = Rd32(&gfx[pos + 4]);
    const uint16_t cmdId = static_cast<uint16_t>(Rd32(&gfx[pos + 8]));
    const uint32_t surfaceId = Rd32(&gfx[pos + 12]);
    uint32_t scalars[4] = {Rd32(&gfx[pos + 16]), Rd32(&gfx[pos + 20]), Rd32(&gfx[pos + 24]),
                           Rd32(&gfx[pos + 28])};
    const uint32_t paramsLen = Rd32(&gfx[pos + 32]);
    const uint32_t payloadLen = Rd32(&gfx[pos + 36]);
    if (pos + 40 + paramsLen + payloadLen > gfx.size()) {
      break;
    }
    const uint8_t* params = paramsLen > 0 ? &gfx[pos + 40] : nullptr;
    const uint8_t* payload = payloadLen > 0 ? &gfx[pos + 40 + paramsLen] : nullptr;
    desktop->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
    res.records++;

    if (cmdId == kCmdEndFrame) {
      const auto hIt = hashes.find(index);
      const auto bIt = baselines.find(index);
      if (hIt != hashes.end() || bIt != baselines.end()) {
        uint64_t mism = 0;
        uint64_t cmp = 0;
        uint64_t hashed = 0;
        uint64_t hashMism = 0;
        int64_t sumAbs = 0;
        size_t surfaceCount = 0;
        auto compareFull = [&](const GfxSurface& ours, const BaselineSurface& ref) {
          if (static_cast<uint32_t>(ours.width) != ref.width ||
              static_cast<uint32_t>(ours.height) != ref.height ||
              static_cast<uint32_t>(ours.stride) != ref.stride ||
              ref.data.size() != ours.data.size()) {
            mism++;
            return;
          }
          const size_t bytes = ours.data.size();
          if (std::memcmp(ours.data.data(), ref.data.data(), bytes) != 0) {
            for (size_t i = 0; i < bytes; ++i) {
              const int d = static_cast<int>(ours.data[i]) - static_cast<int>(ref.data[i]);
              const int ad = d < 0 ? -d : d;
              sumAbs += ad;
              if (ad > 0) {
                mism++;
              }
            }
          }
          cmp += bytes;
          res.surfacesCompared++;
        };

        if (hIt != hashes.end()) {
          for (const auto& kv : hIt->second) {
            const GfxSurface* ours = desktop->FindSurface(kv.first);
            surfaceCount++;
            if (ours == nullptr) {
              mism++;
              continue;
            }
            const BaselineSurface* full = nullptr;
            if (bIt != baselines.end()) {
              for (const BaselineSurface& b : bIt->second) {
                if (b.id == kv.first) {
                  full = &b;
                  break;
                }
              }
            }
            if (full != nullptr) {
              compareFull(*ours, *full);
            } else {
              const uint64_t h = HashBytes(ours->data.data(), ours->data.size());
              hashed++;
              if (h != kv.second) {
                hashMism++;
                mism++;
              }
            }
          }
        } else if (bIt != baselines.end()) {
          for (const BaselineSurface& ref : bIt->second) {
            const GfxSurface* ours = desktop->FindSurface(ref.id);
            surfaceCount++;
            if (ours == nullptr) {
              mism++;
              continue;
            }
            compareFull(*ours, ref);
          }
        }
        res.comparedRecords++;
        res.compared += cmp;
        res.mismatch += mism;
        res.surfacesHashed += hashed;
        res.hashMismatch += hashMism;
        totalAbs += sumAbs;
        if (mism != 0) {
          res.badRecords++;
        }
        std::snprintf(line, sizeof(line),
                      "rec%u surf=%zu cmp=%llu mism=%llu hash=%llu hMism=%llu", index,
                      surfaceCount, static_cast<unsigned long long>(cmp),
                      static_cast<unsigned long long>(mism),
                      static_cast<unsigned long long>(hashed),
                      static_cast<unsigned long long>(hashMism));
        res.log += line;
        res.log += "\n";
      }
    }
    pos += 40 + paramsLen + payloadLen;
  }
  res.meanAbs = res.compared ? static_cast<double>(totalAbs) / static_cast<double>(res.compared)
                             : 0.0;
  res.ok = res.ran && res.badRecords == 0 && res.comparedRecords > 0;
  return res;
}

}  // namespace hmrdp
