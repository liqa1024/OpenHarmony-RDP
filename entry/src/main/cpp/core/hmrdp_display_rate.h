#ifndef HMRDP_DISPLAY_RATE_H
#define HMRDP_DISPLAY_RATE_H

#include <cstdint>

namespace hmrdp {

// The default display's current refresh rate in Hz, 0 while the platform states none. Read on first
// use and refreshed whenever the display changes: the system moves the panel between gears while a
// session runs, so a value read once at start is not what the display is actually doing.
int DisplayRefreshRateHz();

// Present slot cursor for the display's current gear: the display holds the frame it was given for a
// refresh period, so only one frame per period can be shown. Callers keep their own cursor and
// decide what a missed slot means — the video path drops the frame, the desktop path carries it.
class PresentSlots {
 public:
  // True when the display can take a frame now; advances the cursor when it returns true.
  bool Take();
  // Microseconds until Take() would return true, 0 when it already would.
  int64_t WaitUs() const;

 private:
  int64_t nextUs_ = 0;
};

}

#endif
