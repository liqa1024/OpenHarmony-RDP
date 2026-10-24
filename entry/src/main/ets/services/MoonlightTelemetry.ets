import { hilog } from '@kit.PerformanceAnalysisKit';
import { MoonlightTelemetry } from 'libhmrdp.so';

// Same domain / tag as the native logger, so a `perf:` grep finds the line.
const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMoonlight';

// How long a reading a network hiccup produced stays readable: a resync request or a latency spike is
// a random event nobody is watching for at the moment it happens (moonlight-session.md §2).
const TRAIL_SAMPLES: number = 10;

// One sampler tick as the trail keeps it: levels are the window's readings (-1 = none stated), counts
// are what the window added.
class TrailSample {
  rttMs: number = -1;
  lossBp: number = -1;
  idrRequests: number = 0;
  inputDropped: number = 0;
}

export class MoonlightCounters {
  rttMs: number = -1;
  rttVarianceMs: number = -1;
  fps: number = 0;
  bytesPerSec: number = 0;
  hostLatencyMs: number = -1;
  decodeMs: number = -1;
  pendingVideoFrames: number = 0;
  // Total loss per second: input-budget drops + display-gate drops + hand-off replacements.
  droppedPerSec: number = 0;
  // In-window counts of what reached the screen and of the loss given up because the display's gear
  // was below the stream rate; their ratio names a stream the panel cannot take (RefreshHintWatch).
  presentedFrames: number = 0;
  refreshDroppedFrames: number = 0;
  // The display's current gear; diagnosis only.
  displayRefreshRateHz: number = 0;
  // Loss basis points: (FEC failed + out-of-order + invalid) / (video + FEC).
  lossBp: number = -1;
  // Over the last TRAIL_SAMPLES windows: worst level (RTT / loss, -1 when none) and what happened
  // (resync requests, input drops). Slides, so an event ages out of its own reading.
  trailRttPeakMs: number = -1;
  trailLossPeakBp: number = -1;
  trailIdrRequests: number = 0;
  trailInputDropped: number = 0;
  // Per presented frame: convert / 超分 / letterbox GPU time, their sum, and the display wait.
  gpuCopyUs: number = 0;
  gpuSrUs: number = 0;
  gpuBlitUs: number = 0;
  gpuFrameUs: number = 0;
  presentWaitUs: number = 0;
}

// The native sample is cumulative: diffs give rates (fps / bitrate) and the
// sums divided by their frame counts give per-frame means.
export class MoonlightTelemetryAggregator {
  private hasPrev: boolean = false;
  private prevFrames: number = 0;
  private prevPresented: number = 0;
  private prevBytes: number = 0;
  private prevHostSum: number = 0;
  private prevHostCount: number = 0;
  private prevDecodeSum: number = 0;
  private prevDecodeCount: number = 0;
  private prevDropped: number = 0;
  private prevPresentDropped: number = 0;
  private prevRefreshDropped: number = 0;
  private prevHandoff: number = 0;
  private prevVideo: number = 0;
  private prevFec: number = 0;
  private prevFailed: number = 0;
  private prevOos: number = 0;
  private prevInvalid: number = 0;
  private prevIdrRequests: number = 0;
  private hasInput: boolean = false;
  private prevInputDropped: number = 0;
  private trail: TrailSample[] = [];

