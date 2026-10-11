import { MessageEvents, ThreadWorkerGlobalScope, worker } from '@kit.ArkTS';
import { audio } from '@kit.AudioKit';
import { BusinessError, SuppressWarnings, SuppressWarningsType } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { audioFormat, audioRead, moonlightAudioFormat, moonlightAudioRead,
  RdpAudioFormat } from 'libhmrdp.so';
import { AudioOps, AudioMessage, AudioSource } from '../services/AudioProtocol';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpAudio';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

// generation invalidates an in-flight rebuild.
class SessionAudio {
  handle: number = 0;
  source: number = AudioSource.RDP;
  active: boolean = false;
  rate: number = 0;
  channels: number = 0;
  generation: number = 0;
  renderer: audio.AudioRenderer | undefined = undefined;
}

const sessions: Map<number, SessionAudio> = new Map<number, SessionAudio>();

// The Moonlight ring is process-global, so it has one fixed session key.
const MOONLIGHT_KEY: number = -1;

function stateFor(key: number): SessionAudio {
  let state: SessionAudio | undefined = sessions.get(key);
  if (state === undefined) {
    state = new SessionAudio();
    state.handle = key;
    sessions.set(key, state);
  }
  return state;
}

// AudioKit references in one place so a single 多设备 (syscap) suppression covers them all.
@SuppressWarnings({ rules: [SuppressWarningsType.SYSCAP] })
class AudioFormats {
  static sampleRate(rate: number): audio.AudioSamplingRate {
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

  static channel(channels: number): audio.AudioChannel {
    if (channels === 1) {
      return audio.AudioChannel.CHANNEL_1;
    }
    if (channels === 3) {
      return audio.AudioChannel.CHANNEL_3;
    }
    if (channels === 4) {
      return audio.AudioChannel.CHANNEL_4;
    }
    if (channels === 5) {
      return audio.AudioChannel.CHANNEL_5;
    }
    if (channels === 6) {
      return audio.AudioChannel.CHANNEL_6;
    }
    if (channels === 7) {
      return audio.AudioChannel.CHANNEL_7;
    }
    if (channels === 8) {
      return audio.AudioChannel.CHANNEL_8;
    }
    return audio.AudioChannel.CHANNEL_2;
  }

  // Surround speaker layout; mono/stereo leave it unset so the 2-channel path is
  // unchanged. Only 5.1 / 7.1 are mapped yet.
  static channelLayout(channels: number): audio.AudioChannelLayout | undefined {
    if (channels === 6) {
      return audio.AudioChannelLayout.CH_LAYOUT_5POINT1;
    }
    if (channels === 8) {
      return audio.AudioChannelLayout.CH_LAYOUT_7POINT1;
    }
    return undefined;
  }

  static streamInfo(rate: number, channels: number): audio.AudioStreamInfo {
    const layout: audio.AudioChannelLayout | undefined = AudioFormats.channelLayout(channels);
    const info: audio.AudioStreamInfo = {
      samplingRate: AudioFormats.sampleRate(rate),
      channels: AudioFormats.channel(channels),
      sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
      encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW,
      channelLayout: layout
    };
    return info;
  }

  static rendererInfo(): audio.AudioRendererInfo {
    const info: audio.AudioRendererInfo = {
      usage: audio.StreamUsage.STREAM_USAGE_MUSIC,
      rendererFlags: 0
    };
    return info;
  }

  static create(options: audio.AudioRendererOptions): Promise<audio.AudioRenderer> {
    return audio.createAudioRenderer(options);
  }

  static dataCallbackOk(): audio.AudioDataCallbackResult {
    return audio.AudioDataCallbackResult.VALID;
  }

  static isResume(event: audio.InterruptEvent): boolean {
    return event.hintType === audio.InterruptHint.INTERRUPT_HINT_RESUME;
  }
}

function releaseRenderer(state: SessionAudio): void {
  const renderer: audio.AudioRenderer | undefined = state.renderer;
  state.renderer = undefined;
  if (renderer !== undefined) {
    renderer.release().catch((error: BusinessError): void => {
      hilog.warn(DOMAIN, TAG, 'release failed: %{public}s', error.message);
    });
  }
}

async function build(state: SessionAudio): Promise<void> {
  state.generation++;
  const generation: number = state.generation;
  releaseRenderer(state);
  if (!state.active) {
    return;
  }
  const handle: number = state.handle;
  const source: number = state.source;
  const options: audio.AudioRendererOptions = {
    streamInfo: AudioFormats.streamInfo(state.rate, state.channels),
    rendererInfo: AudioFormats.rendererInfo()
  };
  try {
    const renderer: audio.AudioRenderer = await AudioFormats.create(options);
    // A stop or a newer format may have landed while the renderer was opening.
    if (generation !== state.generation || !state.active) {
      renderer.release().catch((error: BusinessError): void => {});
      return;
    }
    state.renderer = renderer;
    renderer.on('writeData', (buffer: ArrayBuffer): audio.AudioDataCallbackResult => {
      // The native ring fills the whole buffer (audio + silence), so the result is always VALID.
      if (source === AudioSource.MOONLIGHT) {
        moonlightAudioRead(buffer);
      } else {
        audioRead(handle, buffer);
      }
      return AudioFormats.dataCallbackOk();
    });
    // Only a shared resume needs an explicit restart; forced pause/stop is done
    // by the system.
    renderer.on('audioInterrupt', (event: audio.InterruptEvent): void => {
      if (AudioFormats.isResume(event)) {
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
  const source: number = message.source ?? AudioSource.RDP;
  const handle: number = message.handle;
  if (source === AudioSource.RDP && handle <= 0) {
    return;
  }
  const key: number = source === AudioSource.MOONLIGHT ? MOONLIGHT_KEY : handle;
  const state: SessionAudio = stateFor(key);
  state.source = source;
  state.handle = handle;
  if (message.op === AudioOps.START) {
    state.active = true;
    // Seed the format from the current native format. Moonlight's is the Opus
    // decoder config (known from init, before the session reports connected); RDP
    // may not have sent a packet yet, so its FORMAT event can still arrive later.
    if (state.rate <= 0 || state.channels <= 0) {
      const current: RdpAudioFormat = source === AudioSource.MOONLIGHT
        ? moonlightAudioFormat() : audioFormat(handle);
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
    sessions.delete(key);
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
