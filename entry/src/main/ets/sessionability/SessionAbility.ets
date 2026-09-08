/*
 * HmRdp - UIAbility hosting an independent RDP session window.
 *
 * Launched via startAbility with a unique instanceKey so every connection gets
 * its own main window (standard title bar, movable / resizable).
 */
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
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
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onWindowStageCreate');
    const settings: AppSettings = SettingsStore.load();
    WindowController.applyWindowStageSize(windowStage,
      settings.sessionWindowWidth, settings.sessionWindowHeight,
      settings.sessionWindowSizeEnabled);
    windowStage.loadContent('pages/SessionPage', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'Failed to load the content. Cause: %{public}s',
          JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, '%{public}s', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'SessionAbility onBackground');
  }
}
