/*
 * HmRdp - touchpad displacement -> high-resolution RDP wheel mapping.
 *
 * ArkUI reports a two-finger touchpad swipe as raw finger displacement in vp
 * (sourceTool == TOUCHPAD); unlike a Windows touchpad driver it does not already
 * convert that into wheel detents. RDP carries a wheel rotation as a 9-bit
 * two's-complement value in the pointer flags, where 120 equals one detent, and
 * a single RDP input event can only carry up to 255. FreeRDP's SDL client scales
 * high-resolution wheel input by 120 and splits larger amounts into <= 0xFF
 * chunks; this mapper does the same, so the remote receives the same
 * high-resolution wheel stream a native FreeRDP client would send.
 *
 * The mapping is a continuous rotation accumulator:
 *
 *   rotation += displacementVp * (WHEEL_DETENT / VP_PER_DETENT) * speed
 *
 * VP_PER_DETENT is the finger travel equivalent to one detent at 100% speed, so
 * the speed setting scales how much rotation a swipe produces without touching
 * the emission granularity. Fractional rotation is carried across events (no
 * rounding loss) and only integer chunks leave the mapper.
 */

// Microsoft/FreeRDP wheel rotation unit: 120 == one wheel detent.
export const WHEEL_DETENT: number = 120;
// Largest rotation a single RDP pointer event can carry (9-bit wheel field).
const WHEEL_MAX_CHUNK: number = 0xFF;
// Finger travel (vp) that maps to one full detent at 100% speed. Matches the
// feel of the previous fixed step while leaving room to tune either way.
export const VP_PER_DETENT: number = 16;

export const MIN_SCROLL_SPEED_PERCENT: number = 25;
export const MAX_SCROLL_SPEED_PERCENT: number = 400;
export const DEFAULT_SCROLL_SPEED_PERCENT: number = 100;

/** One RDP wheel event produced by the mapper. */
export class WheelChunk {
  horizontal: boolean = false;
  negative: boolean = false;
  rotation: number = 0;
}

/**
 * Stateful mapper: feed it the per-event axis displacement and collect the
 * wheel chunks to send. Horizontal and vertical axes accumulate independently.
 */
export class TouchpadWheelMapper {
  private speedScale: number = 1;
  private accumX: number = 0;
  private accumY: number = 0;

  setSpeedPercent(percent: number): void {
    const clamped: number =
      Math.max(MIN_SCROLL_SPEED_PERCENT, Math.min(MAX_SCROLL_SPEED_PERCENT, percent));
    this.speedScale = clamped / 100;
  }

  /** Drops any pending sub-chunk remainder (call when a gesture starts). */
  reset(): void {
    this.accumX = 0;
    this.accumY = 0;
  }

  /**
   * Converts one touchpad axis update into wheel chunks appended to `out`.
   * `vertical` / `horizontal` are ArkUI per-event displacements in vp.
   */
  map(vertical: number, horizontal: number, out: WheelChunk[]): void {
    const rotationPerVp: number = (WHEEL_DETENT / VP_PER_DETENT) * this.speedScale;
    // Natural scrolling: the remote content follows the fingers. Positive
    // axisVertical is up and positive axisHorizontal is left, which map to a
    // negative vertical wheel and a positive horizontal wheel respectively,
    // matching the RDP wheel sign convention.
    this.accumY += (-vertical) * rotationPerVp;
    this.accumX += horizontal * rotationPerVp;
    this.accumY = TouchpadWheelMapper.drain(false, this.accumY, out);
    this.accumX = TouchpadWheelMapper.drain(true, this.accumX, out);
  }

  /** Emits integer chunks of at most WHEEL_MAX_CHUNK, returns the remainder. */
  private static drain(horizontal: boolean, accum: number, out: WheelChunk[]): number {
    let remaining: number = accum;
    while (Math.abs(remaining) >= 1) {
      const magnitude: number = Math.min(Math.floor(Math.abs(remaining)), WHEEL_MAX_CHUNK);
      if (magnitude < 1) {
        break;
      }
      const chunk: WheelChunk = new WheelChunk();
      chunk.horizontal = horizontal;
      chunk.negative = remaining < 0;
      chunk.rotation = magnitude;
      out.push(chunk);
      remaining -= remaining < 0 ? -magnitude : magnitude;
    }
    return remaining;
  }
}
