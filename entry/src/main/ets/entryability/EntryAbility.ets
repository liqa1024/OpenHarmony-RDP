/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { UIContext, window } from '@kit.ArkUI';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
import { ConnectionStore } from '../services/ConnectionStore';
import { RdpNative } from '../services/RdpNative';
import { SessionManager } from '../services/SessionManager';
import { WindowController } from '../services/WindowController';
import { showConfirm } from '../utils/UiUtil';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  private mainWindowStage: window.WindowStage | undefined = undefined;
  private allowingTerminate: boolean = false;
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    SettingsStore.init(this.context);
    ConnectionStore.init(this.context);
    // Single-window mode: if this launch restores the main window after a
    // session, finish terminating that session now that we are registered.
    WindowController.onMainWindowReady();
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');
    this.mainWindowStage = windowStage;
    WindowController.init(windowStage);
    const settings: AppSettings = SettingsStore.load();
    // The cursor switch is process-global and must be applied before any
    // session can connect.
    RdpNative.applyRdpCursor(settings.useRdpCursor);
    // Dev capture: configure before any session can connect so the RDPGFX
    // command stream is captured from the first frame. Gated on developer
    // options so a stale stored value cannot start a capture while the tools
    // are hidden.
    RdpNative.applyGfxCapture(settings.developerMode && settings.gfxCapture, this.context.filesDir);
    WindowController.applyMainWindowSize(settings.mainWindowWidth, settings.mainWindowHeight,
      settings.mainWindowSizeEnabled);

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
    });
  }

  onPrepareToTerminate(): boolean {
    if (this.allowingTerminate || SettingsStore.load().autoHideMainWindow) {
      return false;
    }
    if (!SessionManager.hasSessionWindow()) {
      return false;
    }
    const stage: window.WindowStage | undefined = this.mainWindowStage;
    if (stage === undefined) {
      return false;
    }
    let mainWindow: window.Window | undefined = undefined;
    try {
      mainWindow = stage.getMainWindowSync();
    } catch (error) {
      hilog.error(DOMAIN, 'testTag', 'get main window failed: %{public}s',
        JSON.stringify(error));
      return false;
    }
    if (mainWindow === undefined) {
      return false;
    }
    if (WindowController.isMinimized(mainWindow)) {
      mainWindow.restore().then(() => {
        this.promptCloseMainWindow();
      }).catch((error: BusinessError) => {
        hilog.error(DOMAIN, 'testTag', 'restore main window failed: %{public}s',
          JSON.stringify(error));
        this.promptCloseMainWindow();
      });
    } else {
      this.promptCloseMainWindow();
    }
    return true;
  }

  private promptCloseMainWindow(): void {
    const stage: window.WindowStage | undefined = this.mainWindowStage;
    if (stage === undefined) {
      return;
    }
    let ui: UIContext | undefined = undefined;
    try {
      ui = stage.getMainWindowSync().getUIContext();
    } catch (error) {
      hilog.error(DOMAIN, 'testTag', 'get main window UIContext failed: %{public}s',
        JSON.stringify(error));
      return;
    }
    if (ui === undefined) {
      return;
    }
    showConfirm(ui, {
      title: '关闭主窗口',
      message: '还有会话正在运行，关闭主窗口将断开并关闭这些会话。',
      confirmLabel: '断开并关闭',
      danger: true,
      onConfirm: (): void => {
        this.allowingTerminate = true;
        SessionManager.closeAllSessions();
        this.context.terminateSelf().catch((error: BusinessError) => {
          hilog.error(DOMAIN, 'testTag', 'terminate main window failed: %{public}s',
            JSON.stringify(error));
        });
      }
    });
  }

  onWindowStageDestroy(): void {
    // The list page's aboutToDisappear() does not run on window close, so drop
    // its state listener here to avoid notifying a destroyed component.
    SessionManager.clearStateListeners();
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
