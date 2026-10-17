export class DebouncedSaver {
  private timer: number = -1;
  private pending: boolean = false;
  private readonly delayMs: number;
  private readonly write: () => void;

  constructor(write: () => void, delayMs: number = 400) {
    this.write = write;
    this.delayMs = delayMs;
  }

  schedule(): void {
    if (this.timer !== -1) {
      clearTimeout(this.timer);
    }
    this.pending = true;
    this.timer = setTimeout(() => {
      this.timer = -1;
      this.pending = false;
      this.write();
    }, this.delayMs);
  }

  flush(): void {
    if (!this.pending) {
      return;
    }
    if (this.timer !== -1) {
      clearTimeout(this.timer);
      this.timer = -1;
    }
    this.pending = false;
    this.write();
  }

  cancel(): void {
    if (this.timer !== -1) {
      clearTimeout(this.timer);
      this.timer = -1;
    }
    this.pending = false;
  }
}
