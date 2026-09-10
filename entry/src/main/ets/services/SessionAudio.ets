/*
 * HmRdp - plays the PCM stream produced by the native rdpsnd sink.
 *
 * The native side decodes RDP audio to 16-bit PCM and pushes each packet
 * through the Node-API bridge; this service feeds it to a streaming
 * AudioRenderer. The renderer pulls data from a bounded queue (writeData
 * callback), so a stalled UI thread can never make playback latency grow
 * without bound.
 */
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD005;
const TAG: string = 'HmRdpAudio';
// Cap the renderer queue (~1.3s of 48kHz stereo 16-bit PCM); drop the oldest
// data when the server outruns playback.
const MAX_QUEUED_BYTES: number = 512 * 1024;

export class SessionAudio {
  private renderer: audio.AudioRenderer | undefined = undefined;
  private rate: number = 0;
  private channels: number = 0;
  private chunks: Uint8Array[] = [];
  private queuedBytes: number = 0;
  // Chunks received while the renderer is being created asynchronously.
  private pendingChunks: Uint8Array[] = [];
  private pendingRate: number = 0;
  private pendingChannels: number = 0;
  private creating: boolean = false;
  private released: boolean = false;

  /**
   * Queues one PCM packet. Safe to call from the UI thread; the renderer is
   * created lazily on the first packet, once the negotiated format is known.
   */
  push(data: ArrayBuffer, sampleRate: number, channels: number): void {
    if (this.released || data.byteLength === 0 || sampleRate <= 0) {
      return;
    }
    const chunk: Uint8Array = new Uint8Array(data);
    if (this.renderer === undefined) {
      if (this.creating) {
        if (sampleRate === this.pendingRate && channels === this.pendingChannels) {
          this.pendingChunks.push(chunk);
        }
        return;
      }
      this.startCreate(sampleRate, channels, chunk);
      return;
    }
    if (sampleRate !== this.rate || channels !== this.channels) {
      // The negotiated format changed: rebuild the renderer for the new one.
      this.disposeRenderer();
      this.startCreate(sampleRate, channels, chunk);
      return;
    }
    this.enqueue(chunk);
  }

  private startCreate(sampleRate: number, channels: number, chunk: Uint8Array): void {
    this.creating = true;
    this.pendingRate = sampleRate;
    this.pendingChannels = channels;
    this.pendingChunks.push(chunk);
    this.createRenderer(sampleRate, channels);
  }

