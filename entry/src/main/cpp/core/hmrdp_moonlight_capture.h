#ifndef HMRDP_MOONLIGHT_CAPTURE_H
#define HMRDP_MOONLIGHT_CAPTURE_H

#include <string>

namespace hmrdp {

// Dev-only raw video capture (the Moonlight equivalent of the RDP GFX capture). The library, via
// the patched LiSetVideoCapture, hands each decode unit here right before submitDecodeUnit, and this
// writes `<dir>/hmrdp_ml.bin`: the complete, decodable Annex-B access unit the host encoded, plus
// the per-frame facts the depacketizer carried. Idempotent: re-applying the same value does not
// truncate a running capture.
void MoonlightCaptureConfigure(bool enabled, const std::string& dir);

// Closes the file; the library hook stays installed but inert.
void MoonlightCaptureShutdown();

// Flushes buffered records (session stop, app teardown).
void MoonlightCaptureFlush();

}

#endif
