/*
 * HmRdp - window sizing and session window launch.
 */
import { display, window } from '@kit.ArkUI';
import { common, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD002;
const TAG: string = 'HmRdpWindow';
const BUNDLE_NAME: string = 'com.lixa.hmrdp';
const SESSION_ABILITY: string = 'SessionAbility';

export class WindowController {
  private static stage: window.WindowStage | undefined = undefined;

  static init(stage: window.WindowStage): void {
    WindowController.stage = stage;
  }

  static getMainWindow(): window.Window | undefined {
    const stage: window.WindowStage | undefined = WindowController.stage;
    if (stage === undefined) {
      return undefined;
    }
    return stage.getMainWindowSync();
  }

  static applyMainWindowSize(width: number, height: number, enabled: boolean): void {
    if (!enabled || width <= 0 || height <= 0) {
      return;
    }
    const mainWindow: window.Window | undefined = WindowController.getMainWindow();
    if (mainWindow === undefined) {
      return;
    }
    WindowController.resizeAndCenter(mainWindow, width, height);
  }

  static applyWindowStageSize(stage: window.WindowStage, width: number, height: number,
    enabled: boolean): void {
    if (!enabled || width <= 0 || height <= 0) {
      return;
    }
    WindowController.resizeAndCenter(stage.getMainWindowSync(), width, height);
  }

  /**
   * Opens the RDP session in an independent main window (SessionAbility) so it
   * gets a standard title bar with minimize / maximize / close buttons and can
   * be moved and resized like any other window.
   */
  static openSessionWindow(context: common.UIAbilityContext): void {
    const want: Want = {
      bundleName: BUNDLE_NAME,
      abilityName: SESSION_ABILITY,
      parameters: {
        instanceKey: 'rdp_' + Date.now().toString()
      }
    };
    context.startAbility(want).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'start session ability failed: %{public}s',
        JSON.stringify(error));
    });
  }

  private static resizeAndCenter(win: window.Window, width: number, height: number): void {
    const disp: display.Display | undefined = WindowController.displayOrUndefined();
    if (disp === undefined) {
      hilog.warn(DOMAIN, TAG, 'display unavailable, skip window resize');
      return;
    }
    const targetWidth: number = Math.max(320, Math.min(width, disp.width));
    const targetHeight: number = Math.max(240, Math.min(height, disp.height));
    win.resize(targetWidth, targetHeight).then(() => {
      const x: number = Math.max(0, Math.round((disp.width - targetWidth) / 2));
      const y: number = Math.max(0, Math.round((disp.height - targetHeight) / 2));
      win.moveWindowTo(x, y).catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'moveWindowTo failed: %{public}s', JSON.stringify(error));
      });
    }).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'resize failed: %{public}s', JSON.stringify(error));
    });
  }

  private static displayOrUndefined(): display.Display | undefined {
    try {
      return display.getDefaultDisplaySync();
    } catch (error) {
      return undefined;
    }
  }
}
