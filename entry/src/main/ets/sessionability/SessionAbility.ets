/*
 * HmRdp - UIAbility hosting an independent RDP session window.
 *
 * Launched via startAbility with a unique instanceKey so every connection gets
 * its own main window (standard title bar, movable / resizable). The launch
 * request is identified by `sessionKey`, which lets several sessions coexist
 * and lets each window tear down only its own native session.
 */
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { SessionRequests } from '../model/RdpModels';
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
    // Final safety net: if the window is closed before the page lifecycle runs,
    // tear down the native session here so the RDP connection never outlives
    // its window. stopByKey() only affects this window's session.
    this.releaseSession();
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onDestroy');
  }

  /**
   * Mirrors the toolbar "断开" action: stop this window's FreeRDP session, drop
   * its event listener and scrub the transient credentials held by its request.
   */
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
    const settings: AppSettings = SettingsStore.load();
    if (!settings.sessionFullscreen) {
      WindowController.applyWindowStageSize(windowStage,
        settings.sessionWindowWidth, settings.sessionWindowHeight,
        settings.sessionWindowSizeEnabled);
    }
    // Single-window mode: block the system close button so the main window can
    // be relaunched first (see WindowController.closeSession).
    windowStage.on('windowStageClose', () => {
      return WindowController.closeSession(this.context);
    });
    const storage: LocalStorage = new LocalStorage();
    storage.setOrCreate<string>('sessionKey', this.sessionKey);
    windowStage.loadContent('pages/SessionPage', storage, (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'Failed to load the content. Cause: %{public}s',
          JSON.stringify(err));
        return;
      }
      WindowController.setupSessionWindow(windowStage, settings.sessionFullscreen);
      WindowController.onSessionWindowReady();
      hilog.info(DOMAIN, TAG, '%{public}s', 'Succeeded in loading the content.');
    });
  }

  /**
   * Intercepts dock/tray close as well as the title-bar close so the main window
   * is relaunched before this session ability goes away in single-window mode.
   */
  onPrepareToTerminate(): boolean {
    return WindowController.closeSession(this.context);
  }

  onWindowStageDestroy(): void {
    // Closing the session window (title-bar X / Alt+F4) must disconnect the RDP
    // session exactly like the toolbar button. This runs even when the page
    // never receives aboutToDisappear().
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
