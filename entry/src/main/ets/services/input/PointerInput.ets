import { TouchpadTapTarget } from '../TouchpadTapFilter';
import { HeldInputs } from './HeldInputs';
import { InputAction, InputButton, InputSink } from './InputSink';
import { InputGeometry } from './InputPolicy';
import { relativeCountsGain } from './RelativeUnits';

// Mouse / pen: buttons and synthetic clicks, plus the absolute move. Implements the
// touchpad tap target so the double-tap filter can synthesize right/middle clicks.
// Continuous motion is handed over as plain verbs; the port's gate owns the stream's window.
export class PointerInput implements TouchpadTapTarget {
  private sink: InputSink;
  private held: HeldInputs;
  private geometry: InputGeometry;
  private lastX: number = 0;
  private lastY: number = 0;
  private havePosition: boolean = false;
  private relativeBaselineTaken: boolean = false;
  private anchorPending: boolean = false;
  private relativeSpeed: number = 1;

  constructor(sink: InputSink, held: HeldInputs, geometry: InputGeometry) {
    this.sink = sink;
    this.held = held;
    this.geometry = geometry;
  }

  // Relative mode drops the absolute mapping: motion becomes the difference of consecutive mapped
  // positions, so the baseline is re-taken when the mode turns on.
  setRelativePointer(enabled: boolean): void {
    if (enabled) {
      this.relativeBaselineTaken = false;
      this.anchorPending = true;
    }
  }

  // The host cursor can be anywhere after a focus switch, so the next pointer event re-takes the
  // baseline and re-states where the local pointer is.
  reanchor(): void {
    this.relativeBaselineTaken = false;
    this.anchorPending = true;
  }

  setRelativeSpeed(speed: number): void {
    this.relativeSpeed = speed;
  }

  lastPointerX(): number {
    return this.lastX;
  }

  lastPointerY(): number {
    return this.lastY;
  }

  hasPosition(): boolean {
    return this.havePosition;
  }

  reportPosition(): boolean {
    if (!this.havePosition) {
      return false;
    }
    this.sink.pointerMove(this.lastX, this.lastY);
    return true;
  }

  rightDown(x: number, y: number): void {
    this.handleButton(InputAction.PRESS, InputButton.RIGHT, x, y);
  }

  rightUp(x: number, y: number): void {
    this.handleButton(InputAction.RELEASE, InputButton.RIGHT, x, y);
  }

  clickRight(x: number, y: number): void {
    this.sendSyntheticClick(InputButton.RIGHT, x, y);
  }

  clickMiddle(x: number, y: number): void {
    this.sendSyntheticClick(InputButton.MIDDLE, x, y);
  }

  handleMouseEvent(event: MouseEvent, point: number[]): void {
    if (event.action === MouseAction.Press) {
      this.handleButton(InputAction.PRESS, PointerInput.buttonFor(event.button), point[0], point[1]);
    } else if (event.action === MouseAction.Release) {
      this.handleButton(InputAction.RELEASE, PointerInput.buttonFor(event.button), point[0], point[1]);
    } else if (event.action === MouseAction.Move) {
      if (this.sink.relativePointerEnabled()) {
        this.handleRelativeMove(point[0], point[1]);
        return;
      }
      this.handleMove(point[0], point[1]);
    }
  }

  private handleButton(action: number, button: number, x: number, y: number): void {
    this.lastX = x;
    this.lastY = y;
    this.havePosition = true;
    this.sink.button(action, button, x, y);
    if (action === InputAction.PRESS) {
      this.held.pressButton(button);
    } else {
      this.held.releaseButton(button);
    }
  }

  private handleMove(x: number, y: number): void {
    this.lastX = x;
    this.lastY = y;
    this.havePosition = true;
    this.sink.pointerMove(x, y);
  }

  // Consecutive positions differenced; the host keeps its own cursor position from there on. The
  // first frame of a (re)taken baseline states the absolute position instead of a delta.
  private handleRelativeMove(x: number, y: number): void {
    const dx: number = x - this.lastX;
    const dy: number = y - this.lastY;
    const baseline: boolean = !this.relativeBaselineTaken;
    this.relativeBaselineTaken = true;
    this.lastX = x;
    this.lastY = y;
    this.havePosition = true;
    if (baseline) {
      if (this.anchorPending) {
        this.anchorPending = false;
        this.sink.pointerMove(x, y);
      }
      return;
    }
    if (dx === 0 && dy === 0) {
      return;
    }
    const gain: number = relativeCountsGain(this.sink.hostDisplayScalePercent(), this.relativeSpeed);
    this.sink.pointerMoveRelative(dx * gain, dy * gain);
  }

  /** One whole click as press + release, so the remote never keeps a button down. */
  private sendSyntheticClick(button: number, x: number, y: number): void {
    this.handleButton(InputAction.PRESS, button, x, y);
    this.handleButton(InputAction.RELEASE, button, x, y);
  }

  private static buttonFor(button: MouseButton): number {
    if (button === MouseButton.Left) {
      return InputButton.LEFT;
    }
    if (button === MouseButton.Middle) {
      return InputButton.MIDDLE;
    }
    if (button === MouseButton.Right) {
      return InputButton.RIGHT;
    }
    return 0;
  }
}