  private async createRenderer(sampleRate: number, channels: number): Promise<void> {
    try {
      const streamInfo: audio.AudioStreamInfo = {
        samplingRate: SessionAudio.mapRate(sampleRate),
        channels: channels >= 2 ? audio.AudioChannel.CHANNEL_2 : audio.AudioChannel.CHANNEL_1,
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
      const renderer: audio.AudioRenderer = await audio.createAudioRenderer(options);
      if (this.released) {
        await renderer.release();
        return;
      }
      renderer.on('writeData', this.writeDataCallback);
      renderer.on('audioInterrupt', this.interruptCallback);
      this.renderer = renderer;
      this.rate = sampleRate;
      this.channels = channels;
      const buffered: Uint8Array[] = this.pendingChunks;
      this.pendingChunks = [];
      for (const chunk of buffered) {
        this.enqueue(chunk);
      }
      this.creating = false;
      await renderer.start();
      hilog.info(DOMAIN, TAG, 'audio renderer started: rate=%{public}d channels=%{public}d',
        sampleRate, channels);
    } catch (error) {
      this.creating = false;
      hilog.error(DOMAIN, TAG, 'create audio renderer failed: %{public}s',
        JSON.stringify(error as BusinessError));
    }
  }

  private enqueue(chunk: Uint8Array): void {
    if (chunk.length >= MAX_QUEUED_BYTES) {
      this.chunks = [];
      this.queuedBytes = 0;
      return;
    }
    while (this.chunks.length > 0 && this.queuedBytes + chunk.length > MAX_QUEUED_BYTES) {
      const dropped: Uint8Array | undefined = this.chunks.shift();
      if (dropped !== undefined) {
        this.queuedBytes -= dropped.length;
      }
    }
    this.chunks.push(chunk);
    this.queuedBytes += chunk.length;
  }

  // Renderer pulls: if the queue runs dry we emit silence so playback (and the
  // callback cadence) never stalls.
  private writeDataCallback: audio.AudioRendererWriteDataCallback =
    (buffer: ArrayBuffer): audio.AudioDataCallbackResult => {
      const out: Uint8Array = new Uint8Array(buffer);
      let offset: number = 0;
      while (offset < out.length && this.chunks.length > 0) {
        const head: Uint8Array | undefined = this.chunks[0];
        if (head === undefined) {
          break;
        }
        const room: number = out.length - offset;
        if (head.length <= room) {
          out.set(head, offset);
          offset += head.length;
          this.chunks.shift();
          this.queuedBytes -= head.length;
        } else {
          out.set(head.subarray(0, room), offset);
          offset += room;
          this.chunks[0] = head.subarray(room);
          this.queuedBytes -= room;
        }
      }
      if (offset < out.length) {
        out.fill(0, offset);
      }
      return audio.AudioDataCallbackResult.VALID;
    };

  private interruptCallback: (event: audio.InterruptEvent) => void =
    (event: audio.InterruptEvent): void => {
      const hint: audio.InterruptHint = event.hintType;
      if (hint === audio.InterruptHint.INTERRUPT_HINT_PAUSE ||
        hint === audio.InterruptHint.INTERRUPT_HINT_STOP) {
        hilog.info(DOMAIN, TAG, 'audio interrupted: pause');
        return;
      }
      if (hint === audio.InterruptHint.INTERRUPT_HINT_RESUME) {
        const renderer: audio.AudioRenderer | undefined = this.renderer;
        if (renderer !== undefined) {
          renderer.start().catch((error: BusinessError) => {
            hilog.warn(DOMAIN, TAG, 'resume audio failed: %{public}s',
              JSON.stringify(error));
          });
        }
        hilog.info(DOMAIN, TAG, 'audio interrupted: resume');
      }
    };

  // Detaches the live renderer and clears all buffered state, returning the
  // renderer so the caller can release it (or null if there was none).
  private disposeRenderer(): audio.AudioRenderer | undefined {
    const renderer: audio.AudioRenderer | undefined = this.renderer;
    this.renderer = undefined;
    this.rate = 0;
    this.channels = 0;
    this.chunks = [];
    this.queuedBytes = 0;
    if (renderer !== undefined) {
      try {
        renderer.off('writeData');
        renderer.off('audioInterrupt');
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'detach audio callbacks failed: %{public}s',
          JSON.stringify(error as BusinessError));
      }
    }
    return renderer;
  }

  /** Stops playback and frees the audio stream; idempotent. */
  async release(): Promise<void> {
    this.released = true;
    this.pendingChunks = [];
    const renderer: audio.AudioRenderer | undefined = this.disposeRenderer();
    if (renderer === undefined) {
      return;
    }
    try {
      await renderer.release();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'release audio failed: %{public}s',
        JSON.stringify(error as BusinessError));
    }
  }

  // RDP audio only negotiates the standard PCM rates; higher rates are never
  // sent by the server. The explicit enum keeps playback valid on API 23.
  private static mapRate(rate: number): audio.AudioSamplingRate {
    switch (rate) {
      case 8000:
        return audio.AudioSamplingRate.SAMPLE_RATE_8000;
      case 11025:
        return audio.AudioSamplingRate.SAMPLE_RATE_11025;
      case 12000:
        return audio.AudioSamplingRate.SAMPLE_RATE_12000;
      case 16000:
        return audio.AudioSamplingRate.SAMPLE_RATE_16000;
      case 22050:
        return audio.AudioSamplingRate.SAMPLE_RATE_22050;
      case 24000:
        return audio.AudioSamplingRate.SAMPLE_RATE_24000;
      case 32000:
        return audio.AudioSamplingRate.SAMPLE_RATE_32000;
      case 44100:
        return audio.AudioSamplingRate.SAMPLE_RATE_44100;
      default:
        return audio.AudioSamplingRate.SAMPLE_RATE_48000;
    }
  }
}
