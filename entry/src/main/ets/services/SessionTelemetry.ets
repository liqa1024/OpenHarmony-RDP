/*
 * HmRdp - per-session telemetry aggregation (ArkTS side).
 *
 * Native exposes only raw samples (RdpTelemetry): one window's transport /
 * frame deltas and drained work / audio / GPU sums. This aggregator turns them
 * into the toolbar's display figures (rates, per-frame averages, duty, loss
 * rate) and owns the short audio-loss window. Keeping the arithmetic here is
 * what lets native stay a plain data source and never know what the toolbar
 * shows (doc_agent/architecture.md §3.1).
 */
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RdpTelemetry } from 'libhmrdp.so';

// Same domain / tag as the native logger, so a `perf:` grep keeps working after
// the line moved to the aggregation side.
const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpNative';
// The audio-loss rolling window, in samples (roughly one per second).
const AUDIO_WINDOW: number = 5;

/** One computed telemetry window: exactly the figures the toolbar shows. */
export class RdpCounters {
  rttMs: number = -1;
  rxBytesPerSec: number = 0;
  txBytesPerSec: number = 0;
  fps: number = 0;
  audioRateHz: number = -1;
  audioLossBp: number = -1;
  zgxParseUs: number = 0;
  decodeUs: number = 0;
  composeUs: number = 0;
  presentUs: number = 0;
  bytesPerFrame: number = 0;
  dutyPermille: number = 0;
  commandsPerFrame: number = 0;
  syncUs: number = 0;
  presentWaitUs: number = 0;
  gpuCopyUs: number = 0;
  gpuSrUs: number = 0;
  gpuBlitUs: number = 0;
}

/**
 * Turns the raw SessionTelemetry samples of one session into display windows.
 * The window deltas / sums are normalized by the sample's own `windowElapsedMs`
 * and `windowFrames`, so the figures do not depend on how often the caller
 * pulls. One instance per session.
 */
export class SessionTelemetry {
  private hasSeq: boolean = false;
  private lastSeq: number = 0;
  private audioLost: number[] = new Array<number>(AUDIO_WINDOW).fill(0);
  private audioTotal: number[] = new Array<number>(AUDIO_WINDOW).fill(0);
  private audioIndex: number = 0;

  /**
   * One display window from one raw sample. Returns undefined for the very first
   * sample (no window yet: windowElapsedMs is 0) and for a sample the caller
   * already consumed, so the toolbar keeps its current figures either way.
   */
  sample(raw: RdpTelemetry): RdpCounters | undefined {
    if (raw.windowElapsedMs === 0) {
      return undefined;
    }
    // The UI cadence is not locked to the native tick, so the same window may be
    // pulled twice; fold each window into the rolling figures only once.
    if (this.hasSeq && raw.sampleSeq === this.lastSeq) {
      return undefined;
    }
    this.hasSeq = true;
    this.lastSeq = raw.sampleSeq;

    const seconds: number = raw.windowElapsedMs / 1000;
    const counters: RdpCounters = new RdpCounters();
    counters.rttMs = raw.rttMs;
    counters.rxBytesPerSec = Math.round(raw.rxBytesWindow / seconds);
    counters.txBytesPerSec = Math.round(raw.txBytesWindow / seconds);
    counters.fps = Math.round(raw.framesWindow / seconds);

    const frames: number = raw.windowFrames;
    const perFrame = (value: number): number => frames > 0 ? Math.floor(value / frames) : 0;
    counters.zgxParseUs = perFrame(raw.zgxParseUs);
    counters.decodeUs = perFrame(raw.decodeUs);
    counters.composeUs = perFrame(raw.composeUs);
    counters.presentUs = perFrame(raw.presentUs);
    counters.syncUs = perFrame(raw.syncUs);
    counters.presentWaitUs = perFrame(raw.presentWaitUs);
    counters.bytesPerFrame = perFrame(raw.windowBytes);
    counters.commandsPerFrame = perFrame(raw.windowCommands);
    counters.gpuCopyUs = perFrame(raw.gpuCopyUs);
    counters.gpuSrUs = perFrame(raw.gpuSrUs);
    counters.gpuBlitUs = perFrame(raw.gpuBlitUs);

    const workUs: number = raw.zgxParseUs + raw.decodeUs + raw.composeUs + raw.presentUs;
    const windowUs: number = raw.windowElapsedMs * 1000;
    counters.dutyPermille = windowUs > 0 ? Math.floor(workUs * 1000 / windowUs) : 0;

    this.sampleAudio(raw, counters);
    this.log(counters, raw, seconds);
    return counters;
  }

  private sampleAudio(raw: RdpTelemetry, counters: RdpCounters): void {
    this.audioLost[this.audioIndex] = raw.audioLostBytes;
    this.audioTotal[this.audioIndex] = raw.audioTotalBytes;
    this.audioIndex = (this.audioIndex + 1) % AUDIO_WINDOW;
    let lostSum: number = 0;
    let totalSum: number = 0;
    for (let i = 0; i < AUDIO_WINDOW; i++) {
      lostSum += this.audioLost[i];
      totalSum += this.audioTotal[i];
    }
    if (totalSum > 0) {
      counters.audioRateHz = raw.audioRateHz;
      counters.audioLossBp = Math.floor(lostSum * 10000 / totalSum);
    }
  }

  /**
   * The per-second `perf:` diagnostic line (doc_agent/session-and-input.md §3),
   * emitted from the same aggregation the toolbar uses and under the native
   * logger's domain / tag, so the existing `perf:` grep still finds it.
   */
  private log(counters: RdpCounters, raw: RdpTelemetry, seconds: number): void {
    const frames: number = raw.windowFrames;
    if (frames === 0) {
      return;
    }
    const workUs: number = counters.zgxParseUs + counters.decodeUs + counters.composeUs
      + counters.presentUs;
    const line: string = 'perf: work ' + workUs + ' us/frame = zgx+parse ' + counters.zgxParseUs
      + ' + decode ' + counters.decodeUs + ' + compose ' + counters.composeUs
      + ' + present ' + counters.presentUs
      + ' (+ sync ' + counters.syncUs + ' + presentWait ' + counters.presentWaitUs + ' blocked)'
      + ' (frames=' + frames + ' presents=' + Math.round(raw.framesWindow)
      + ' frames/s=' + Math.round(frames / seconds)
      + ' cmds/frame=' + counters.commandsPerFrame + ' kB/frame=' + Math.floor(counters.bytesPerFrame / 1024)
      + ' duty=' + Math.floor(counters.dutyPermille / 10) + '.' + (counters.dutyPermille % 10) + '%'
      + ' rx=' + counters.rxBytesPerSec + 'B/s fps=' + counters.fps
      + ' gpu copy=' + counters.gpuCopyUs + ' sr=' + counters.gpuSrUs
      + ' blit=' + counters.gpuBlitUs + ' us/frame)';
    hilog.info(DOMAIN, TAG, '%{public}s', line);
  }
}
