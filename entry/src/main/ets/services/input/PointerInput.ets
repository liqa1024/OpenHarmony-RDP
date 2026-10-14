import { TouchpadTapTarget } from '../TouchpadTapFilter';
import { HeldInputs } from './HeldInputs';
import { InputAction, InputButton, InputSink } from './InputSink';
import { InputGeometry } from './InputPolicy';
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

// Mouse / pen: buttons and synthetic clicks, plus the coalesced absolute move. Implements the
// touchpad tap target so the double-tap filter can synthesize right/middle clicks.
export class PointerInput implements TouchpadTapTarget {
  private sink: InputSink;
  private held: HeldInputs;
  private geometry: InputGeometry;
  private lastX: number = 0;
  private lastY: number = 0;
  private move: RateCoalescer<MovePoint>;

  constructor(sink: InputSink, held: HeldInputs, geometry: InputGeometry, moveIntervalMs: number) {
    this.sink = sink;
    this.held = held;
    this.geometry = geometry;
    this.move = new RateCoalescer<MovePoint>(moveIntervalMs,
      (_id: number, point: MovePoint): void => {
        this.sink.pointerMove(point.x, point.y);
      });
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
