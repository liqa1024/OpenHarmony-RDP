/*
 * HmRdp - UI-thread facade for the audio worker.
 *
 * Owns the one audio worker this process uses and forwards control to it. The
 * worker holds the ArkTS AudioRenderer and pulls PCM from the native sink ring;
 * this facade never touches the renderer. See doc_agent/architecture.md §4.4.
 */
import { ErrorEvent, worker } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AudioOps, AudioMessage } from './AudioProtocol';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpAudio';

class AudioPlayerImpl {
  private thread: worker.ThreadWorker | undefined = undefined;

  /** Lazily starts the audio worker (realtime work needs its own thread). */
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

  /** Begins servicing a session's audio (no renderer until a format arrives). */
  start(handle: number): void {
    if (handle <= 0) {
      return;
    }
    const message: AudioMessage = { op: AudioOps.START, handle: handle };
    this.ensure().postMessage(message, []);
  }

  /** Releases a session's renderer (disconnect / window close). */
  stop(handle: number): void {
    const thread: worker.ThreadWorker | undefined = this.thread;
    if (handle <= 0 || thread === undefined) {
      return;
    }
    const message: AudioMessage = { op: AudioOps.STOP, handle: handle };
    thread.postMessage(message, []);
  }

  /** Native reported the session's PCM format: build / rebuild the renderer. */
  onFormat(handle: number, rate: number, channels: number): void {
    if (handle <= 0 || rate <= 0 || channels <= 0) {
      return;
    }
    const message: AudioMessage =
      { op: AudioOps.FORMAT, handle: handle, rate: rate, channels: channels };
    this.ensure().postMessage(message, []);
  }
}

export const AudioPlayer: AudioPlayerImpl = new AudioPlayerImpl();
