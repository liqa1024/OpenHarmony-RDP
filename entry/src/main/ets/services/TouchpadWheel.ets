export const WHEEL_DETENT: number = 120;
// Largest rotation a single RDP pointer event can carry (9-bit wheel field).
const WHEEL_MAX_CHUNK: number = 0xFF;
// Remote content px per wheel notch, the host's nominal: SPI_GETWHEELSCROLLLINES = 3 lines x ~16px.
export const REMOTE_PX_PER_DETENT: number = 48;
// End-to-end scroll calibration, applied on top of the nominal anchor to match the remote's actual
// scroll (empirical; see doc_agent/session-layer.md §8). Its own constant, so the nominal one keeps
// its literal meaning.
const SCROLL_CALIBRATION: number = 4;
// Wheel axis is in degrees: 15°/notch -> LINE_HEIGHT_DESKTOP/MOUSE_WHEEL_DEGREES vp (ArkUI's own
// degree -> vp step, which the mapper's vp口径 then shares unchanged).
export const WHEEL_VP_PER_DEGREE: number = 21 / 15;

// Two-finger travel the host's precision touchpad needs per remote notch. The mm -> scroll gain is
// Windows-side (PT curve plus the host's scroll/scale settings), so this is measured end to end
// (~4.8 mm/notch) rather than derived; see doc_agent/session-layer.md §8.
const TOUCHPAD_PAN_MM_PER_DETENT: number = 4.8;
export const TOUCHPAD_PAN_MM_PER_AXIS_PX: number =
  TOUCHPAD_PAN_MM_PER_DETENT / (REMOTE_PX_PER_DETENT * SCROLL_CALIBRATION);

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
  private pxPerVp: number = 1;
  private accumX: number = 0;
  private accumY: number = 0;

  setSpeed(scale: number): void {
    this.speedScale = Math.max(MIN_SPEED_SCALE, Math.min(MAX_SPEED_SCALE, scale));
  }

  setPxPerVp(pxPerVp: number): void {
    this.pxPerVp = pxPerVp > 0 ? pxPerVp : 1;
  }

  reset(): void {
    this.accumX = 0;
    this.accumY = 0;
  }

  // vertical / horizontal are content displacement in vp.
  map(vertical: number, horizontal: number, out: WheelChunk[]): void {
    const rotationPerVp: number =
      (WHEEL_DETENT / (REMOTE_PX_PER_DETENT * SCROLL_CALIBRATION)) * this.pxPerVp * this.speedScale;
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
