import { ConnectAction, connectActionLabel, HostPower, SavedConnection } from './Connections';
import { ConnectionState } from '../services/SessionManager';
import { WakeReachability } from '../services/WakeOnLan';
import { connectActionFor } from '../utils/ConnectAction';

export enum ConnectionRowIcon {
  Loading = 0,
  Refresh = 1,
  Power = 2,
  Stop = 3,
  Play = 4
}

export enum ConnectionRowBadge {
  None = 0,
  Connected = 1,
  Running = 2
}

export enum ConnectionRowIntent {
  None = 0,
  Toggle = 1,
  Terminate = 2,
  Refresh = 3,
  Wake = 4
}

export class ConnectionRowModel {
  action: ConnectAction = ConnectAction.Connect;
  primary: ConnectAction = ConnectAction.Connect;
  icon: number = ConnectionRowIcon.Play;
  badge: number = ConnectionRowBadge.None;
  intent: number = ConnectionRowIntent.None;
  busy: boolean = false;
  hostDown: boolean = false;
  wakeDisabled: boolean = false;
  menuAllowed: boolean = true;
  actionTip: string = '';

  static of(conn: SavedConnection, state: ConnectionState, hostTargetId: number,
    targetId: number, power: HostPower, wakeReachability: WakeReachability,
    waking: boolean, probing: boolean): ConnectionRowModel {
    const model: ConnectionRowModel = new ConnectionRowModel();
    model.action = connectActionFor(state, hostTargetId, targetId);
    model.primary = conn.primaryAction(model.action);
    model.busy = state === ConnectionState.Disconnecting;
    model.menuAllowed = !model.busy;
    model.hostDown = state === ConnectionState.Idle && power !== HostPower.Online;
    model.wakeDisabled = model.hostDown && power === HostPower.Offline
      && (!conn.supportsWake() || wakeReachability === WakeReachability.OffLan);

    if (state === ConnectionState.Connected) {
      model.badge = ConnectionRowBadge.Connected;
    } else if (model.action === ConnectAction.Resume) {
      model.badge = ConnectionRowBadge.Running;
    }

    if (model.busy || state === ConnectionState.Connecting) {
      model.icon = ConnectionRowIcon.Loading;
    } else if (model.hostDown && waking) {
      model.icon = ConnectionRowIcon.Loading;
    } else if (model.hostDown && probing
      && (power === HostPower.Unknown || power === HostPower.NoService)) {
      model.icon = ConnectionRowIcon.Loading;
    } else if (model.hostDown
      && (power === HostPower.Unknown || power === HostPower.NoService)) {
      model.icon = ConnectionRowIcon.Refresh;
    } else if (model.hostDown) {
      model.icon = ConnectionRowIcon.Power;
    } else if (model.primary === ConnectAction.Terminate
      || model.primary === ConnectAction.Disconnect) {
      model.icon = ConnectionRowIcon.Stop;
    } else {
      model.icon = ConnectionRowIcon.Play;
    }

    if (model.hostDown) {
      if (waking) {
        model.actionTip = '取消状态检测';
      } else if (probing && power === HostPower.Unknown) {
        model.actionTip = '正在检测主机状态';
      } else if (power === HostPower.NoService) {
        model.actionTip = '请检查 Sunshine 是否运行';
      } else if (power === HostPower.Unknown) {
        model.actionTip = '刷新主机状态';
      } else if (wakeReachability === WakeReachability.OffLan) {
        model.actionTip = '不在主机所在局域网';
      } else {
        model.actionTip = conn.supportsWake() ? '唤醒主机' : '未获取到主机 MAC';
      }
    } else {
      model.actionTip = connectActionLabel(model.primary);
    }

    if (model.busy) {
      model.intent = ConnectionRowIntent.None;
    } else if (model.hostDown) {
      if (waking) {
        model.intent = ConnectionRowIntent.Wake;
      } else if (power === HostPower.Unknown || power === HostPower.NoService) {
        model.intent = ConnectionRowIntent.Refresh;
      } else if (!model.wakeDisabled) {
        model.intent = ConnectionRowIntent.Wake;
      }
    } else if (model.primary === ConnectAction.Terminate) {
      model.intent = ConnectionRowIntent.Terminate;
    } else if (model.primary !== ConnectAction.Busy) {
      model.intent = ConnectionRowIntent.Toggle;
    }

    return model;
  }
}
