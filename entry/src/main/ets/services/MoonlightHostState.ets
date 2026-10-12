import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { socket } from '@kit.NetworkKit';
import { HostPower, MoonlightConnection, SavedConnection } from '../model/Connections';
import { ConnectionStore } from './ConnectionStore';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { ServerInfoResult } from '../moonlight/MoonlightModels';

const DOMAIN: number = 0xD005;
const TAG: string = 'HmRdpMlHostState';
const DEFAULT_HTTPS_PORT: number = 47984;
// Grace before a post-action re-probe: /cancel stops the host app asynchronously,
// so a probe fired immediately can still see the old app.
export const HOST_REPROBE_DELAY_MS: number = 1500;
// A running Windows keeps these listening, so a hit means "host on, streaming service
// down" and rules wake out. Only consulted after the service probe fails ambiguously.
const OS_PROBE_PORTS: number[] = [445, 3389];
const OS_PROBE_TIMEOUT_MS: number = 1200;

export type HostTargetListener =
  (connectionId: string, currentGame: number, power: HostPower, probing: boolean) => void;

class HostTarget {
  currentGame: number = 0;
  httpsPort: number = DEFAULT_HTTPS_PORT;
  // Unknown until a probe returns; Offline reports no running app (row offers wake).
  power: HostPower = HostPower.Unknown;
}

export class MoonlightHostState {
  private static targets: Map<string, HostTarget> = new Map<string, HostTarget>();
  private static listeners: HostTargetListener[] = [];
  private static probing: Map<string, boolean> = new Map<string, boolean>();

  static subscribe(listener: HostTargetListener): void {
    MoonlightHostState.listeners.push(listener);
  }

  static unsubscribe(listener: HostTargetListener): void {
    const index: number = MoonlightHostState.listeners.indexOf(listener);
    if (index >= 0) {
      MoonlightHostState.listeners.splice(index, 1);
    }
  }

  static targetOf(connectionId: string): number {
    const target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    return target === undefined || target.power !== HostPower.Online ? 0 : target.currentGame;
  }

  static reachableOf(connectionId: string): boolean {
    return MoonlightHostState.powerOf(connectionId) === HostPower.Online;
  }

  static powerOf(connectionId: string): HostPower {
    const target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    return target === undefined ? HostPower.Unknown : target.power;
  }

  static httpsPortOf(connectionId: string): number {
    const target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    return target === undefined ? DEFAULT_HTTPS_PORT : target.httpsPort;
  }

  static probingOf(connectionId: string): boolean {
    return MoonlightHostState.probing.get(connectionId) === true;
  }

  // Only after the host really started / resumed the app (no optimistic write).
  static markLaunched(connectionId: string, appId: number): void {
    if (appId <= 0) {
      return;
    }
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.currentGame = appId;
    target.power = HostPower.Online;
    MoonlightHostState.notify(connectionId);
  }

