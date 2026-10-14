import { InputSink, TouchPhase } from './InputSink';
import { CANCEL_HOLD_MS, CANCEL_RESUME_MAX_DISTANCE, CANCEL_RESUME_MIN_DISTANCE, CANCEL_RESUME_RATIO,
  InputGeometry } from './InputPolicy';
import { ImmediateMoveDelivery, MoveDelivery, MoveFlush } from './MoveDelivery';
import { RateCoalescer } from './RateCoalescer';

class TouchSample {
  x: number = 0;
  y: number = 0;
  pressure: number = 0;

  static of(x: number, y: number, pressure: number): TouchSample {
    const sample: TouchSample = new TouchSample();
    sample.x = x;
    sample.y = y;
    sample.pressure = pressure;
    return sample;
  }
}

// Direct-touch state machine: finger id +1 away from the free 0 slot, cancel-is-not-lift resume,
// and a coalesced per-finger move so both protocols bound the contact message rate.
export class TouchInput {
  private sink: InputSink;
  private geometry: InputGeometry;
  private touchEnabled: boolean = false;
  private points: Map<number, number[]> = new Map<number, number[]>();
  private contactIds: Map<number, number> = new Map<number, number>();
  private pendingCancelIds: number[] = [];
  private cancelTimers: Map<number, number> = new Map<number, number>();
  private move: MoveDelivery<TouchSample>;
  private directContactListener: () => void = (): void => {};

  constructor(sink: InputSink, geometry: InputGeometry, moveIntervalMs: number) {
    this.sink = sink;
    this.geometry = geometry;
    const flush: MoveFlush<TouchSample> =
      (id: number, sample: TouchSample): void => {
        this.sink.touch(TouchPhase.MOVE, id, sample.x, sample.y, sample.pressure);
      };
    this.move = sink.rateLimitMoveStreams()
      ? new RateCoalescer<TouchSample>(moveIntervalMs, flush)
      : new ImmediateMoveDelivery<TouchSample>(flush);
  }

  setMoveInterval(ms: number): void {
    this.move.setInterval(ms);
  }

  setTouchEnabled(enabled: boolean): void {
    this.touchEnabled = enabled;
  }

  isEnabled(): boolean {
    return this.touchEnabled;
  }

  // Fired on every direct touch Down, even while touch is disabled, so the scroll engine can
  // yield the gesture that follows to the touch path. A caller that withholds a contact fires it
  // itself (noteDirectContact) at the real Down, before deciding to replay or drop.
  setDirectContactListener(listener: () => void): void {
    this.directContactListener = listener;
  }

  handleTouch(event: TouchEvent): void {
    if (event.source !== SourceType.TouchScreen) {
      return;
    }
    const changed: TouchObject[] = event.changedTouches;
    for (let i = 0; i < changed.length; i++) {
      if (changed[i].type === TouchType.Down) {
        this.directContactListener();
        break;
      }
    }
    if (!this.touchEnabled) {
      return;
    }
    for (let i = 0; i < changed.length; i++) {
      const touch: TouchObject = changed[i];
      this.dispatch(touch.type, touch.id, touch.x, touch.y, touch.pressure);
    }
  }

  // A caller that withholds a contact until it knows whether the remote should see it replays what
  // it held through here: one plain sample at a time, since a TouchEvent does not outlive its callback.
  handleTouchPoint(phase: TouchType, id: number, x: number, y: number,
    pressure: number | undefined): void {
    if (!this.touchEnabled) {
      return;
    }
    this.dispatch(phase, id, x, y, pressure);
  }

  // The engine yields the gesture that follows a direct contact, whatever the touch path then does
  // with it — including dropping it.
  noteDirectContact(): void {
    this.directContactListener();
  }

  private dispatch(phase: TouchType, id: number, x: number, y: number,
    pressure: number | undefined): void {
    const point: number[] = this.geometry.map(x, y);
    const normalized: number = TouchInput.normalizePressure(pressure);
    if (phase === TouchType.Down) {
      this.onDown(id, point, normalized);
    } else if (phase === TouchType.Move) {
      this.onMove(id, point, normalized);
    } else if (phase === TouchType.Up) {
      this.onUp(id, point, normalized);
    } else if (phase === TouchType.Cancel) {
      this.onCancel(id, point);
    }
  }

