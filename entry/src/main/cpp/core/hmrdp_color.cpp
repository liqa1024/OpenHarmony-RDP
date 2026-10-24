#include "hmrdp_color.h"

#include <multimedia/player_framework/native_avcodec_base.h>
#include <native_buffer/native_buffer.h>

namespace hmrdp {
namespace {

using Primaries = ColorDescription::Primaries;
using Transfer = ColorDescription::Transfer;
using Matrix = ColorDescription::Matrix;
using Range = ColorDescription::Range;

// One row per modelled OH colour space. The platform value is a packed bitfield
// (primaries | transfer << 8 | matrix << 16 | range << 21), so the reverse direction is a search of
// this same table; several OH names are aliases of an earlier row (OH_COLORSPACE_DISPLAY_SRGB ==
// SRGB_FULL, OH_COLORSPACE_LINEAR_BT709 == LINEAR_SRGB, the DISPLAY_P3_* == P3_*_FULL pairs) and are
// deliberately not repeated.
//
// The OH matrix field is honoured for the YUV entries, which is what the convert pass needs. The RGB
// entries carry a "standard" matrix in OH (sRGB is filed under BT.601) that no conversion applies,
// so they are modelled as kIdentity: a buffer that says sRGB is then honestly a different YUV
// decision from one negotiated as Rec.709, rather than silently agreeing.
struct Entry {
  int32_t oh;
  Primaries primaries;
  Transfer transfer;
  Matrix matrix;
  Range range;
};

constexpr Entry kEntries[] = {
    { OH_COLORSPACE_BT601_EBU_FULL, Primaries::kBt601Ebu, Transfer::kGamma22, Matrix::kBt601, Range::kFull },
    { OH_COLORSPACE_BT601_EBU_LIMIT, Primaries::kBt601Ebu, Transfer::kGamma22, Matrix::kBt601, Range::kLimited },
    { OH_COLORSPACE_BT601_SMPTE_C_FULL, Primaries::kBt601SmpteC, Transfer::kGamma22, Matrix::kBt601, Range::kFull },
    { OH_COLORSPACE_BT601_SMPTE_C_LIMIT, Primaries::kBt601SmpteC, Transfer::kGamma22, Matrix::kBt601, Range::kLimited },
    { OH_COLORSPACE_BT709_FULL, Primaries::kBt709, Transfer::kGamma22, Matrix::kBt709, Range::kFull },
    { OH_COLORSPACE_BT709_LIMIT, Primaries::kBt709, Transfer::kGamma22, Matrix::kBt709, Range::kLimited },
    { OH_COLORSPACE_BT2020_HLG_FULL, Primaries::kBt2020, Transfer::kHlg, Matrix::kBt2020Ncl, Range::kFull },
    { OH_COLORSPACE_BT2020_HLG_LIMIT, Primaries::kBt2020, Transfer::kHlg, Matrix::kBt2020Ncl, Range::kLimited },
    { OH_COLORSPACE_BT2020_PQ_FULL, Primaries::kBt2020, Transfer::kPq, Matrix::kBt2020Ncl, Range::kFull },
    { OH_COLORSPACE_BT2020_PQ_LIMIT, Primaries::kBt2020, Transfer::kPq, Matrix::kBt2020Ncl, Range::kLimited },
    { OH_COLORSPACE_BT2020_LOG_FULL, Primaries::kBt2020, Transfer::kLog, Matrix::kBt2020Ncl, Range::kFull },
    { OH_COLORSPACE_BT2020_LOG_LIMIT, Primaries::kBt2020, Transfer::kLog, Matrix::kBt2020Ncl, Range::kLimited },
    { OH_COLORSPACE_DISPLAY_BT2020_SRGB, Primaries::kBt2020, Transfer::kSrgb, Matrix::kBt2020Ncl, Range::kFull },
    { OH_COLORSPACE_SRGB_FULL, Primaries::kSrgb, Transfer::kSrgb, Matrix::kIdentity, Range::kFull },
    { OH_COLORSPACE_SRGB_LIMIT, Primaries::kSrgb, Transfer::kSrgb, Matrix::kIdentity, Range::kLimited },
    { OH_COLORSPACE_P3_FULL, Primaries::kP3, Transfer::kSrgb, Matrix::kIdentity, Range::kFull },
    { OH_COLORSPACE_P3_LIMIT, Primaries::kP3, Transfer::kSrgb, Matrix::kIdentity, Range::kLimited },
    { OH_COLORSPACE_P3_HLG_FULL, Primaries::kP3, Transfer::kHlg, Matrix::kIdentity, Range::kFull },
    { OH_COLORSPACE_P3_HLG_LIMIT, Primaries::kP3, Transfer::kHlg, Matrix::kIdentity, Range::kLimited },
    { OH_COLORSPACE_P3_PQ_FULL, Primaries::kP3, Transfer::kPq, Matrix::kIdentity, Range::kFull },
    { OH_COLORSPACE_P3_PQ_LIMIT, Primaries::kP3, Transfer::kPq, Matrix::kIdentity, Range::kLimited },
    { OH_COLORSPACE_ADOBERGB_FULL, Primaries::kAdobeRgb, Transfer::kGamma22, Matrix::kIdentity, Range::kFull },
    { OH_COLORSPACE_ADOBERGB_LIMIT, Primaries::kAdobeRgb, Transfer::kGamma22, Matrix::kIdentity, Range::kLimited },
    // The linear descriptors leave the matrix and range bits unset; they describe compositing input,
    // never a codec's decoded buffer, so the range is taken as full and nothing depends on the choice.
    { OH_COLORSPACE_LINEAR_SRGB, Primaries::kSrgb, Transfer::kLinear, Matrix::kIdentity, Range::kFull },
    { OH_COLORSPACE_LINEAR_P3, Primaries::kP3, Transfer::kLinear, Matrix::kIdentity, Range::kFull },
    { OH_COLORSPACE_LINEAR_BT2020, Primaries::kBt2020, Transfer::kLinear, Matrix::kIdentity, Range::kFull },
};

const char* Name(Primaries primaries) {
  switch (primaries) {
    case Primaries::kBt601SmpteC: return "bt601-smpte-c";
    case Primaries::kBt601Ebu: return "bt601-ebu";
    case Primaries::kBt709: return "bt709";
    case Primaries::kBt2020: return "bt2020";
    case Primaries::kP3: return "p3";
    case Primaries::kAdobeRgb: return "adobe-rgb";
    case Primaries::kSrgb: return "srgb";
  }
  return "?";
}

const char* Name(Transfer transfer) {
  switch (transfer) {
    case Transfer::kGamma22: return "gamma22";
    case Transfer::kSrgb: return "srgb";
    case Transfer::kLinear: return "linear";
    case Transfer::kPq: return "pq";
    case Transfer::kHlg: return "hlg";
    case Transfer::kLog: return "log";
  }
  return "?";
}

const char* Name(Matrix matrix) {
  switch (matrix) {
    case Matrix::kBt601: return "bt601";
    case Matrix::kBt709: return "bt709";
    case Matrix::kBt2020Ncl: return "bt2020-ncl";
    case Matrix::kIdentity: return "rgb";
  }
  return "?";
}

const char* Name(Range range) {
  switch (range) {
    case Range::kLimited: return "limited";
    case Range::kFull: return "full";
  }
  return "?";
}

}  // namespace

ColorDescription SdrBt709Limited() {
  return ColorDescription{ Primaries::kBt709, Transfer::kGamma22, Matrix::kBt709, Range::kLimited, 8 };
}

ColorDescription Hdr10Bt2020PqLimited() {
  return ColorDescription{ Primaries::kBt2020, Transfer::kPq, Matrix::kBt2020Ncl, Range::kLimited, 10 };
}

ColorDescription HlgBt2020Limited() {
  return ColorDescription{ Primaries::kBt2020, Transfer::kHlg, Matrix::kBt2020Ncl, Range::kLimited, 10 };
}

bool TryFromNativeBuffer(int32_t ohColorSpace, int bitDepth, ColorDescription* out) {
  if (out == nullptr || ohColorSpace == OH_COLORSPACE_NONE) {
    return false;
  }
  for (const Entry& entry : kEntries) {
    if (entry.oh == ohColorSpace) {
      out->primaries = entry.primaries;
      out->transfer = entry.transfer;
      out->matrix = entry.matrix;
      out->range = entry.range;
      out->bitDepth = bitDepth;
      return true;
    }
  }
  return false;
}

int32_t ToNativeBuffer(const ColorDescription& description) {
  for (const Entry& entry : kEntries) {
    if (entry.primaries == description.primaries && entry.transfer == description.transfer &&
        entry.matrix == description.matrix && entry.range == description.range) {
      return entry.oh;
    }
  }
  return OH_COLORSPACE_NONE;
}

bool SameMatrixAndRange(const ColorDescription& a, const ColorDescription& b) {
  return a.matrix == b.matrix && a.range == b.range;
}

std::string ToString(const ColorDescription& description) {
  return std::string(Name(description.primaries)) + " " + Name(description.range) + " " +
         Name(description.transfer) + " " + Name(description.matrix) + " " +
         std::to_string(description.bitDepth) + "-bit";
}

}  // namespace hmrdp
