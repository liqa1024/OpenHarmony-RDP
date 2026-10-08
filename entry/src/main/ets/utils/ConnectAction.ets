/*
 * HmRdp - what a connect affordance will do, protocol-neutral.
 *
 * The connection row and the app tile both need to answer "what will a click
 * do?", so the 连接 / 恢复 / 切换 / 终止 / 断开 decision lives here instead of in
 * each view.
 *
 * The local session state is shared by every protocol. 恢复 (Resume) only exists
 * where the host can keep a session alive after the local one ends: Moonlight
 * (Sunshine) keeps the running app, so reconnecting resumes it (`/resume`).
 * **RDP has no such state** — closing its session disconnects completely, so RDP
 * passes `hostTargetId = 0` and never reaches Resume. That is by design, not a
 * gap in this helper.
 */
import { ConnectionState } from '../services/SessionManager';

export enum ConnectAction {
  // Start the target (RDP: connect; Moonlight: launch the app).
  Connect = 0,
  // The host is already running the target: resume it (Moonlight `/resume`).
  Resume = 1,
  // The host is running a different target: it is stopped, then the target starts.
  Switch = 2,
  // Close the session and stop the host's running app (Moonlight).
  Terminate = 3,
  // A connect attempt is in flight.
  Cancel = 4,
  // Close the local session only (RDP; the host keeps nothing).
  Disconnect = 5,
  // Tearing down; attempts are refused.
  Busy = 6
}

/**
 * `hostTargetId` identifies what the host currently has open (0 = none). Only a
 * protocol whose host keeps something running passes it (Moonlight: the running
 * app id); RDP passes 0.
 * `targetId` identifies what the affordance would open; 0 means "the connection's
 * own default", which this helper cannot resolve, so the action stays neutral.
 */
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

/**
 * The connection row's primary button. A protocol whose session cannot outlive
 * the app on the host (Moonlight: closing the session stops the app too)
 * collapses its resume / switch / disconnect states into one 终止 action, so the
 * button never shows 恢复 — resume lives in the context menu. A protocol without
 * a host-side session (RDP) keeps 断开连接.
 */
export function primaryConnectAction(action: ConnectAction,
  terminatesHostApp: boolean): ConnectAction {
  if (terminatesHostApp
    && (action === ConnectAction.Resume || action === ConnectAction.Switch
      || action === ConnectAction.Disconnect)) {
    return ConnectAction.Terminate;
  }
  return action;
}

/** The connect affordance's hover tip / menu label for an action. */
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
