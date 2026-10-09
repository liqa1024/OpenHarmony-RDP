// Microsoft/FreeRDP wheel rotation unit: 120 == one wheel detent.
export const WHEEL_DETENT: number = 120;
// Largest rotation a single RDP pointer event can carry (9-bit wheel field).
const WHEEL_MAX_CHUNK: number = 0xFF;
// Finger travel (vp) that maps to one full detent at 1.0x speed.
export const VP_PER_DETENT: number = 16;

export const MIN_SPEED_SCALE: number = 0.2;
export const MAX_SPEED_SCALE: number = 4;
export const DEFAULT_SPEED_SCALE: number = 1;
// Pinch detents per unit change of the two-finger distance ratio at 1.0x speed.
export const DEFAULT_PINCH_NOTCHES_PER_UNIT: number = 20;

export class WheelChunk {
  horizontal: boolean = false;
  negative: boolean = false;
  rotation: number = 0;
}

export class TouchpadWheelMapper {
  private speedScale: number = 1;
  private accumX: number = 0;
  private accumY: number = 0;

  setSpeed(scale: number): void {
    this.speedScale = Math.max(MIN_SPEED_SCALE, Math.min(MAX_SPEED_SCALE, scale));
  }

  reset(): void {
    this.accumX = 0;
    this.accumY = 0;
  }

  map(vertical: number, horizontal: number, out: WheelChunk[]): void {
    const rotationPerVp: number = (WHEEL_DETENT / VP_PER_DETENT) * this.speedScale;
    // Natural scrolling, RDP wheel signs: axisVertical+ (up) maps to a negative
    // vertical wheel, axisHorizontal+ (left) to a positive horizontal wheel.
    this.accumY += (-vertical) * rotationPerVp;
    this.accumX += horizontal * rotationPerVp;
    this.accumY = TouchpadWheelMapper.drain(false, this.accumY, out);
    this.accumX = TouchpadWheelMapper.drain(true, this.accumX, out);
  }

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
