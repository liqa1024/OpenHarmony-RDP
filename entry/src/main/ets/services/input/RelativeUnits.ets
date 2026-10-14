// Unit conversion for relative pointer motion. The delta is the mapped pointer movement in content
// pixels, sent as host counts: 1:1 keeps the host cursor travel equal to what the local pointer
// covered on the content, so relative mode matches absolute mode's gain — a host display scale other
// than 100% has to be divided out (see doc_agent/session-layer.md §8).
export const RELATIVE_COUNTS_PER_PX: number = 1;

// Fine-tune range of the user-facing relative speed multiplier.
export const MIN_RELATIVE_SPEED: number = 0.25;
export const MAX_RELATIVE_SPEED: number = 2;

// The host reads relative counts as logical pixels while the content is physical pixels, so its
// display scale multiplies the travel. `hostScalePercent` is the scale in effect on the streamed
// display (0/unknown ⇒ 100%, i.e. no correction).
export function relativeCountsGain(hostScalePercent: number, speed: number): number {
  const scale: number = hostScalePercent > 0 ? hostScalePercent : 100;
  return RELATIVE_COUNTS_PER_PX * speed * 100 / scale;
}
