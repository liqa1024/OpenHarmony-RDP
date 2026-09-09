/*
 * HmRdp - window sizing and session window launch.
 */
import { display, window } from '@kit.ArkUI';
import { AbilityConstant, common, StartOptions, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AppSettings, SettingsStore } from './SettingsStore';

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
   * be moved and resized like any other window. `sessionKey` identifies the
   * launch request and is also used as the unique instanceKey, so several
   * sessions can be open at once.
   */
  static openSessionWindow(context: common.UIAbilityContext, sessionKey: string): void {
    const want: Want = {
      bundleName: BUNDLE_NAME,
      abilityName: SESSION_ABILITY,
      parameters: {
        instanceKey: 'rdp_' + sessionKey,
        sessionKey: sessionKey
      }
    };
    const settings: AppSettings = SettingsStore.load();
    if (settings.sessionFullscreen) {
      const options: StartOptions = {
        windowMode: AbilityConstant.WindowMode.WINDOW_MODE_FULLSCREEN
      };
      context.startAbility(want, options).catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'start session ability failed: %{public}s',
          JSON.stringify(error));
      });
      return;
    }
    context.startAbility(want).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'start session ability failed: %{public}s',
        JSON.stringify(error));
    });
  }

  /**
   * Session windows always maximize to immersive fullscreen; the system title
   * bar and dock hover are disabled there so they do not cover the app's own
   * auto-hide toolbar (which carries the minimize / restore buttons instead).
   */
  static setupSessionWindow(stage: window.WindowStage, fullscreenDefault: boolean): void {
    let win: window.Window | undefined = undefined;
    try {
      win = stage.getMainWindowSync();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'get main window failed: %{public}s', JSON.stringify(error));
      return;
    }
    if (win === undefined) {
      return;
    }
    const mainWindow: window.Window = win;
    mainWindow.on('windowStatusChange', (status: window.WindowStatusType) => {
      if (status === window.WindowStatusType.MAXIMIZE) {
        // The system maximize button only maximizes; promote it to immersive
        // fullscreen so the session always fills the whole screen.
        mainWindow.maximize(
          window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER)
          .catch((error: BusinessError) => {
            hilog.error(DOMAIN, TAG, 'enter immersive failed: %{public}s',
              JSON.stringify(error));
          });
      } else if (status === window.WindowStatusType.FULL_SCREEN) {
        WindowController.hideSystemTitleAndDock(mainWindow);
      }
    });
    if (fullscreenDefault) {
      WindowController.hideSystemTitleAndDock(mainWindow);
    }
  }

  private static hideSystemTitleAndDock(win: window.Window | undefined): void {
    if (win === undefined) {
      return;
    }
    win.setTitleAndDockHoverShown(false, false).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'hide title/dock hover failed: %{public}s',
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
