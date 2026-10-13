import { common } from '@kit.AbilityKit';
import { UIContext } from '@kit.ArkUI';
import { ConnectOrigin, MoonlightConnection, RdpConnection, SavedConnection }
  from '../model/Connections';
import { MoonlightConnector } from './MoonlightConnector';
import { MoonlightLauncher } from './MoonlightLauncher';
import { RdpLauncher } from './RdpLauncher';

export class ConnectionLauncher {
  static start(ui: UIContext, context: common.UIAbilityContext, conn: SavedConnection,
    origin: ConnectOrigin, appId: number = 0, onConnected?: () => void): void {
    if (conn instanceof MoonlightConnection) {
      MoonlightLauncher.connect(ui, context, conn, origin, appId, onConnected);
      return;
    }
    if (conn instanceof RdpConnection) {
      RdpLauncher.connect(context, conn, origin);
    }
  }

  static afterDelete(conn: SavedConnection, filesDir: string): void {
    if (conn instanceof MoonlightConnection) {
      MoonlightConnector.forgetHost(filesDir, conn.host, conn.port);
    }
  }
}
