import { common } from '@kit.AbilityKit';
import { UIContext } from '@kit.ArkUI';
import { MoonlightConnection, PendingConnection, PROTOCOL_MOONLIGHT,
  ConnectOrigin } from '../model/Connections';
import { ML_RES_FIXED, MoonlightProfile } from '../moonlight/MoonlightModels';
import { PairResult } from '../moonlight/MoonlightModels';
import { SessionManager } from './SessionManager';
import { SettingsStore } from './SettingsStore';
import { defaultDisplay } from '../utils/DisplayUtil';
import { promptPairingPin } from '../utils/PairingPrompt';

export class MoonlightLauncher {
  static connect(ui: UIContext, context: common.UIAbilityContext, conn: MoonlightConnection,
    origin: ConnectOrigin, appId: number = 0, onConnected?: () => void): void {
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
    request.moonlightAppId = appId > 0 ? appId : conn.mlDefaultAppIdValue();
    request.moonlightPinProvider =
      (pin: string, pairing: Promise<PairResult>): Promise<boolean> =>
        promptPairingPin(ui, pin, pairing);
    request.onConnected = onConnected;
    SessionManager.connect(request, context);
  }
}
