// Unit conversion for relative pointer motion. The delta is the mapped pointer movement in content
// pixels, sent as host counts: 1:1 keeps the host cursor travel equal to what the local pointer
// covered on the content, so relative mode matches absolute mode's gain (a host display scale other
// than 100% would have to divide this, see doc_agent/session-layer.md §8).
export const RELATIVE_COUNTS_PER_PX: number = 1;

// Fine-tune range of the user-facing relative speed multiplier.
export const MIN_RELATIVE_SPEED: number = 0.25;
export const MAX_RELATIVE_SPEED: number = 2;
