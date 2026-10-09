export enum SessionSignalKind {
  Connected = 0,
  Disconnected = 1,
  Error = 2,
  UiEvent = 3
}

export class SessionSignal {
  kind: SessionSignalKind = SessionSignalKind.UiEvent;
  eventCode: number = -1;
  message: string = '';
  code: number = 0;
  payload: Object | undefined = undefined;
}
