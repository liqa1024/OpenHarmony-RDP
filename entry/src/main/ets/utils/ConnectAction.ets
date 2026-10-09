import { ConnectionState } from '../services/SessionManager';

export enum ConnectAction {
  Connect = 0,
  Resume = 1,
  Switch = 2,
  Terminate = 3,
  Cancel = 4,
  Disconnect = 5,
  Busy = 6
}

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
  if (hostTargetId !== 0 && targetId > 0) {
    return hostTargetId === targetId ? ConnectAction.Resume : ConnectAction.Switch;
  }
  return ConnectAction.Connect;
}

export function primaryConnectAction(action: ConnectAction,
  terminatesHostApp: boolean): ConnectAction {
  if (terminatesHostApp
    && (action === ConnectAction.Resume || action === ConnectAction.Switch
      || action === ConnectAction.Disconnect)) {
    return ConnectAction.Terminate;
  }
  return action;
}

export function connectActionLabel(action: ConnectAction): string {
  if (action === ConnectAction.Cancel) {
    return '取消连接';
  }
  if (action === ConnectAction.Busy) {
    return '正在断开';
  }
  if (action === ConnectAction.Terminate) {
    return '终止';
  }
  if (action === ConnectAction.Disconnect) {
    return '断开连接';
  }
  if (action === ConnectAction.Resume) {
    return '恢复';
  }
  if (action === ConnectAction.Switch) {
    return '切换';
  }
  return '连接';
}
