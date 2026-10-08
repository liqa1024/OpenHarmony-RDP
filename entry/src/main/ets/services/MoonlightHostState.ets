/*
 * HmRdp - Moonlight host-side target state (single authority).
 *
 * The one place that answers "which app is the host running, and on which HTTPS
 * port". The connection list (row 恢复 / 终止) and the app page (运行中 badge and
 * the running app's actions) both read from here, so the two surfaces can never
 * disagree about the host's state. The local session state stays in
 * SessionManager; this only models the host side.
 *
 * Value sources, in priority order: an action this client performed
 * (markLaunched / markStopped) is applied immediately, because the client knows
 * its own outcome; a host probe (refresh) reconciles it afterwards. A failed
 * probe keeps the last known value, so a transient network error cannot flip the
 * UI back to a wrong state.
 */
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

/** A host target changed for one connection (currentGame 0 = nothing running). */
export type HostTargetListener = (connectionId: string, currentGame: number) => void;

class HostTarget {
  currentGame: number = 0;
  httpsPort: number = DEFAULT_HTTPS_PORT;
  // True once a host probe succeeded for this connection (a failure keeps the
  // last known value instead of clearing it).
  probed: boolean = false;
}

export class MoonlightHostState {
  private static targets: Map<string, HostTarget> = new Map<string, HostTarget>();
  private static listeners: HostTargetListener[] = [];
  // One in-flight probe per connection; a page show while a probe is running is
  // coalesced instead of firing a second serverinfo request.
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

  /** The host's running app id (0 = nothing / not yet known). */
  static targetOf(connectionId: string): number {
    const target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    return target === undefined ? 0 : target.currentGame;
  }

  /** The host's HTTPS control port (fallback 47984 before any probe). */
  static httpsPortOf(connectionId: string): number {
    const target: HostTarget | undefined = MoonlightHostState.targets.get(connectionId);
    return target === undefined ? DEFAULT_HTTPS_PORT : target.httpsPort;
  }

  /**
   * Records the app this client is opening: a connect launches it (and a switch
   * stops the previous one first), so the running badge can appear before the
   * next host probe. `appId` 0 means "host default, still unknown" and is ignored.
   */
  static markLaunched(connectionId: string, appId: number): void {
    if (appId <= 0) {
      return;
    }
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.currentGame = appId;
    MoonlightHostState.notify(connectionId, target.currentGame);
  }

  /** Records that this client stopped the host app (a successful `/cancel`). */
  static markStopped(connectionId: string): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.currentGame = 0;
    MoonlightHostState.notify(connectionId, target.currentGame);
  }

  /** Feed an authoritative probe result (the app page's full app-list probe). */
  static setProbed(connectionId: string, currentGame: number, httpsPort: number): void {
    const target: HostTarget = MoonlightHostState.ensure(connectionId);
    target.probed = true;
    target.currentGame = currentGame;
    if (httpsPort > 0) {
      target.httpsPort = httpsPort;
    }
    MoonlightHostState.notify(connectionId, target.currentGame);
  }

  /** One cheap host probe (serverinfo only, no pairing / app list). */
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

  /** Probes every Moonlight connection in the list (best effort). */
  static refreshAll(connections: SavedConnection[], filesDir: string): void {
    for (const conn of connections) {
      MoonlightHostState.refresh(conn, filesDir);
    }
  }

  /**
   * Re-probes once after `delayMs`, so a host-side change (a `/cancel` taking
   * effect) has settled. The immediate markStopped already made the UI correct;
   * this only reconciles if the host did something else.
   */
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
