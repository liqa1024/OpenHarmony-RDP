#ifndef HMRDP_COLOR_H
#define HMRDP_COLOR_H

#include <cstdint>
#include <string>

namespace hmrdp {

// How a picture's samples are to be interpreted, in one value: gamut, transfer function, the YUV
// matrix the samples were encoded with, and the quantisation range. The pipeline carries this single
// type instead of a (colorSpace, colorRange) pair so "is this HDR" is a property of the description
// (IsHdr) rather than a second flag travelling beside it.
//
// The platform already models colour this way: OH_NativeBuffer_ColorSpace is the packed
// (primaries | transfer | matrix | range) descriptor the codec prints on a decoded buffer, so the
// frame's own statement is the richest source the pipeline has (see TryFromNativeBuffer).
struct ColorDescription {
  enum class Primaries : int {
    kBt601SmpteC,  // SMPTE 170M / SMPTE-C
    kBt601Ebu,     // EBU 3213-E
    kBt709,
    kBt2020,
    kP3,  // P3-D65 (DCI-P3 and Display-P3 differ only in white point)
    kAdobeRgb,
    kSrgb,
  };

  enum class Transfer : int {
    kGamma22,  // BT.1886 / gamma ~2.2, the SDR YUV transfer
    kSrgb,
    kLinear,  // scRGB / Windows advanced-colour compositing
    kPq,      // SMPTE ST 2084
    kHlg,     // ARIB STD-B67
    kLog,     // BT.2020 log (not produced by this pipeline)
  };

  enum class Matrix : int {
    kBt601,
    kBt709,
    kBt2020Ncl,
    // Samples are already RGB (a packed layout or a surface): there is no YUV matrix to apply.
    kIdentity,
  };

  enum class Range : int {
    kLimited,
    kFull,
  };

  Primaries primaries = Primaries::kBt709;
  Transfer transfer = Transfer::kGamma22;
  Matrix matrix = Matrix::kBt709;
  Range range = Range::kLimited;
  int bitDepth = 8;

  bool IsRgb() const {
    return matrix == Matrix::kIdentity;
  }
};

// The SDR contract the session falls back to when nothing is negotiated: Rec.709 limited, which is
// what the host produces for an HD stream and what an unstated buffer means.
ColorDescription SdrBt709Limited();

// The 10-bit Rec.2020 PQ stream an HDR session requests and the HDR10 surface carries.
ColorDescription Hdr10Bt2020PqLimited();

// The 10-bit Rec.2020 HLG stream an HLG session requests (same gamut and range as HDR10, ARIB
// STD-B67 transfer instead of PQ).
ColorDescription HlgBt2020Limited();

// Converts an OH_NativeBuffer_ColorSpace into the canonical type. False for a value this pipeline
// does not model (OH_COLORSPACE_NONE and anything unmapped), so the caller can state "the buffer
// declares a space we cannot honour" instead of silently assuming a matrix.
//
// `bitDepth` is not part of the OH value (it comes from the pixel format), so the caller supplies it.
bool TryFromNativeBuffer(int32_t ohColorSpace, int bitDepth, ColorDescription* out);

// The OH_NativeBuffer_ColorSpace the description corresponds to, for the window's colour space and
// the negotiated expectation; OH_COLORSPACE_NONE when the description has no OH equivalent.
int32_t ToNativeBuffer(const ColorDescription& description);

// Whether two descriptions make the same *YUV decision*: the same matrix and the same range. Gamut
// and transfer do not change how samples become RGB, so they are not a mismatch; this is what the
// negotiated expectation is checked against.
bool SameMatrixAndRange(const ColorDescription& a, const ColorDescription& b);

// A stable, log-friendly rendering, e.g. "bt709 limited gamma22 8-bit".
std::string ToString(const ColorDescription& description);

}

#endif
