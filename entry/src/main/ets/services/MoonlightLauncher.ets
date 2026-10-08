/*
 * HmRdp - starts a Moonlight session from a saved connection.
 *
 * Builds the launch request (resolved stream profile + real pixel size, the app
 * to open, the sandbox files dir and the PIN popup provider) and hands it to the
 * manager, which runs the control layer's pre-flight (serverinfo -> pair ->
 * launch) through the backend's prepare() while the connection is still
 * Connecting. Shared by the list, the editor and the app-management page so the
 * request shape exists once.
 */
import { common } from '@kit.AbilityKit';
import { UIContext } from '@kit.ArkUI';
import { ML_RES_FIXED, MoonlightProfile, PendingConnection, PROTOCOL_MOONLIGHT,
  RdpConnectOrigin, SavedConnection } from '../model/RdpModels';
import { PairResult } from '../moonlight/MoonlightModels';
import { SessionManager } from './SessionManager';
import { SettingsStore } from './SettingsStore';
import { defaultDisplay } from '../utils/DisplayUtil';
import { promptPairingPin } from '../utils/PairingPrompt';

export class MoonlightLauncher {
  /**
   * Starts a Moonlight connection. `appId` 0 opens the connection's default app
   * (or the host default); a non-zero id opens that specific app. `ui` owns the
   * PIN popup, `context` supplies the sandbox files dir, and `onConnected` (if
   * given) persists the connection once a session is up.
   */
  static connect(ui: UIContext, context: common.UIAbilityContext, conn: SavedConnection,
    origin: RdpConnectOrigin, appId: number = 0, onConnected?: () => void): void {
    const stream: MoonlightProfile = SettingsStore.resolveStream(conn);
    let width: number = conn.mlWidth;
    let height: number = conn.mlHeight;
    if (stream.resMode !== ML_RES_FIXED || width <= 0 || height <= 0) {
      const size = defaultDisplay();
      if (size !== undefined) {
        width = size.width;
        height = size.height;
      }
    }
    stream.width = width;
    stream.height = height;
    const request: PendingConnection = new PendingConnection();
    request.connectionId = conn.id;
    request.origin = origin;
    request.protocol = PROTOCOL_MOONLIGHT;
    request.savedConnection = conn;
    request.stream = stream;
    request.filesDir = context.filesDir;
    request.moonlightAppId = appId > 0 ? appId : MoonlightLauncher.defaultAppId(conn);
    request.moonlightPinProvider =
      (pin: string, pairing: Promise<PairResult>): Promise<boolean> =>
        promptPairingPin(ui, pin, pairing);
    request.onConnected = onConnected;
    SessionManager.connect(request, context);
  }

  /** The connection's stored default app id (0 = the host default). */
  private static defaultAppId(conn: SavedConnection): number {
    if (conn.mlDefaultAppId.length === 0) {
      return 0;
    }
    const parsed: number = Number.parseInt(conn.mlDefaultAppId, 10);
    return Number.isNaN(parsed) ? 0 : parsed;
  }
}
