import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { SavedConnection } from '../model/RdpModels';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { ServerInfoResult } from '../moonlight/MoonlightModels';

const DOMAIN: number = 0xD005;
const TAG: string = 'HmRdpMlHostState';
const DEFAULT_HTTPS_PORT: number = 47984;
// Grace before a post-action re-probe: /cancel stops the host app asynchronously,
// so a probe fired immediately can still see the old app.
export const HOST_REPROBE_DELAY_MS: number = 1500;

export type HostTargetListener = (connectionId: string, currentGame: number) => void;

class HostTarget {
  currentGame: number = 0;
  httpsPort: number = DEFAULT_HTTPS_PORT;
  probed: boolean = false;
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
    return target === undefined ? 0 : target.currentGame;
  }

  static httpsPortOf(connectionId: string): number {
    const target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    return target === undefined ? DEFAULT_HTTPS_PORT : target.httpsPort;
  }

  static markLaunched(connectionId: string, appId: number): void {
    if (appId <= 0) {
      return;
    }
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.currentGame = appId;
    MoonlightHostState.notify(connectionId, target.currentGame);
  }

  static markStopped(connectionId: string): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.currentGame = 0;
    MoonlightHostState.notify(connectionId, target.currentGame);
  }

  static setProbed(connectionId: string, currentGame: number, httpsPort: number): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.probed = true;
    target.currentGame = currentGame;
    if (httpsPort > 0) {
      target.httpsPort = httpsPort;
    }
    MoonlightHostState.notify(connectionId, target.currentGame);
  }

  static refresh(conn: SavedConnection, filesDir: string): void {
    if (!conn.isMoonlight() || filesDir.length === 0) {
      return;
    }
    if (MoonlightHostState.probing.get(conn.id) === true) {
      return;
    }
    MoonlightHostState.probing.set(conn.id, true);
    MoonlightClient.fetchServerInfo(filesDir, conn.host, conn.port)
      .then((result: ServerInfoResult): void => {
        MoonlightHostState.probing.set(conn.id, false);
        if (!result.ok) {
          hilog.warn(DOMAIN, TAG, 'host target probe failed for %{public}s', conn.id);
          return;
        }
        MoonlightHostState.setProbed(conn.id, result.info.currentGame, result.info.httpsPort);
      })
      .catch((error: BusinessError): void => {
        MoonlightHostState.probing.set(conn.id, false);
        hilog.warn(DOMAIN, TAG, 'host target probe error: %{public}s', JSON.stringify(error));
      });
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

  private static ensure(connectionId: string): HostTarget {
    let target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    if (target === undefined) {
      target = new HostTarget();
      MoonlightHostState.targets.set(connectionId, target);
    }
    return target;
  }

  private static notify(connectionId: string, currentGame: number): void {
    const listeners: HostTargetListener[] = MoonlightHostState.listeners.slice();
    for (let i = 0; i < listeners.length; i++) {
      listeners[i](connectionId, currentGame);
    }
  }
}
