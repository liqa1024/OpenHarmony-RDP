import { TouchpadTapTarget } from '../TouchpadTapFilter';
import { HeldInputs } from './HeldInputs';
import { InputAction, InputButton, InputSink } from './InputSink';
import { InputGeometry } from './InputPolicy';
import { ImmediateMoveDelivery, MoveDelivery, MoveFlush } from './MoveDelivery';
import { RateCoalescer } from './RateCoalescer';

class MovePoint {
  x: number = 0;
  y: number = 0;

  static of(x: number, y: number): MovePoint {
    const point: MovePoint = new MovePoint();
    point.x = x;
    point.y = y;
    return point;
  }
}

// Mouse / pen: buttons and synthetic clicks, plus the absolute move. Implements the
// touchpad tap target so the double-tap filter can synthesize right/middle clicks.
export class PointerInput implements TouchpadTapTarget {
  private sink: InputSink;
  private held: HeldInputs;
  private geometry: InputGeometry;
  private lastX: number = 0;
  private lastY: number = 0;
  private havePosition: boolean = false;
  private move: MoveDelivery<MovePoint>;

  constructor(sink: InputSink, held: HeldInputs, geometry: InputGeometry, moveIntervalMs: number) {
    this.sink = sink;
    this.held = held;
    this.geometry = geometry;
    const flush: MoveFlush<MovePoint> =
      (_id: number, point: MovePoint): void => {
        this.sink.pointerMove(point.x, point.y);
      };
    // The sink owns the policy (see InputSink.rateLimitMoveStreams).
    this.move = sink.rateLimitMoveStreams()
      ? new RateCoalescer<MovePoint>(moveIntervalMs, flush)
      : new ImmediateMoveDelivery<MovePoint>(flush);
  }

  setMoveInterval(ms: number): void {
    this.move.setInterval(ms);
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
    this.move.cancel();
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
        // The host owns the cursor position, so the local mapping must not be applied.
        this.sink.pointerMoveRelative(event.rawDeltaX ?? 0, event.rawDeltaY ?? 0);
        return;
      }
      this.handleMove(point[0], point[1]);
    }
  }

  cancelMove(): void {
    this.move.cancel();
  }

  // Flush the pending move so a discrete pointer event (button, wheel) lands on current coordinates.
  flushMove(): void {
    this.move.flush(0);
  }

  private handleButton(action: number, button: number, x: number, y: number): void {
    this.lastX = x;
    this.lastY = y;
    this.havePosition = true;
    this.flushMove();
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
    this.move.post(0, MovePoint.of(x, y));
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
