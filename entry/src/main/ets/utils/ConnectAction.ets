import { ConnectAction } from '../model/Connections';
import { ConnectionState } from '../services/SessionManager';

export function connectActionFor(state: ConnectionState, hostTargetId: number,
  targetId: number): ConnectAction {
  if (state === ConnectionState.Connecting) {
    return ConnectAction.Cancel;
  }
  if (state === ConnectionState.Connected) {
    return ConnectAction.Disconnect;
  }
  if (state === ConnectionState.Disconnecting) {
    return ConnectAction.Busy;
  }
  // targetId (0 = no default app) only separates resume from switch.
  if (hostTargetId !== 0) {
    return targetId > 0 && hostTargetId !== targetId
      ? ConnectAction.Switch : ConnectAction.Resume;
  }
  return ConnectAction.Connect;
}
