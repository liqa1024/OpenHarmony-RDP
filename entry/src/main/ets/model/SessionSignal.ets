/*
 * HmRdp - protocol-neutral session signal.
 *
 * The session lifecycle (connect / first frame / error / disconnect) is the same
 * shape for every protocol, so SessionManager routes it without knowing which
 * native client produced it. The protocol-specific UI events (cursor, clipboard,
 * telemetry, ...) ride on the same envelope with the protocol's own event code:
 * the session window is protocol-aware and interprets that code itself, while the
 * manager only ever looks at `kind`.
 */
export enum SessionSignalKind {
  Connected = 0,
  Disconnected = 1,
  Error = 2,
  // A UI-only event; `eventCode` is the protocol's own event id and `payload`
  // the protocol's payload object.
  UiEvent = 3
}

export class SessionSignal {
  kind: SessionSignalKind = SessionSignalKind.UiEvent;
  // Protocol-specific event id (RdpEvent for RDP). -1 for the lifecycle kinds.
  eventCode: number = -1;
  // Human-readable reason for Error / Disconnected.
  message: string = '';
  // Native error code for Error / Disconnected (0 when none).
  code: number = 0;
  // The protocol's own payload object (RdpEventPayload for RDP).
  payload: Object | undefined = undefined;
}
