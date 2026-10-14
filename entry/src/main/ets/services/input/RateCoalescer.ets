export type CoalesceFlush<T> = (id: number, value: T) => void;

// Latest-value-wins rate limiter for continuous position streams: one self-rescheduling timer
// serves every key; a discrete event flushes its key immediately so ordering/position hold.
export class RateCoalescer<T> {
  private minIntervalMs: number;
  private onFlush: CoalesceFlush<T>;
  private pending: Map<number, T> = new Map<number, T>();
  private timer: number = -1;
  private lastFlushAt: number = 0;

  constructor(minIntervalMs: number, onFlush: CoalesceFlush<T>) {
    this.minIntervalMs = minIntervalMs;
    this.onFlush = onFlush;
  }

  setInterval(ms: number): void {
    this.minIntervalMs = ms;
  }

  post(id: number, value: T): void {
    this.pending.set(id, value);
    const elapsed: number = Date.now() - this.lastFlushAt;
    if (elapsed >= this.minIntervalMs) {
      this.flushAll();
    } else if (this.timer === -1) {
      this.timer = setTimeout((): void => {
        this.timer = -1;
        this.flushAll();
      }, this.minIntervalMs - elapsed);
    }
  }

  flush(id: number): void {
    const value: T | undefined = this.pending.get(id);
    if (value === undefined) {
      return;
    }
    this.pending.delete(id);
    this.lastFlushAt = Date.now();
    this.onFlush(id, value);
  }

  flushAll(): void {
    if (this.timer !== -1) {
      clearTimeout(this.timer);
      this.timer = -1;
    }
    if (this.pending.size === 0) {
      return;
    }
    const ids: number[] = [];
    this.pending.forEach((_value: T, id: number): void => {
      ids.push(id);
    });
    this.lastFlushAt = Date.now();
    for (let i = 0; i < ids.length; i++) {
      const value: T | undefined = this.pending.get(ids[i]);
      if (value !== undefined) {
        this.onFlush(ids[i], value);
      }
    }
    this.pending.clear();
  }

  cancel(): void {
    if (this.timer !== -1) {
      clearTimeout(this.timer);
      this.timer = -1;
    }
    this.pending.clear();
  }
}
