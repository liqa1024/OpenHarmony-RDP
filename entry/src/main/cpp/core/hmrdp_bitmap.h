/*
 * HmRdp - HarmonyOS RDP client
 * Protocol-agnostic BGRA bitmap kernels (cursor resampling) and content
 * hashing. The policy (target size, when to scale, dedup) lives in ArkTS;
 * these are the numeric primitives it drives (doc_agent/session-and-input.md).
 */
#ifndef HMRDP_BITMAP_H
#define HMRDP_BITMAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hmrdp {

// Downscale by area averaging on premultiplied alpha (un-premultiplied
// afterwards): without premultiplying, transparent pixels (RGB 0) bleed dark
// fringes into the edges.
void DownscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                   uint32_t dstW, uint32_t dstH, std::vector<uint8_t>* dst);

// Upscale by a genuine resample (premultiplied Lanczos3), optionally followed
// by a contrast-adaptive sharpen (5-tap unsharp mask clamped to the local
// neighbourhood); `sharpen` <= 0 skips the pass.
void UpscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                 uint32_t dstW, uint32_t dstH, float sharpen, std::vector<uint8_t>* dst);

// FNV-1a over the bitmap bytes: a cheap content key the ArkTS cursor installer
// uses to drop identical resends without pulling the bitmap across NAPI.
uint32_t HashBgra(const uint8_t* data, size_t size);

}  // namespace hmrdp

#endif  // HMRDP_BITMAP_H
