export type MoveFlush<T> = (id: number, value: T) => void;

export interface MoveDelivery<T> {
  post(id: number, value: T): void;
  flush(id: number): void;
  cancel(): void;
  setInterval(ms: number): void;
}

// Straight-through delivery: the transport coalesces the stream itself, so nothing is ever pending.
export class ImmediateMoveDelivery<T> implements MoveDelivery<T> {
  private onFlush: MoveFlush<T>;

  constructor(onFlush: MoveFlush<T>) {
    this.onFlush = onFlush;
  }

  post(id: number, value: T): void {
    this.onFlush(id, value);
  }

  flush(_id: number): void {
  }

  cancel(): void {
  }

  setInterval(_ms: number): void {
  }
}
