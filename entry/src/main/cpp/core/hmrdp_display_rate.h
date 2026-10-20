#ifndef HMRDP_DISPLAY_RATE_H
#define HMRDP_DISPLAY_RATE_H

#include <cstdint>

namespace hmrdp {

// The refresh rate of the display the session window is on, pushed by the UI layer: only that layer
// knows which display a window sits on (`window` properties), and it can differ from the default
// display on a multi-monitor 2in1. Non-zero wins over the automatic reading; 0 hands the reading back
// to the default display (see DisplayRefreshRateHz).
void SetDisplayRefreshRateHz(int hz);

// The default display's current refresh rate in Hz, 0 while the platform states none. Read on first
// use and refreshed whenever the display changes: the system moves the panel between gears while a
// session runs, so a value read once at start is not what the display is actually doing.
int DisplayRefreshRateHz();

// True when the display's current gear is known and lower than `frameRate`: a frame given up now was
// given up because the screen cannot hold the stream's rate — not because the decoder, the GPU or the
// network is behind. `frameRate` 0 (unknown) is never attributed to the display.
bool DisplayBelowFrameRate(int frameRate);

// How far ahead of the cursor a caller may claim a slot, as a fraction of the refresh period it
// divides. The desktop path claims half a period: a frame earlier than that joins the carried region
// rather than going out on its own, because the desktop is a state and one present per period is all
// the display can show. The video path claims a whole period: a frame that early is *held* for its
// slot instead of being dropped, since the slot it would take is the one the display was going to
// refresh at anyway.
enum class SlotTolerance : int64_t {
  kHalfPeriod = 2,
  kFullPeriod = 1,
};

// Present slot cursor for the display's current gear: the display holds the frame it was given for a
// refresh period, so only one frame per period can be shown. The cursor answers *when* a frame may
// go out — a slot instant, not a yes/no — and leaves the caller to decide what a refused frame means
// (the video path holds it, the desktop path carries it).
class SlotSchedule {
 public:
  // Claims the next slot for a frame arriving at `nowUs`: true when that slot opens no later than
  // `period / tolerance`, with its instant written to `slotUs` (null when the caller only wants the
  // decision). False means the frame is too early for any slot inside that window, and the cursor
  // does not move. A display whose gear is unknown never refuses and never advances.
  // The grid is a free-running progression: the cursor advances by whole periods and is *not* rebased
  // on `nowUs`, so a late wake-up cannot make it tick slower than the display.
  bool Claim(int64_t nowUs, SlotTolerance tolerance, int64_t* slotUs = nullptr);
  // Microseconds until Claim() would return true for the same tolerance, 0 when it already would.
  int64_t WaitUs(int64_t nowUs, SlotTolerance tolerance) const;
  // The instant a frame going out right now would be shown: the cursor while it is still in the
  // future, `nowUs` otherwise. A query — the cursor only moves on Claim(), so a caller can hold a
  // frame until its slot and still claim that same slot when it lets it go.
  int64_t NextSlotUs(int64_t nowUs) const;

 private:
  int64_t nextUs_ = 0;
};

}

#endif
