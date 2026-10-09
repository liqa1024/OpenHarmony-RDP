#ifndef HMRDP_BITMAP_H
#define HMRDP_BITMAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hmrdp {

// Downscale by area averaging on premultiplied alpha (un-premultiplied
// afterwards); premultiplying keeps transparent pixels from bleeding dark
// fringes into the edges.
void DownscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                   uint32_t dstW, uint32_t dstH, std::vector<uint8_t>* dst);

void UpscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                 uint32_t dstW, uint32_t dstH, float sharpen, std::vector<uint8_t>* dst);

uint32_t HashBgra(const uint8_t* data, size_t size);

}  // namespace hmrdp

#endif  // HMRDP_BITMAP_H
