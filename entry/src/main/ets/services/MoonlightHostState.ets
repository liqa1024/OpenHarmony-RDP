import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { connection, socket } from '@kit.NetworkKit';
import { HostPower, MoonlightConnection, SavedConnection } from '../model/Connections';
import { ConnectionStore } from './ConnectionStore';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { ServerInfoResult } from '../moonlight/MoonlightModels';
import { WakeOnLan, WakeReachability } from './WakeOnLan';

const DOMAIN: number = 0xD005;
const TAG: string = 'HmRdpMlHostState';
const DEFAULT_HTTPS_PORT: number = 47984;
// Grace before a post-action re-probe: /cancel stops the host app asynchronously,
// so a probe fired immediately can still see the old app.
export const HOST_REPROBE_DELAY_MS: number = 1500;
// Ports a running Windows keeps listening: an answer means the OS is up.
const OS_PROBE_PORTS: number[] = [445, 135, 3389];
const OS_PROBE_TIMEOUT_MS: number = 800;
const ICMP_PROBE_SECONDS: number = 1;
const ICMP_MIN_API_VERSION: number = 26;
const SERVICE_PROBE_TIMEOUT_MS: number = 3000;

export type HostTargetListener =
  (connectionId: string, currentGame: number, power: HostPower, probing: boolean) => void;

class HostTarget {
  currentGame: number = 0;
  httpsPort: number = DEFAULT_HTTPS_PORT;
  power: HostPower = HostPower.Unknown;
  // Only meaningful while Offline: whether this client is on the host's LAN.
  wake: WakeReachability = WakeReachability.Unknown;
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

  static wakeReachabilityOf(connectionId: string): WakeReachability {
    const target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    return target === undefined ? WakeReachability.Unknown : target.wake;
  }

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

  // Sets the field without notifying: the caller's state change does the broadcast.
  private static setWake(connectionId: string, reach: WakeReachability): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.wake = reach;
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
    MoonlightHostState.probe(conn, filesDir);
  }

  private static async probe(conn: MoonlightConnection, filesDir: string): Promise<void> {
    const info: Promise<ServerInfoResult> =
      MoonlightClient.fetchServerInfo(filesDir, conn.host, conn.port, SERVICE_PROBE_TIMEOUT_MS);
    info.catch((): void => {
    });
    let osUp: boolean = false;
    try {
      osUp = await MoonlightHostState.osAlive(conn.host);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'os probe error for %{public}s: %{public}s',
        conn.id, JSON.stringify(error));
    }
    if (!osUp) {
      hilog.info(DOMAIN, TAG, 'host %{public}s silent: offline', conn.host);
      // Silence cannot tell a sleeping NIC from a dead one; only the send path can
      // be ruled in/out, so evaluate it here where wake would be offered.
      const reach: WakeReachability = await WakeOnLan.reachability(conn.host);
      MoonlightHostState.setWake(conn.id, reach);
      MoonlightHostState.endProbe(conn.id);
      MoonlightHostState.markUnreachable(conn.id);
      return;
    }
    try {
      const result: ServerInfoResult = await info;
      MoonlightHostState.endProbe(conn.id);
      if (!result.ok) {
        hilog.info(DOMAIN, TAG, 'host %{public}s up, service did not answer: no service',
          conn.host);
        MoonlightHostState.markNoService(conn.id);
        return;
      }
      MoonlightHostState.setProbed(conn.id, result.info.currentGame, result.info.httpsPort);
      MoonlightHostState.captureMac(conn, result.info.mac);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'service probe error for %{public}s: %{public}s',
        conn.id, JSON.stringify(error));
      MoonlightHostState.endProbe(conn.id);
      MoonlightHostState.markNoService(conn.id);
    }
  }

  // True as soon as any signal answers; false only once they all fail together.
  private static osAlive(host: string): Promise<boolean> {
    return new Promise<boolean>((resolve: (value: boolean) => void) => {
      let pending: number = OS_PROBE_PORTS.length + 1;
      const settle = (alive: boolean): void => {
        if (alive) {
          resolve(true);
          return;
        }
        pending -= 1;
        if (pending === 0) {
          resolve(false);
        }
      };
      MoonlightHostState.icmpAlive(host)
        .then((alive: boolean): void => settle(alive))
        .catch((): void => settle(false));
      for (let i = 0; i < OS_PROBE_PORTS.length; i++) {
        MoonlightHostState.portOpen(host, OS_PROBE_PORTS[i])
          .then((open: boolean): void => settle(open))
          .catch((): void => settle(false));
      }
    });
  }

  private static async icmpAlive(host: string): Promise<boolean> {
    // Since 26.0.0, newer than compatibleSdkVersion: same two-step gate as SystemMaterial.
    if (deviceInfo.sdkApiVersion < ICMP_MIN_API_VERSION) {
      return false;
    }
    if (deviceInfo.apiAvailable('26.0.0')) {
      try {
        const result: connection.ProbeResultInfo =
          await connection.queryProbeResult(host, ICMP_PROBE_SECONDS);
        return result.lossRate < 100;
      } catch (error) {
        hilog.debug(DOMAIN, TAG, 'icmp probe %{public}s failed: %{public}s',
          host, JSON.stringify(error));
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
