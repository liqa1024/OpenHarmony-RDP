export const TOOLBAR_ROW_HEIGHT: number = 48;
export const TOOLBAR_TELEMETRY_HEIGHT: number = 20;
export const TOOLBAR_RADIUS: number = 14;
export const TOOLBAR_ZONE_NONE: number = 0;
export const TOOLBAR_ZONE_TRIGGER: number = 1;
export const TOOLBAR_ZONE_BAR: number = 2;
// Toolbar width: the pill hugs its content (never below the configured floor)
// and snaps into the full-bleed bar form once it comes this close to the full
// width; the looser threshold on the way back keeps a content change at the
// boundary from flipping the form back and forth.
export const TOOLBAR_SNAP_RATIO: number = 0.95;
export const TOOLBAR_UNSNAP_RATIO: number = 0.88;

export const REVEAL_HINT_WIDTH: number = 96;
export const REVEAL_HINT_HEIGHT: number = 3;
export const REVEAL_HINT_FADE_MS: number = 140;
export const TOUCH_BAR_HEIGHT: number = 4;
