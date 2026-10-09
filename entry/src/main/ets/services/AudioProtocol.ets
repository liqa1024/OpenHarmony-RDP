export class AudioOps {
  // The renderer is built once the format is known (FORMAT may arrive before or after START).
  static readonly START: string = 'start';
  static readonly STOP: string = 'stop';
  static readonly FORMAT: string = 'format';
}

export interface AudioMessage {
  op: string;
  handle: number;
  rate?: number;
  channels?: number;
}
