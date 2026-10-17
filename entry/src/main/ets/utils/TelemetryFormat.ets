// Telemetry display helpers shared by the RDP and Moonlight toolbars: both report per-frame GPU
// microseconds and the same convert/超分/blit split, only the first phase's label differs.
export function usToMs(us: number): number {
  return Math.round(us / 1000);
}

export function usToMsTenth(us: number): string {
  return (us / 1000).toFixed(1);
}

export function gpuFrameLabel(frameUs: number): string {
  return frameUs > 0 ? 'GPU ' + usToMsTenth(frameUs) + 'ms' : 'GPU --';
}

export function gpuSplitLabel(copyUs: number, srUs: number, blitUs: number,
  copyLabel: string): string {
  return copyLabel + ' ' + usToMsTenth(copyUs) + ' / 超分 ' + usToMsTenth(srUs)
    + ' / blit ' + usToMsTenth(blitUs) + ' ms';
}
