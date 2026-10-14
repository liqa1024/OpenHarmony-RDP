import { PointMapper } from './InputSink';

// Timing/geometry policy shared by the services/input/* controllers.

// Continuous position streams (mouse / pen move, touch move) are rate-limited to this floor.
export const MOUSE_MOVE_MIN_INTERVAL_MS: number = 8;
export const TOUCH_MOVE_INTERVAL_HIGH_RATE_MS: number = 8;
export const TOUCH_MOVE_INTERVAL_DEFAULT_MS: number = 20;
export const TOUCH_MOVE_INTERVAL_FALLBACK_MS: number = 8;

// A TouchType.Cancel does not always mean the finger lifted: a Down within this distance of its
// last position resumes the contact instead of lifting and re-pressing.
export const CANCEL_RESUME_RATIO: number = 0.02;
export const CANCEL_RESUME_MIN_DISTANCE: number = 16;
export const CANCEL_RESUME_MAX_DISTANCE: number = 64;
export const CANCEL_HOLD_MS: number = 400;

export const TOUCHPAD_DOUBLE_TAP_MS: number = 300;
export const TOUCHPAD_PRESS_HOLD_MS: number = 30;
export const TOUCHPAD_MOVE_SLOP: number = 8;

export const MIN_PINCH_DISTANCE_PERCENT: number = 1;
export const MAX_PINCH_DISTANCE_PERCENT: number = 20;
export const MIN_PINCH_HALF: number = 8;
export const MIN_PINCH_FRAC: number = 0.02;
export const NATIVE_PINCH_BASE_PERCENT: number = 20;
// The native pair lies along the pad diagonal (45 deg in the normalized pad square, which
// maps to the physical corner-to-corner direction), maximizing the usable separation length.
export const NATIVE_PINCH_ANGLE_DEG: number = 45;
export const TOUCHPAD_WIDTH_MM: number = 100;
export const TOUCHPAD_HEIGHT_MM: number = 70;
// Host precision-touchpad pan: the pair re-touches at the middle when the declared pad runs out.
export const TOUCHPAD_PAN_FINGER_MM: number = 25;
export const TOUCHPAD_PAN_SAFE_HALF: number = 0.35;
export const TOUCHPAD_PAN_END_GRACE_MS: number = 60;
export const TOUCHPAD_PAN_CENTER: number = 0.5;
export const TOUCHPAD_PAN_ID_A: number = 30;
export const TOUCHPAD_PAN_ID_B: number = 31;
// axisPinch is a distance ratio relative to the gesture start; clamp it so a bad sample cannot
// explode or collapse the separation.
export const MIN_PINCH_RATIO: number = 0.05;
export const MAX_PINCH_RATIO: number = 20;
export const PINCH_TOUCH_ID_A: number = 20;
export const PINCH_TOUCH_ID_B: number = 21;
// FreeRDP coalesces touch frames, so the first MOTION must not reach RDPEI before its DOWN flush
// or it overwrites the unsent DOWN.
export const PINCH_TOUCH_DOWN_FLUSH_MS: number = 30;
export const PINCH_CURSOR_HYSTERESIS: number = 0.02;

export function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

export function isPinchEvent(event: AxisEvent): boolean {
  if ((event.axisPinch ?? 0) > 0) {
    return true;
  }
  try {
    return event.hasAxis(AxisType.PINCH_AXIS);
  } catch (error) {
    return false;
  }
}

export function pinchScaleValue(event: AxisEvent): number {
  const direct: number = event.axisPinch ?? 0;
  if (direct > 0) {
    return direct;
  }
  try {
    return event.getPinchAxisScaleValue();
  } catch (error) {
    return 0;
  }
}

// Shared pointer space: the logical view size and the client-area -> pointer-space mapper.
// Owned by the facade and read by every controller that maps a position.
export class InputGeometry {
  private width: number = 0;
  private height: number = 0;
  private mapper: PointMapper = (x: number, y: number): number[] => [x, y];

  setSpace(width: number, height: number): void {
    this.width = width;
    this.height = height;
  }

  setMapper(mapper: PointMapper): void {
    this.mapper = mapper;
  }

  map(x: number, y: number): number[] {
    return this.mapper(x, y);
  }

  pointerWidth(): number {
    return this.width;
  }

  pointerHeight(): number {
    return this.height;
  }

  minDim(): number {
    return Math.min(this.width, this.height);
  }
}
