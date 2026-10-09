/*
 * HmRdp - HarmonyOS RDP client
 * Protocol-agnostic BGRA bitmap kernels and content hashing.
 */
#include "hmrdp_bitmap.h"

#include <algorithm>
#include <cmath>

namespace hmrdp {

void DownscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                   uint32_t dstW, uint32_t dstH, std::vector<uint8_t>* dst) {
  dst->assign(static_cast<size_t>(dstW) * dstH * 4, 0);
  const double scaleX = static_cast<double>(srcW) / dstW;
  const double scaleY = static_cast<double>(srcH) / dstH;
  for (uint32_t dy = 0; dy < dstH; dy++) {
    const uint32_t y0 = static_cast<uint32_t>(dy * scaleY);
    uint32_t y1 = static_cast<uint32_t>((dy + 1) * scaleY);
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    if (y1 > srcH) {
      y1 = srcH;
    }
    for (uint32_t dx = 0; dx < dstW; dx++) {
      const uint32_t x0 = static_cast<uint32_t>(dx * scaleX);
      uint32_t x1 = static_cast<uint32_t>((dx + 1) * scaleX);
      if (x1 <= x0) {
        x1 = x0 + 1;
      }
      if (x1 > srcW) {
        x1 = srcW;
      }
      uint64_t aSum = 0;
      uint64_t rSum = 0;
      uint64_t gSum = 0;
      uint64_t bSum = 0;
      uint32_t count = 0;
      for (uint32_t y = y0; y < y1; y++) {
        const uint8_t* row = &src[(static_cast<size_t>(y) * srcW + x0) * 4];
        for (uint32_t x = x0; x < x1; x++) {
          const uint32_t alpha = row[3];
          bSum += static_cast<uint64_t>(row[0]) * alpha;
          gSum += static_cast<uint64_t>(row[1]) * alpha;
          rSum += static_cast<uint64_t>(row[2]) * alpha;
          aSum += alpha;
          row += 4;
          count++;
        }
      }
      uint8_t* out = &(*dst)[(static_cast<size_t>(dy) * dstW + dx) * 4];
      if (count == 0 || aSum == 0) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
        continue;
      }
      out[0] = static_cast<uint8_t>(bSum / aSum);
      out[1] = static_cast<uint8_t>(gSum / aSum);
      out[2] = static_cast<uint8_t>(rSum / aSum);
      out[3] = static_cast<uint8_t>(aSum / count);
    }
  }
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// Lanczos window (a = 3). Sharper than a bilinear or bicubic tap at the small
// magnifications 超分 uses (output ÷ session), which is the whole point of
// going through a real resample rather than a stretch.
double Lanczos3Weight(double x) {
  const double ax = x < 0.0 ? -x : x;
  if (ax < 1e-9) {
    return 1.0;
  }
  if (ax >= 3.0) {
    return 0.0;
  }
  const double px = kPi * ax;
  return (std::sin(px) / px) * (std::sin(px / 3.0) / (px / 3.0));
}

// One separable Lanczos pass over RGBA float samples that are already
// premultiplied. `horizontal` picks which axis is resampled; the other keeps
// its extent. Source samples outside the bitmap mirror the edge, so the
// outermost pixels are not pulled towards black.
void ResampleLanczos3Pass(const std::vector<float>& src, int srcW, int srcH, int outW, int outH,
                          bool horizontal, std::vector<float>* dst) {
  dst->assign(static_cast<size_t>(outW) * outH * 4, 0.0f);
  const int inLen = horizontal ? srcW : srcH;
  const int outLen = horizontal ? outW : outH;
  const int otherLen = horizontal ? outH : outW;
  const double scale = static_cast<double>(inLen) / outLen;
  // Widening the kernel while downscaling is what keeps a shrink from aliasing;
  // for a magnification it stays at the base support.
  const double filterScale = scale > 1.0 ? scale : 1.0;
  for (int line = 0; line < otherLen; ++line) {
    for (int i = 0; i < outLen; ++i) {
      const double center = (static_cast<double>(i) + 0.5) * scale - 0.5;
      const int first = static_cast<int>(std::ceil(center - 3.0 * filterScale));
      const int last = static_cast<int>(std::floor(center + 3.0 * filterScale));
      double sum[4] = {0.0, 0.0, 0.0, 0.0};
      double weightSum = 0.0;
      for (int tap = first; tap <= last; ++tap) {
        const double weight = Lanczos3Weight((center - tap) / filterScale);
        if (weight == 0.0) {
          continue;
        }
        int clamped = tap < 0 ? 0 : (tap >= inLen ? inLen - 1 : tap);
        const size_t index = horizontal ? (static_cast<size_t>(line) * srcW + clamped) * 4
                                        : (static_cast<size_t>(clamped) * srcW + line) * 4;
        for (int c = 0; c < 4; ++c) {
          sum[c] += weight * src[index + c];
        }
        weightSum += weight;
      }
      if (weightSum == 0.0) {
        continue;
      }
      const size_t outIndex = horizontal ? (static_cast<size_t>(line) * outW + i) * 4
                                         : (static_cast<size_t>(i) * outW + line) * 4;
      for (int c = 0; c < 4; ++c) {
        double value = sum[c] / weightSum;
        value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
        (*dst)[outIndex + c] = static_cast<float>(value);
      }
    }
  }
}

