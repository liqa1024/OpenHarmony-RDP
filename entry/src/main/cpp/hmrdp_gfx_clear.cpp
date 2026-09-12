/*
 * HmRdp - FreeRDP glue for the B1 GFX desktop reference.
 *
 * The portable desktop model (hmrdp_gfx_desktop.cpp) deliberately does not
 * depend on FreeRDP. This file supplies the one piece that does: the ClearCodec
 * decoder, wired to FreeRDP's clear_decompress (which also covers NSCodec, RLEX,
 * glyph/VBar caches - see PERF-TODO §2.8), and exposes the device-side replay
 * entry point.
 */
#include <freerdp/codec/clear.h>

#include "hmrdp_gfx_desktop.h"
#include "hmrdp_log.h"

namespace hmrdp {
namespace {

class FreeRdpClearDecoder : public GfxClearDecoder {
 public:
  FreeRdpClearDecoder() : clear_(clear_context_new(0 /* compressor = FALSE */)) {}
  ~FreeRdpClearDecoder() override {
    if (clear_ != nullptr) {
      clear_context_free(clear_);
      clear_ = nullptr;
    }
  }

  FreeRdpClearDecoder(const FreeRdpClearDecoder&) = delete;
  FreeRdpClearDecoder& operator=(const FreeRdpClearDecoder&) = delete;

  bool Decode(const uint8_t* src, size_t size, int width, int height, uint32_t dstFormat,
              uint8_t* dst, int stride, int xDst, int yDst, int dstW, int dstH) override {
    if (clear_ == nullptr || src == nullptr || dst == nullptr) {
      return false;
    }
    const INT32 rc =
        clear_decompress(clear_, src, static_cast<UINT32>(size), static_cast<UINT32>(width),
                         static_cast<UINT32>(height), dst, dstFormat, static_cast<UINT32>(stride),
                         static_cast<UINT32>(xDst), static_cast<UINT32>(yDst),
                         static_cast<UINT32>(dstW), static_cast<UINT32>(dstH), nullptr);
    return rc >= 0;
  }

 private:
  CLEAR_CONTEXT* clear_ = nullptr;
};

}  // namespace

std::unique_ptr<GfxClearDecoder> CreateFreeRdpClearDecoder() {
  return std::unique_ptr<GfxClearDecoder>(new FreeRdpClearDecoder());
}

GfxDesktopSelfTestResult RunGfxDesktopSelfTest(const std::string& gfxPath,
                                               const std::string& surfacePath) {
  FreeRdpClearDecoder clear;
  GfxDesktop desktop(&clear);
  GfxDesktopSelfTestResult res = ReplayGfxCapture(&desktop, gfxPath, surfacePath);
  const GfxDesktopStats& stats = desktop.stats();
  HMRDP_LOGI("gfx desktop selftest: ran=%{public}d ok=%{public}d rec=%{public}u "
             "cmpRec=%{public}u badRec=%{public}u mism=%{public}llu hashOK=%{public}llu "
             "hashMism=%{public}llu meanAbs=%{public}d (mAbs*1000) clear=%{public}u prog=%{public}u "
             "tiles=%{public}u simple=%{public}u unc=%{public}u fill=%{public}u s2s=%{public}u "
             "s2c=%{public}u c2s=%{public}u evict=%{public}u unsup=%{public}u err=%{public}u",
             res.ran ? 1 : 0, res.ok ? 1 : 0, res.records, res.comparedRecords, res.badRecords,
             static_cast<unsigned long long>(res.mismatch),
             static_cast<unsigned long long>(res.surfacesHashed),
             static_cast<unsigned long long>(res.hashMismatch),
             static_cast<int>(res.meanAbs * 1000.0), stats.clearCodec, stats.progressive,
             stats.progressiveTiles, stats.progressiveSimple, stats.uncompressed, stats.solidFill,
             stats.surfaceToSurface, stats.surfaceToCache, stats.cacheToSurface, stats.evicted,
             stats.unsupportedCodecs, stats.errors);
  return res;
}

}  // namespace hmrdp
