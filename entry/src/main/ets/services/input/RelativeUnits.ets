// Unit conversion for relative pointer motion. The delta is the mapped pointer movement in content
// pixels, sent as host counts: 1:1 keeps the host cursor travel equal to what the local pointer
// covered on the content, so relative mode matches absolute mode's gain.
export const RELATIVE_COUNTS_PER_PX: number = 1;
