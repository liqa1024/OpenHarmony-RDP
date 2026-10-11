import { ErrorEvent, worker } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AudioOps, AudioMessage, AudioSource } from './AudioProtocol';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpAudio';

class AudioPlayerImpl {
  private thread: worker.ThreadWorker | undefined = undefined;

  private ensure(): worker.ThreadWorker {
    const existing: worker.ThreadWorker | undefined = this.thread;
    if (existing !== undefined) {
      return existing;
    }
    const thread: worker.ThreadWorker =
      new worker.ThreadWorker('entry/ets/workers/audio.worker.ets', { name: 'hmrdp-audio' });
    thread.onerror = (e: ErrorEvent): void => {
      hilog.error(DOMAIN, TAG, 'audio worker error: %{public}s', e.message);
    };
    this.thread = thread;
    return thread;
  }

  // A dead worker must not throw into the caller.
  private post(thread: worker.ThreadWorker, message: AudioMessage, transfer: ArrayBuffer[]): void {
    try {
      thread.postMessage(message, transfer);
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'audio worker post failed: %{public}s', JSON.stringify(error));
    }
  }

  start(handle: number): void {
    if (handle <= 0) {
      return;
    }
    const message: AudioMessage = { op: AudioOps.START, handle: handle };
    this.post(this.ensure(), message, []);
  }

  stop(handle: number): void {
    const thread: worker.ThreadWorker | undefined = this.thread;
    if (handle <= 0 || thread === undefined) {
      return;
    }
    const message: AudioMessage = { op: AudioOps.STOP, handle: handle };
    this.post(thread, message, []);
  }

  onFormat(handle: number, rate: number, channels: number): void {
    if (handle <= 0 || rate <= 0 || channels <= 0) {
      return;
    }
    const message: AudioMessage =
      { op: AudioOps.FORMAT, handle: handle, rate: rate, channels: channels };
    this.post(this.ensure(), message, []);
  }

  // Moonlight audio is a single process-global ring, so it carries no handle.
  startMoonlight(): void {
    const message: AudioMessage = { op: AudioOps.START, source: AudioSource.MOONLIGHT, handle: 0 };
    this.post(this.ensure(), message, []);
  }

  stopMoonlight(): void {
    const thread: worker.ThreadWorker | undefined = this.thread;
    if (thread === undefined) {
      return;
    }
    const message: AudioMessage = { op: AudioOps.STOP, source: AudioSource.MOONLIGHT, handle: 0 };
    this.post(thread, message, []);
  }
}

export const AudioPlayer: AudioPlayerImpl = new AudioPlayerImpl();