  static markStopped(connectionId: string): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.currentGame = 0;
    MoonlightHostState.notify(connectionId);
  }

  static setProbed(connectionId: string, currentGame: number, httpsPort: number): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.currentGame = currentGame;
    if (httpsPort > 0) {
      target.httpsPort = httpsPort;
    }
    target.power = HostPower.Online;
    MoonlightHostState.notify(connectionId);
  }

  private static markUnreachable(connectionId: string): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.power = HostPower.Offline;
    MoonlightHostState.notify(connectionId);
  }

  private static markNoService(connectionId: string): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.power = HostPower.NoService;
    MoonlightHostState.notify(connectionId);
  }

  static refresh(conn: SavedConnection, filesDir: string): void {
    if (!(conn instanceof MoonlightConnection) || filesDir.length === 0) {
      return;
    }
    if (MoonlightHostState.probing.get(conn.id) === true) {
      return;
    }
    MoonlightHostState.probing.set(conn.id, true);
    MoonlightHostState.notify(conn.id);
    MoonlightClient.fetchServerInfo(filesDir, conn.host, conn.port)
      .then((result: ServerInfoResult): void => {
        if (!result.ok) {
          hilog.warn(DOMAIN, TAG, 'host target probe failed for %{public}s', conn.id);
          MoonlightHostState.settleUnreachable(conn, result.error);
          return;
        }
        MoonlightHostState.endProbe(conn.id);
        MoonlightHostState.setProbed(conn.id, result.info.currentGame, result.info.httpsPort);
        MoonlightHostState.captureMac(conn, result.info.mac);
      })
      .catch((error: BusinessError): void => {
        hilog.warn(DOMAIN, TAG, 'host target probe error: %{public}s', JSON.stringify(error));
        MoonlightHostState.settleUnreachable(conn, 'request');
      });
  }

  // A silent host (timeout) is the powered-off / absent case and stays ambiguous
  // otherwise, so the OS probe decides: an answer means the service alone is down.
  private static settleUnreachable(conn: MoonlightConnection, error: string): void {
    if (error === 'timeout') {
      MoonlightHostState.endProbe(conn.id);
      MoonlightHostState.markUnreachable(conn.id);
      return;
    }
    MoonlightHostState.osAlive(conn.host)
      .then((alive: boolean): void => {
        MoonlightHostState.endProbe(conn.id);
        if (alive) {
          MoonlightHostState.markNoService(conn.id);
        } else {
          MoonlightHostState.markUnreachable(conn.id);
        }
      })
      .catch((): void => {
        MoonlightHostState.endProbe(conn.id);
        MoonlightHostState.markUnreachable(conn.id);
      });
  }

  private static async osAlive(host: string): Promise<boolean> {
    for (let i = 0; i < OS_PROBE_PORTS.length; i++) {
      if (await MoonlightHostState.portOpen(host, OS_PROBE_PORTS[i])) {
        return true;
      }
    }
    return false;
  }

  private static async portOpen(host: string, port: number): Promise<boolean> {
    const tcp: socket.TCPSocket = socket.constructTCPSocketInstance();
    try {
      const bindAddress: socket.NetAddress = { address: '0.0.0.0', port: 0 };
      await tcp.bind(bindAddress);
      const options: socket.TCPConnectOptions = {
        address: { address: host, port: port },
        timeout: OS_PROBE_TIMEOUT_MS
      };
      await tcp.connect(options);
      return true;
    } catch (error) {
      hilog.debug(DOMAIN, TAG, 'os probe %{public}s:%{public}d closed: %{public}s',
        host, port, JSON.stringify(error));
      return false;
    } finally {
      tcp.close().catch((error: BusinessError): void => {
        hilog.debug(DOMAIN, TAG, 'os probe close: %{public}s', JSON.stringify(error));
      });
    }
  }

  private static endProbe(connectionId: string): void {
    MoonlightHostState.probing.set(connectionId, false);
  }

  static refreshAll(connections: SavedConnection[], filesDir: string): void {
    for (const conn of connections) {
      MoonlightHostState.refresh(conn, filesDir);
    }
  }

  static refreshAfter(conn: SavedConnection, filesDir: string, delayMs: number): void {
    setTimeout((): void => {
      MoonlightHostState.refresh(conn, filesDir);
    }, delayMs);
  }

  // The MAC only comes from serverinfo, so persist it here on probe success.
  private static captureMac(conn: SavedConnection, mac: string): void {
    if (!(conn instanceof MoonlightConnection) || mac.length === 0 || conn.mlMac === mac) {
      return;
    }
    conn.mlMac = mac;
    conn.updatedAt = Date.now();
    ConnectionStore.save(conn);
  }

  private static ensure(connectionId: string): HostTarget {
    let target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    if (target === undefined) {
      target = new HostTarget();
      MoonlightHostState.targets.set(connectionId, target);
    }
    return target;
  }

  private static notify(connectionId: string): void {
    const currentGame: number = MoonlightHostState.targetOf(connectionId);
    const power: HostPower = MoonlightHostState.powerOf(connectionId);
    const probing: boolean = MoonlightHostState.probingOf(connectionId);
    const listeners: HostTargetListener[] = MoonlightHostState.listeners.slice();
    for (let i = 0; i < listeners.length; i++) {
      listeners[i](connectionId, currentGame, power, probing);
    }
  }
}
