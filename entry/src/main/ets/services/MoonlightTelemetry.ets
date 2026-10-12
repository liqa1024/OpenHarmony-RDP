import { hilog } from '@kit.PerformanceAnalysisKit';
import { MoonlightTelemetry } from 'libhmrdp.so';

// Same domain / tag as the native logger, so a `perf:` grep finds the line.
const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMoonlight';

export class MoonlightCounters {
  rttMs: number = -1;
  rttVarianceMs: number = -1;
  fps: number = 0;
  bytesPerSec: number = 0;
  // Host frame processing latency, ms (per-frame mean; -1 when the host omits it).
  hostLatencyMs: number = -1;
  // Submit -> decoded output round trip, ms (per-frame mean; -1 with no paired
  // frame yet). Includes codec queue and output-buffer wait, so it bounds the
  // pure decode cost rather than equal to it.
  decodeMs: number = -1;
  pendingVideoFrames: number = 0;
  // Loss basis points: (FEC failed + out-of-order + invalid) / (video + FEC).
  lossBp: number = -1;
}

// The native sample is cumulative: diffs give rates (fps / bitrate) and the
// sums divided by their frame counts give per-frame means.
export class MoonlightTelemetryAggregator {
  private hasPrev: boolean = false;
  private prevFrames: number = 0;
  private prevBytes: number = 0;
  private prevHostSum: number = 0;
  private prevHostCount: number = 0;
  private prevDecodeSum: number = 0;
  private prevDecodeCount: number = 0;
  private prevVideo: number = 0;
  private prevFec: number = 0;
  private prevFailed: number = 0;
  private prevOos: number = 0;
  private prevInvalid: number = 0;

  sample(raw: MoonlightTelemetry, windowMs: number): MoonlightCounters {
    const counters: MoonlightCounters = new MoonlightCounters();
    counters.rttMs = raw.rttMs;
    counters.rttVarianceMs = raw.rttVarianceMs;
    counters.pendingVideoFrames = raw.pendingVideoFrames;
    // Re-baseline on the first sample and across a session restart (counters
    // reset to zero, so any decrease means a new session).
    if (!this.hasPrev || raw.frames < this.prevFrames || raw.bytes < this.prevBytes) {
      this.hasPrev = true;
      this.capture(raw);
      return counters;
    }

    const dFrames: number = raw.frames - this.prevFrames;
    const dBytes: number = raw.bytes - this.prevBytes;
    const dHostSum: number = raw.hostLatencyTenthMs - this.prevHostSum;
    const dHostCount: number = raw.hostLatencyFrames - this.prevHostCount;
    const dDecodeSum: number = raw.decodeRoundTripUs - this.prevDecodeSum;
    const dDecodeCount: number = raw.decodeRoundTripFrames - this.prevDecodeCount;
    const dVideo: number = raw.packetVideo - this.prevVideo;
    const dFec: number = raw.packetFec - this.prevFec;
    const dFailed: number = raw.packetFecFailed - this.prevFailed;
    const dOos: number = raw.packetOos - this.prevOos;
    const dInvalid: number = raw.packetInvalid - this.prevInvalid;
    this.capture(raw);

    const seconds: number = windowMs > 0 ? windowMs / 1000 : 0;
    if (seconds > 0) {
      counters.fps = Math.round(dFrames / seconds);
      counters.bytesPerSec = Math.round(dBytes / seconds);
    }
    if (dHostCount > 0) {
      counters.hostLatencyMs = dHostSum / dHostCount / 10;
    }
    if (dDecodeCount > 0) {
      counters.decodeMs = dDecodeSum / dDecodeCount / 1000;
    }
    const packets: number = dVideo + dFec;
    if (packets > 0) {
      counters.lossBp = Math.floor((dFailed + dOos + dInvalid) * 10000 / packets);
    }
    this.log(counters, seconds);
    return counters;
  }

  private capture(raw: MoonlightTelemetry): void {
    this.prevFrames = raw.frames;
    this.prevBytes = raw.bytes;
    this.prevHostSum = raw.hostLatencyTenthMs;
    this.prevHostCount = raw.hostLatencyFrames;
    this.prevDecodeSum = raw.decodeRoundTripUs;
    this.prevDecodeCount = raw.decodeRoundTripFrames;
    this.prevVideo = raw.packetVideo;
    this.prevFec = raw.packetFec;
    this.prevFailed = raw.packetFecFailed;
    this.prevOos = raw.packetOos;
    this.prevInvalid = raw.packetInvalid;
  }

  private log(counters: MoonlightCounters, seconds: number): void {
    if (seconds <= 0) {
      return;
    }
    const line: string = 'perf: moonlight rtt=' + counters.rttMs + 'ms jitter='
      + counters.rttVarianceMs + 'ms host=' + Math.round(counters.hostLatencyMs * 10) / 10
      + 'ms decode=' + Math.round(counters.decodeMs * 10) / 10 + 'ms fps=' + counters.fps
      + ' bitrate=' + Math.round(counters.bytesPerSec * 8 / 1000) + 'kbps pending='
      + counters.pendingVideoFrames + ' loss=' + counters.lossBp + '/10000';
    hilog.info(DOMAIN, TAG, '%{public}s', line);
  }
}
