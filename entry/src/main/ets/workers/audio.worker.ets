/*
 * HmRdp - the audio worker: one ArkTS AudioRenderer per live session.
 *
 * Native keeps the FreeRDP sink and its PCM handoff ring; this worker owns the
 * renderer, so the player does not depend on OHAudio code inside libhmrdp. Each
 * renderer's writeData callback pulls from the ring by handle (audioRead), which
 * fills the buffer with queued audio plus silence on an underrun - so the callback
 * always reports a full, valid buffer and the buffering policy stays in one place
 * (the native sink).
 *
 * Separate from the background worker on purpose: a realtime audio callback must
 * not queue behind a large image decode or a file write (doc_agent/architecture.md
 * §4.4). Correlated renderer state is keyed by session handle, so several
 * sessions share this one worker without interfering.
 */
import { MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { audioFormat, audioRead, RdpAudioFormat } from 'libhmrdp.so';
import { AudioOps, AudioMessage } from '../services/AudioProtocol';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpAudio';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

/** Per-session renderer state. `generation` invalidates an in-flight rebuild. */
class SessionAudio {
  handle: number = 0;
  active: boolean = false;
  rate: number = 0;
  channels: number = 0;
  generation: number = 0;
  renderer: audio.AudioRenderer | undefined = undefined;
}

const sessions: Map<number, SessionAudio> = new Map<number, SessionAudio>();

function stateFor(handle: number): SessionAudio {
  let state: SessionAudio | undefined = sessions.get(handle);
  if (state === undefined) {
    state = new SessionAudio();
    state.handle = handle;
    sessions.set(handle, state);
  }
  return state;
}

/** Maps a PCM sample rate to the AudioKit enum (fallback: 48 kHz). */
function sampleRateEnum(rate: number): audio.AudioSamplingRate {
  if (rate === 8000) {
    return audio.AudioSamplingRate.SAMPLE_RATE_8000;
  }
  if (rate === 11025) {
    return audio.AudioSamplingRate.SAMPLE_RATE_11025;
  }
  if (rate === 16000) {
    return audio.AudioSamplingRate.SAMPLE_RATE_16000;
  }
  if (rate === 22050) {
    return audio.AudioSamplingRate.SAMPLE_RATE_22050;
  }
  if (rate === 24000) {
    return audio.AudioSamplingRate.SAMPLE_RATE_24000;
  }
  if (rate === 32000) {
    return audio.AudioSamplingRate.SAMPLE_RATE_32000;
  }
  if (rate === 44100) {
    return audio.AudioSamplingRate.SAMPLE_RATE_44100;
  }
  if (rate === 48000) {
    return audio.AudioSamplingRate.SAMPLE_RATE_48000;
  }
  if (rate === 96000) {
    return audio.AudioSamplingRate.SAMPLE_RATE_96000;
  }
  return audio.AudioSamplingRate.SAMPLE_RATE_48000;
}

/** Maps a channel count to the AudioKit enum (mono / stereo; fallback: stereo). */
function channelEnum(channels: number): audio.AudioChannel {
  return channels === 1 ? audio.AudioChannel.CHANNEL_1 : audio.AudioChannel.CHANNEL_2;
}

/** Releases the current renderer without waiting (the worker is not blocked). */
function releaseRenderer(state: SessionAudio): void {
  const renderer: audio.AudioRenderer | undefined = state.renderer;
  state.renderer = undefined;
  if (renderer !== undefined) {
    renderer.release().catch((error: BusinessError): void => {
      hilog.warn(DOMAIN, TAG, 'release failed: %{public}s', error.message);
    });
  }
}

/** (Re)builds the renderer for a session's current rate/channels. */
async function build(state: SessionAudio): Promise<void> {
  state.generation++;
  const generation: number = state.generation;
  releaseRenderer(state);
  if (!state.active) {
    return;
  }
  const handle: number = state.handle;
  const streamInfo: audio.AudioStreamInfo = {
    samplingRate: sampleRateEnum(state.rate),
    channels: channelEnum(state.channels),
    sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
    encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
  };
  const rendererInfo: audio.AudioRendererInfo = {
    usage: audio.StreamUsage.STREAM_USAGE_MUSIC,
    rendererFlags: 0
  };
  const options: audio.AudioRendererOptions = {
    streamInfo: streamInfo,
    rendererInfo: rendererInfo
  };
  try {
    const renderer: audio.AudioRenderer = await audio.createAudioRenderer(options);
    // A stop or a newer format may have landed while the renderer was opening.
    if (generation !== state.generation || !state.active) {
      renderer.release().catch((error: BusinessError): void => {});
      return;
    }
    state.renderer = renderer;
    renderer.on('writeData', (buffer: ArrayBuffer): audio.AudioDataCallbackResult => {
      // The native ring fills the whole buffer (audio + silence); the return
      // value is informational only.
      audioRead(handle, buffer);
      return audio.AudioDataCallbackResult.VALID;
    });
    // Only a shared resume needs an explicit restart; forced pause/stop is done
    // by the system.
    renderer.on('audioInterrupt', (event: audio.InterruptEvent): void => {
      if (event.hintType === audio.InterruptHint.INTERRUPT_HINT_RESUME) {
        renderer.start().catch((error: BusinessError): void => {});
      }
    });
    await renderer.start();
    hilog.info(DOMAIN, TAG, 'renderer start handle=%{public}d rate=%{public}d channels=%{public}d',
      handle, state.rate, state.channels);
  } catch (error) {
    hilog.warn(DOMAIN, TAG, 'renderer create/start failed: %{public}s', JSON.stringify(error));
  }
}

workerPort.onmessage = (e: MessageEvents): void => {
  const message: AudioMessage = e.data as AudioMessage;
  const handle: number = message.handle;
  if (handle <= 0) {
    return;
  }
  const state: SessionAudio = stateFor(handle);
  if (message.op === AudioOps.START) {
    state.active = true;
    // The format event may have fired before this window attached (or before the
    // worker existed), so seed from the session's current format instead of
    // waiting for the next change.
    if (state.rate <= 0 || state.channels <= 0) {
      const current: RdpAudioFormat = audioFormat(handle);
      if (current.rate > 0 && current.channels > 0) {
        state.rate = current.rate;
        state.channels = current.channels;
      }
    }
    if (state.rate > 0 && state.channels > 0) {
      build(state);
    }
  } else if (message.op === AudioOps.STOP) {
    state.active = false;
    state.generation++;
    releaseRenderer(state);
    sessions.delete(handle);
  } else if (message.op === AudioOps.FORMAT) {
    const rate: number = message.rate ?? 0;
    const channels: number = message.channels ?? 0;
    if (rate <= 0 || channels <= 0) {
      return;
    }
    const changed: boolean = state.rate !== rate || state.channels !== channels;
    state.rate = rate;
    state.channels = channels;
    if (state.active && (changed || state.renderer === undefined)) {
      build(state);
    }
  }
};
