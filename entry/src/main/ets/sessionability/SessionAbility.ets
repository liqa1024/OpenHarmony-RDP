import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { SessionRequests, PendingConnection, PROTOCOL_MOONLIGHT } from '../model/RdpModels';
import { SessionManager } from '../services/SessionManager';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
import { WindowController } from '../services/WindowController';

const DOMAIN: number = 0x0000;
const TAG: string = 'HmRdpSessionAbility';

export default class SessionAbility extends UIAbility {
  private sessionKey: string = '';

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    SettingsStore.init(this.context);
    this.sessionKey = SessionAbility.readSessionKey(want);
    hilog.info(DOMAIN, TAG, '%{public}s key=%{public}s', 'SessionAbility onCreate',
      this.sessionKey);
  }

  onDestroy(): void {
    // Final safety net: the RDP connection must never outlive its window, even
    // if it is closed before the page lifecycle runs.
    this.releaseSession();
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onDestroy');
  }

  private releaseSession(): void {
    if (this.sessionKey.length === 0) {
      return;
    }
    SessionManager.release(this.sessionKey);
    SessionRequests.discard(this.sessionKey);
  }

  private static readSessionKey(want: Want): string {
    const params: Record<string, Object> | undefined = want.parameters;
    if (params === undefined) {
      return '';
    }
    const value: Object = params['sessionKey'];
    if (typeof value === 'string') {
      return value;
    }
    return '';
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onWindowStageCreate');
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    const moonlight: boolean = request !== undefined && request.protocol === PROTOCOL_MOONLIGHT;
    const settings: AppSettings = SettingsStore.load();
    // Both protocols share the same app-level session window surface.
    const fullscreen: boolean = settings.sessionFullscreen;
    const sizeEnabled: boolean = settings.sessionWindowSizeEnabled;
    const width: number = settings.sessionWindowWidth;
    const height: number = settings.sessionWindowHeight;
    if (!fullscreen) {
      WindowController.applyWindowStageSize(windowStage, width, height, sizeEnabled);
    }
    // Single-window mode: block the system close button so the main window can
    // be relaunched first (see WindowController.closeSession).
    try {
      windowStage.on('windowStageClose', () => {
        return WindowController.closeSession(this.context);
      });
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'watch window close failed: %{public}s', JSON.stringify(error));
    }
    const storage: LocalStorage = new LocalStorage();
    storage.setOrCreate<string>('sessionKey', this.sessionKey);
    const page: string = moonlight ? 'pages/MoonlightSessionPage' : 'pages/SessionPage';
    windowStage.loadContent(page, storage, (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'Failed to load the content. Cause: %{public}s',
          JSON.stringify(err));
        return;
      }
      WindowController.setupSessionWindow(windowStage, fullscreen);
      WindowController.onSessionWindowReady();
      hilog.info(DOMAIN, TAG, '%{public}s', 'Succeeded in loading the content.');
    });
  }

  // Intercepts dock/tray close as well as the title-bar close so the main
  // window is relaunched before this ability goes away in single-window mode.
  onPrepareToTerminate(): boolean {
    return WindowController.closeSession(this.context);
  }

  onWindowStageDestroy(): void {
    // Runs even when the page never receives aboutToDisappear().
    this.releaseSession();
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onBackground');
  }
}
