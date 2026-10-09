import { common } from '@kit.AbilityKit';
import { UIContext } from '@kit.ArkUI';
import { ML_RES_FIXED, MoonlightProfile, PendingConnection, PROTOCOL_MOONLIGHT,
  RdpConnectOrigin, SavedConnection } from '../model/RdpModels';
import { PairResult } from '../moonlight/MoonlightModels';
import { MoonlightHostState } from './MoonlightHostState';
import { SessionManager } from './SessionManager';
import { SettingsStore } from './SettingsStore';
import { defaultDisplay } from '../utils/DisplayUtil';
import { promptPairingPin } from '../utils/PairingPrompt';

export class MoonlightLauncher {
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
    // Mark the known target so the connecting badge shows immediately; the
    // backend corrects it once the pre-flight picks the app.
    MoonlightHostState.markLaunched(conn.id, request.moonlightAppId);
    SessionManager.connect(request, context);
  }

  private static defaultAppId(conn: SavedConnection): number {
    if (conn.mlDefaultAppId.length === 0) {
      return 0;
    }
    const parsed: number = Number.parseInt(conn.mlDefaultAppId, 10);
    return Number.isNaN(parsed) ? 0 : parsed;
  }
}
