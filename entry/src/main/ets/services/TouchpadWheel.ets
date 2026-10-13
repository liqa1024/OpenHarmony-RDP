export const WHEEL_DETENT: number = 120;
// Largest rotation a single RDP pointer event can carry (9-bit wheel field).
const WHEEL_MAX_CHUNK: number = 0xFF;
const SCROLL_LINES_PER_DETENT: number = 3;
const LINE_PX: number = 16;
const DETENT_CALIBRATION: number = 4;
export const VP_PER_DETENT: number = SCROLL_LINES_PER_DETENT * LINE_PX * DETENT_CALIBRATION;
export const WHEEL_VP_PER_DEGREE: number = 21 / 15;
// Touchpad fling after the fingers lift, mirroring ArkUI Scrollable (API>=13 defaults):
// distance = velocity * NEW_VELOCITY_SCALE / (friction * |FRICTION_SCALE|), decaying as a
// critically damped spring (response = 2*PI / (friction * |FRICTION_SCALE|)).
export const FLING_VELOCITY_SCALE: number = 1.5;
export const FLING_FRICTION: number = 0.75;
export const FLING_FRICTION_SCALE: number = 4.2;
export const FLING_MIN_VELOCITY: number = 240;

export const MIN_SPEED_SCALE: number = 0.2;
export const MAX_SPEED_SCALE: number = 4;
export const DEFAULT_SPEED_SCALE: number = 1;
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