// Contrast-adaptive sharpening on the premultiplied colour: a 5-tap unsharp
// mask clamped to the local min/max, so an edge is accentuated but can never
// overshoot into a halo. The alpha channel is left alone - sharpening it
// would only ruffle the outline.
void SharpenPremul(std::vector<float>* image, int width, int height, float strength) {
  const std::vector<float> src = *image;
  auto at = [&](int x, int y, int c) -> float {
    const int cx = x < 0 ? 0 : (x >= width ? width - 1 : x);
    const int cy = y < 0 ? 0 : (y >= height ? height - 1 : y);
    return src[(static_cast<size_t>(cy) * width + cx) * 4 + c];
  };
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        const float center = at(x, y, c);
        const float north = at(x, y - 1, c);
        const float south = at(x, y + 1, c);
        const float west = at(x - 1, y, c);
        const float east = at(x + 1, y, c);
        const float low =
            std::min(std::min(center, north), std::min(std::min(south, west), east));
        const float high =
            std::max(std::max(center, north), std::max(std::max(south, west), east));
        if (high - low <= 1e-6f) {
          continue;
        }
        const float laplacian = 4.0f * center - (north + south + west + east);
        float value = center + strength * 0.25f * laplacian;
        value = value < low ? low : (value > high ? high : value);
        (*image)[(static_cast<size_t>(y) * width + x) * 4 + c] = value;
      }
    }
  }
}

}  // namespace

// A genuine resample of the bitmap, not a stretch. Everything happens on the
// *premultiplied* bitmap - the source's transparent surround is RGB 0, and any
// linear filter run on straight-alpha colour mixes that black into the shape's
// edges (the classic dark fringe); premultiplied, the blend stays physically
// correct, and un-premultiplying at the end restores the straight alpha the
// system cursor expects. Lanczos3 recovers the sharpness the reduced source
// scale cost, and the contrast-adaptive sharpen puts the edge back.
void UpscaleBgra(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH,
                 uint32_t dstW, uint32_t dstH, float sharpen, std::vector<uint8_t>* dst) {
  const size_t srcPixels = static_cast<size_t>(srcW) * srcH;
  std::vector<float> premul(srcPixels * 4, 0.0f);
  for (size_t i = 0; i < srcPixels; ++i) {
    const double alpha = src[i * 4 + 3] / 255.0;
    premul[i * 4 + 0] = static_cast<float>(src[i * 4 + 0] * alpha / 255.0);
    premul[i * 4 + 1] = static_cast<float>(src[i * 4 + 1] * alpha / 255.0);
    premul[i * 4 + 2] = static_cast<float>(src[i * 4 + 2] * alpha / 255.0);
    premul[i * 4 + 3] = static_cast<float>(alpha);
  }
  std::vector<float> horizontal;
  ResampleLanczos3Pass(premul, static_cast<int>(srcW), static_cast<int>(srcH),
                       static_cast<int>(dstW), static_cast<int>(srcH), true, &horizontal);
  std::vector<float> upscaled;
  ResampleLanczos3Pass(horizontal, static_cast<int>(dstW), static_cast<int>(srcH),
                       static_cast<int>(dstW), static_cast<int>(dstH), false, &upscaled);
  if (sharpen > 0.0f) {
    SharpenPremul(&upscaled, static_cast<int>(dstW), static_cast<int>(dstH), sharpen);
  }

  const size_t dstPixels = static_cast<size_t>(dstW) * dstH;
  dst->assign(dstPixels * 4, 0);
  for (size_t i = 0; i < dstPixels; ++i) {
    const double alpha = upscaled[i * 4 + 3];
    const double alphaByte = alpha * 255.0 + 0.5;
    (*dst)[i * 4 + 3] = static_cast<uint8_t>(alphaByte > 255.0 ? 255.0 : alphaByte);
    if (alpha <= 0.0) {
      continue;
    }
    for (int c = 0; c < 3; ++c) {
      double value = upscaled[i * 4 + c] / alpha;
      value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
      (*dst)[i * 4 + c] = static_cast<uint8_t>(value * 255.0 + 0.5);
    }
  }
}

uint32_t HashBgra(const uint8_t* data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

}  // namespace hmrdp