  releaseAll(): void {
    this.move.cancel();
    this.finalizeAllCancels();
    // Lift every tracked contact individually; a protocol-specific "all gone" primitive
    // would leave RDP with stuck fingers.
    this.contactIds.forEach((pointerId: number, id: number): void => {
      const point: number[] | undefined = this.points.get(id);
      if (point !== undefined) {
        this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], 0);
      }
    });
    this.points.clear();
    this.contactIds.clear();
  }

  private onDown(id: number, point: number[], pressure: number): void {
    if (this.contactIds.has(id)) {
      return;
    }
    const resumed: number = this.findResumableCancel(point);
    if (resumed >= 0) {
      const known: number | undefined = this.contactIds.get(resumed);
      const resumedId: number = known !== undefined ? known : resumed + 1;
      this.clearCancelTimer(resumed);
      this.removePendingCancel(resumed);
      this.points.delete(resumed);
      this.contactIds.delete(resumed);
      this.points.set(id, point);
      this.contactIds.set(id, resumedId);
      this.move.flush(resumedId);
      this.sink.touch(TouchPhase.MOVE, resumedId, point[0], point[1], pressure);
      return;
    }
    this.finalizeAllCancels();
    const pointerId: number = id + 1;
    this.points.set(id, point);
    this.contactIds.set(id, pointerId);
    this.sink.touch(TouchPhase.DOWN, pointerId, point[0], point[1], pressure);
  }

  private onMove(id: number, point: number[], pressure: number): void {
    const pointerId: number | undefined = this.contactIds.get(id);
    if (pointerId === undefined) {
      return;
    }
    if (this.isPendingCancel(id)) {
      this.clearCancelTimer(id);
      this.removePendingCancel(id);
    }
    const last: number[] | undefined = this.points.get(id);
    if (last !== undefined && last[0] === point[0] && last[1] === point[1]) {
      return;
    }
    this.points.set(id, point);
    this.move.post(pointerId, TouchSample.of(point[0], point[1], pressure));
  }

  private onUp(id: number, point: number[], pressure: number): void {
    const pointerId: number | undefined = this.contactIds.get(id);
    if (pointerId === undefined) {
      return;
    }
    this.clearCancelTimer(id);
    this.removePendingCancel(id);
    this.points.delete(id);
    this.contactIds.delete(id);
    this.move.flush(pointerId);
    this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], pressure);
  }

  private onCancel(id: number, point: number[]): void {
    if (!this.contactIds.has(id) || this.isPendingCancel(id)) {
      return;
    }
    this.points.set(id, point);
    this.pendingCancelIds.push(id);
    const timer: number = setTimeout(() => {
      this.cancelTimers.delete(id);
      this.finalizeCancel(id);
    }, CANCEL_HOLD_MS);
    this.cancelTimers.set(id, timer);
  }

  private findResumableCancel(point: number[]): number {
    const minDim: number = this.geometry.minDim();
    const limit: number = Math.max(CANCEL_RESUME_MIN_DISTANCE,
      Math.min(CANCEL_RESUME_MAX_DISTANCE, Math.round(minDim * CANCEL_RESUME_RATIO)));
    const limitSq: number = limit * limit;
    let best: number = -1;
    let bestDist: number = limitSq;
    for (let i = 0; i < this.pendingCancelIds.length; i++) {
      const id: number = this.pendingCancelIds[i];
      const last: number[] | undefined = this.points.get(id);
      if (last === undefined) {
        continue;
      }
      const dx: number = last[0] - point[0];
      const dy: number = last[1] - point[1];
      const dist: number = dx * dx + dy * dy;
      if (dist <= bestDist) {
        bestDist = dist;
        best = id;
      }
    }
    return best;
  }

  private isPendingCancel(id: number): boolean {
    return this.pendingCancelIds.indexOf(id) >= 0;
  }

  private removePendingCancel(id: number): void {
    const index: number = this.pendingCancelIds.indexOf(id);
    if (index >= 0) {
      this.pendingCancelIds.splice(index, 1);
    }
  }

  private clearCancelTimer(id: number): void {
    const timer: number | undefined = this.cancelTimers.get(id);
    if (timer !== undefined) {
      clearTimeout(timer);
      this.cancelTimers.delete(id);
    }
  }

  private finalizeCancel(id: number): void {
    this.clearCancelTimer(id);
    this.removePendingCancel(id);
    const pointerId: number | undefined = this.contactIds.get(id);
    const point: number[] | undefined = this.points.get(id);
    this.points.delete(id);
    this.contactIds.delete(id);
    if (pointerId !== undefined && point !== undefined) {
      this.move.flush(pointerId);
      this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], 0);
    }
  }

  private finalizeAllCancels(): void {
    const ids: number[] = this.pendingCancelIds.slice();
    for (let i = 0; i < ids.length; i++) {
      this.finalizeCancel(ids[i]);
    }
  }

  private static normalizePressure(pressure: number | undefined): number {
    if (pressure === undefined || pressure <= 0) {
      return 0;
    }
    return Math.min(1, pressure / 65535);
  }
}