  // `inputDropped` is the pipeline's cumulative drop count (L1 budget drops + sink rejects): the
  // rate names a stream that is over its budget, so a `perf:` grep shows it without a live watch.
  sample(raw: MoonlightTelemetry, windowMs: number, inputDropped: number): MoonlightCounters {
    const counters: MoonlightCounters = new MoonlightCounters();
    counters.rttMs = raw.rttMs;
    counters.rttVarianceMs = raw.rttVarianceMs;
    counters.pendingVideoFrames = raw.pendingVideoFrames;
    // Re-baseline on the first sample and across a session restart (counters
    // reset to zero, so any decrease means a new session).
    if (!this.hasPrev || raw.frames < this.prevFrames || raw.bytes < this.prevBytes
      || raw.presentedFrames < this.prevPresented) {
      this.hasPrev = true;
      this.capture(raw);
      this.hasInput = true;
      this.prevInputDropped = inputDropped;
      // A new session's resync count restarts, so an old trail would name an event it never had.
      this.trail = [];
      return counters;
    }

    const dFrames: number = raw.frames - this.prevFrames;
    const dPresented: number = raw.presentedFrames - this.prevPresented;
    const dBytes: number = raw.bytes - this.prevBytes;
    const dHostSum: number = raw.hostLatencyTenthMs - this.prevHostSum;
    const dHostCount: number = raw.hostLatencyFrames - this.prevHostCount;
    const dDecodeSum: number = raw.decodeRoundTripUs - this.prevDecodeSum;
    const dDecodeCount: number = raw.decodeRoundTripFrames - this.prevDecodeCount;
    const dDropped: number = raw.droppedFrames - this.prevDropped;
    const dPresentDropped: number = raw.presentDroppedFrames - this.prevPresentDropped;
    const dRefreshDropped: number = raw.refreshDroppedFrames - this.prevRefreshDropped;
    const dHandoff: number = raw.handoffReplacedFrames - this.prevHandoff;
    const dVideo: number = raw.packetVideo - this.prevVideo;
    const dFec: number = raw.packetFec - this.prevFec;
    const dFailed: number = raw.packetFecFailed - this.prevFailed;
    const dOos: number = raw.packetOos - this.prevOos;
    const dInvalid: number = raw.packetInvalid - this.prevInvalid;
    // The library resets this per session and the trail is cleared on that same sample, so a
    // backwards step could only be a half-tick race.
    const dIdr: number = raw.idrRequests > this.prevIdrRequests
      ? raw.idrRequests - this.prevIdrRequests : 0;
    this.capture(raw);
    counters.presentedFrames = dPresented;
    counters.refreshDroppedFrames = dRefreshDropped;
    counters.displayRefreshRateHz = raw.displayRefreshRateHz;

    const seconds: number = windowMs > 0 ? windowMs / 1000 : 0;
    if (seconds > 0) {
      counters.fps = Math.round(dPresented / seconds);
      counters.bytesPerSec = Math.round(dBytes / seconds);
      counters.droppedPerSec = Math.round((dDropped + dPresentDropped + dHandoff) / seconds);
    }
    if (dPresented > 0) {
      counters.gpuCopyUs = Math.floor(raw.gpuCopyUs / dPresented);
      counters.gpuSrUs = Math.floor(raw.gpuSrUs / dPresented);
      counters.gpuBlitUs = Math.floor(raw.gpuBlitUs / dPresented);
      counters.gpuFrameUs = counters.gpuCopyUs + counters.gpuSrUs + counters.gpuBlitUs;
      counters.presentWaitUs = Math.floor(raw.presentWaitUs / dPresented);
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
    const dInput: number = this.inputDelta(inputDropped);
    this.pushTrail(raw.rttMs, counters.lossBp, dIdr, dInput);
    this.readTrail(counters);
    this.log(counters, seconds, seconds > 0 ? Math.round(dInput / seconds) : 0, dPresented, dFrames);
    return counters;
  }

  // The window's input-drop count; the perf line divides it by the window.
  private inputDelta(inputDropped: number): number {
    if (!this.hasInput) {
      this.hasInput = true;
      this.prevInputDropped = inputDropped;
      return 0;
    }
    const delta: number = inputDropped - this.prevInputDropped;
    this.prevInputDropped = inputDropped;
    return delta > 0 ? delta : 0;
  }

  private pushTrail(rttMs: number, lossBp: number, idrRequests: number, inputDropped: number): void {
    const sample = new TrailSample();
    sample.rttMs = rttMs;
    sample.lossBp = lossBp;
    sample.idrRequests = idrRequests;
    sample.inputDropped = inputDropped;
    this.trail.push(sample);
    while (this.trail.length > TRAIL_SAMPLES) {
      this.trail.shift();
    }
  }

  // -1 keeps the "no reading" convention of the per-window fields rather than meaning zero.
  private readTrail(counters: MoonlightCounters): void {
    let rttPeak: number = -1;
    let lossPeak: number = -1;
    let idr: number = 0;
    let input: number = 0;
    for (const sample of this.trail) {
      if (sample.rttMs >= 0 && (rttPeak < 0 || sample.rttMs > rttPeak)) {
        rttPeak = sample.rttMs;
      }
      if (sample.lossBp >= 0 && (lossPeak < 0 || sample.lossBp > lossPeak)) {
        lossPeak = sample.lossBp;
      }
      idr += sample.idrRequests;
      input += sample.inputDropped;
    }
    counters.trailRttPeakMs = rttPeak;
    counters.trailLossPeakBp = lossPeak;
    counters.trailIdrRequests = idr;
    counters.trailInputDropped = input;
  }

  private capture(raw: MoonlightTelemetry): void {
    this.prevFrames = raw.frames;
    this.prevPresented = raw.presentedFrames;
    this.prevBytes = raw.bytes;
    this.prevHostSum = raw.hostLatencyTenthMs;
    this.prevHostCount = raw.hostLatencyFrames;
    this.prevDecodeSum = raw.decodeRoundTripUs;
    this.prevDecodeCount = raw.decodeRoundTripFrames;
    this.prevDropped = raw.droppedFrames;
    this.prevPresentDropped = raw.presentDroppedFrames;
    this.prevRefreshDropped = raw.refreshDroppedFrames;
    this.prevHandoff = raw.handoffReplacedFrames;
    this.prevVideo = raw.packetVideo;
    this.prevFec = raw.packetFec;
    this.prevFailed = raw.packetFecFailed;
    this.prevOos = raw.packetOos;
    this.prevInvalid = raw.packetInvalid;
    this.prevIdrRequests = raw.idrRequests;
  }

  private log(counters: MoonlightCounters, seconds: number, inputDropPerSec: number,
    presented: number, decoded: number): void {
    if (seconds <= 0) {
      return;
    }
    const line: string = 'perf: moonlight rtt=' + counters.rttMs + 'ms jitter='
      + counters.rttVarianceMs + 'ms host=' + Math.round(counters.hostLatencyMs * 10) / 10
      + 'ms decode=' + Math.round(counters.decodeMs * 10) / 10 + 'ms fps=' + counters.fps
      + ' bitrate=' + Math.round(counters.bytesPerSec * 8 / 1000) + 'kbps pending='
      + counters.pendingVideoFrames + ' drop=' + counters.droppedPerSec + '/s'
      + ' loss=' + counters.lossBp + '/10000 inputDrop=' + inputDropPerSec + '/s'
      + ' presented=' + presented + ' decoded=' + decoded
      + ' refreshDrop=' + counters.refreshDroppedFrames + ' panel=' + counters.displayRefreshRateHz + 'Hz'
      + ' trail rttPeak=' + counters.trailRttPeakMs + 'ms lossPeak=' + counters.trailLossPeakBp
      + ' resync=' + counters.trailIdrRequests + ' trailInputDrop=' + counters.trailInputDropped
      + ' gpu convert=' + counters.gpuCopyUs + ' sr=' + counters.gpuSrUs
      + ' blit=' + counters.gpuBlitUs + ' us/frame wait=' + counters.presentWaitUs + 'us';
    hilog.info(DOMAIN, TAG, '%{public}s', line);
  }
}
