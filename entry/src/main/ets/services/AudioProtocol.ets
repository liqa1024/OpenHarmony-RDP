export class AudioOps {
  // The renderer is built once the format is known (FORMAT may arrive before or after START).
  static readonly START: string = 'start';
  static readonly STOP: string = 'stop';
  static readonly FORMAT: string = 'format';
}

// Which protocol's native ring the worker pulls from. RDP is keyed by the session
// handle; Moonlight is a single process-global ring, so it uses no handle.
export class AudioSource {
  static readonly RDP: number = 0;
  static readonly MOONLIGHT: number = 1;
}

export interface AudioMessage {
  op: string;
  source?: number;
  handle: number;
  rate?: number;
  channels?: number;
}
