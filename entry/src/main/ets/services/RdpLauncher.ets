import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AdvancedProfile, DisplayProfile, RdpConnectOptions } from '../model/RdpModels';
import { ConnectOrigin, PendingConnection, RdpConnection } from '../model/Connections';
import { CredentialStore } from './CredentialStore';
import { DeviceCapabilities } from './DeviceCapabilities';
import { SessionManager } from './SessionManager';
import { SettingsStore } from './SettingsStore';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpRdpLauncher';

export class RdpLauncher {
  static connect(context: common.UIAbilityContext, conn: RdpConnection,
    origin: ConnectOrigin): void {
    const options: RdpConnectOptions = conn.toOptions();
    const request: PendingConnection = new PendingConnection();
    request.connectionId = conn.id;
    request.origin = origin;
    request.onConnected = undefined;
    request.protocol = conn.protocol;
    request.savedConnection = conn;
    const profile: DisplayProfile = SettingsStore.resolveDisplay(conn);
    options.width = profile.width;
    options.height = profile.height;
    options.streamWidth = profile.sessionWidth;
    options.streamHeight = profile.sessionHeight;
    options.scalePercent = profile.scalePercent;
    options.srEnabled = profile.srEnabled;
    options.srRatioPercent = profile.srRatioPercent;
    options.srBackend = profile.srBackend;
    const advanced: AdvancedProfile = SettingsStore.resolveAdvanced(conn);
    options.enableAudio = advanced.enableAudio && DeviceCapabilities.audio().supported;
    options.ignoreCertificate = advanced.ignoreCertificate;
    CredentialStore.loadPassword(conn.id).then((password: string): void => {
      options.password = password;
      request.options = options;
      SessionManager.connect(request, context);
    }).catch((error: BusinessError): void => {
      hilog.error(DOMAIN, TAG, 'load password failed: %{public}s', JSON.stringify(error));
    });
  }
}
