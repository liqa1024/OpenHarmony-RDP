/*
 * HmRdp - UIAbility hosting an independent RDP session window.
 *
 * Launched via startAbility with a unique instanceKey so every connection gets
 * its own main window (standard title bar, movable / resizable).
 */
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { PendingConnection } from '../model/RdpModels';
import { RdpNative } from '../services/RdpNative';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
import { WindowController } from '../services/WindowController';

const DOMAIN: number = 0x0000;
const TAG: string = 'HmRdpSessionAbility';

export default class SessionAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    SettingsStore.init(this.context);
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onCreate');
  }

  onDestroy(): void {
    // Final safety net: if the window is closed before the page lifecycle runs,
    // tear down the native session here so the RDP connection never outlives
    // its window. stopSession() is idempotent.
    this.releaseSession();
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onDestroy');
  }

  /**
   * Mirrors the toolbar "断开" action: stop the FreeRDP session, drop the event
   * listener and scrub the transient credentials held by PendingConnection.
   */
  private releaseSession(): void {
    RdpNative.get().stopSession();
    RdpNative.get().clearListener();
    PendingConnection.current.options.password = '';
    PendingConnection.current.options.gatewayPassword = '';
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onWindowStageCreate');
    const settings: AppSettings = SettingsStore.load();
    if (!settings.sessionFullscreen) {
      WindowController.applyWindowStageSize(windowStage,
        settings.sessionWindowWidth, settings.sessionWindowHeight,
        settings.sessionWindowSizeEnabled);
    }
    windowStage.loadContent('pages/SessionPage', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'Failed to load the content. Cause: %{public}s',
          JSON.stringify(err));
        return;
      }
      WindowController.setupSessionWindow(windowStage, settings.sessionFullscreen);
      hilog.info(DOMAIN, TAG, '%{public}s', 'Succeeded in loading the content.');
    });
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
