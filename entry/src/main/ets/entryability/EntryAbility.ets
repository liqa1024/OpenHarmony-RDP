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
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
import { ConnectionStore } from '../services/ConnectionStore';
import { RdpNative } from '../services/RdpNative';
import { SessionManager } from '../services/SessionManager';
import { WindowController } from '../services/WindowController';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
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
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');
    WindowController.init(windowStage);
    const settings: AppSettings = SettingsStore.load();
    // Apply the process-global cursor switch before any session can connect
    // (sessions are opened from the list page, not from this ability).
    RdpNative.applyRdpCursor(settings.useRdpCursor);
    // Dev capture: configure before any session can connect so the RDPGFX
    // command stream (ResetGraphics/CreateSurface included) is captured from the
    // first frame. Re-applying the same value later is a native no-op.
    RdpNative.applyRfxDump(settings.rfxDump, this.context.filesDir);
    // Dev probe: log the device GLES compute capability at startup.
    RdpNative.describeGpuCompute();
    WindowController.applyMainWindowSize(settings.mainWindowWidth, settings.mainWindowHeight,
      settings.mainWindowSizeEnabled);

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
      // TEMP dev self-test: runs the GPU RFX decoder over a captured stream and
      // compares it with the captured FreeRDP surface. No-op when the sample
      // files are absent (the native side returns immediately).
      const sampleDir: string = this.context.filesDir;
      // TEMP dev self-test (GPU tile decoder): disabled while B1 is under test,
      // it is slow and blocks the desktop self-test below. Both are dev-only.
      // RdpNative.runGpuSelfTest(`${sampleDir}/hmrdp_rfx.bin`,
      //   `${sampleDir}/hmrdp_rfx_surface.bin`);
      // TEMP dev self-test (B1): replays the captured full GFX command stream
      // through the CPU desktop/surface model. No-op when the sample files are
      // absent (the native side returns immediately).
      RdpNative.runGfxDesktopSelfTest(`${sampleDir}/hmrdp_gfx.bin`,
        `${sampleDir}/hmrdp_gfx_surface.bin`);
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources. The list page's
    // aboutToDisappear() does not run on window close, so drop its state
    // listener here to avoid notifying a destroyed component.
    SessionManager.clearStateListeners();
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}