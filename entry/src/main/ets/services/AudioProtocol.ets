/*
 * HmRdp - message shapes for the audio worker (workers/audio.worker.ets).
 *
 * The audio renderer gets its own worker, separate from the background one: a
 * writeData callback must not queue behind a large image decode or a file write,
 * and a stalled callback is audible (doc_agent/architecture.md §4.4). The UI
 * thread only sends control (start/stop) and the format events native raises;
 * the PCM itself stays native and is pulled by session handle.
 */

/** Requested operations (UI -> audio worker). */
export class AudioOps {
  // Begin servicing a session's audio. The renderer is built once the format is
  // known (a FORMAT that arrives first, or one that arrives later).
  static readonly START: string = 'start';
  // Release the renderer of a session (disconnect / window close).
  static readonly STOP: string = 'stop';
  // The session's PCM format (rate/channels): build or rebuild the renderer.
  static readonly FORMAT: string = 'format';
}

/** One control message. `rate`/`channels` are only set on FORMAT. */
export interface AudioMessage {
  op: string;
  handle: number;
  rate?: number;
  channels?: number;
}
