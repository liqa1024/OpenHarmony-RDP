import { display, window } from '@kit.ArkUI';
import { AbilityConstant, common, StartOptions, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AppSettings, SettingsStore } from './SettingsStore';
import { defaultDisplay } from '../utils/DisplayUtil';

const DOMAIN: number = 0xD002;
const TAG: string = 'HmRdpWindow';
const BUNDLE_NAME: string = 'com.lixa.hmrdp';
const SESSION_ABILITY: string = 'SessionAbility';
const ENTRY_ABILITY: string = 'EntryAbility';

export class WindowController {
  private static stage: window.WindowStage | undefined = undefined;
  private static mainHidden: boolean = false;
  private static mainContext: common.UIAbilityContext | undefined = undefined;
  private static pendingSessionContext: common.UIAbilityContext | undefined = undefined;
  private static pendingTimerId: number = -1;

  static init(stage: window.WindowStage): void {
    WindowController.stage = stage;
  }

  static getMainWindow(): window.Window | undefined {
    const stage: window.WindowStage | undefined = WindowController.stage;
    if (stage === undefined) {
      return undefined;
    }
    try {
      return stage.getMainWindowSync();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'get main window failed: %{public}s', JSON.stringify(error));
      return undefined;
    }
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
    let mainWindow: window.Window | undefined = undefined;
    try {
      mainWindow = stage.getMainWindowSync();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'get main window failed: %{public}s', JSON.stringify(error));
      return;
    }
    WindowController.resizeAndCenter(mainWindow, width, height);
  }

  static openSessionWindow(context: common.UIAbilityContext, sessionKey: string): void {
    const settings: AppSettings = SettingsStore.load();
    const singleWindow: boolean = settings.autoHideMainWindow;
    if (singleWindow) {
      WindowController.mainHidden = true;
      WindowController.mainContext = context;
    }
    const want: Want = {
      bundleName: BUNDLE_NAME,
      abilityName: SESSION_ABILITY,
      parameters: {
        instanceKey: 'rdp_' + sessionKey,
        sessionKey: sessionKey
      }
    };
    if (settings.sessionFullscreen) {
      const options: StartOptions = {
        windowMode: AbilityConstant.WindowMode.WINDOW_MODE_FULLSCREEN
      };
      context.startAbility(want, options).catch((error: BusinessError) => {
        WindowController.cancelHideOnLaunchFailure(singleWindow);
        hilog.error(DOMAIN, TAG, 'start session ability failed: %{public}s',
          JSON.stringify(error));
      });
      return;
    }
    context.startAbility(want).catch((error: BusinessError) => {
      WindowController.cancelHideOnLaunchFailure(singleWindow);
      hilog.error(DOMAIN, TAG, 'start session ability failed: %{public}s',
        JSON.stringify(error));
    });
  }

  private static cancelHideOnLaunchFailure(singleWindow: boolean): void {
    if (singleWindow) {
      WindowController.mainHidden = false;
      WindowController.mainContext = undefined;
    }
  }

  static onSessionWindowReady(): void {
    if (!WindowController.mainHidden) {
      return;
    }
    const context: common.UIAbilityContext | undefined = WindowController.mainContext;
    if (context === undefined) {
      return;
    }
    WindowController.mainContext = undefined;
    context.terminateSelf().catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'terminate main window failed: %{public}s',
        JSON.stringify(error));
    });
  }

  static closeSession(context: common.UIAbilityContext): boolean {
    if (WindowController.pendingSessionContext !== undefined) {
      return true;
    }
    if (!WindowController.mainHidden) {
      return false;
    }
    WindowController.pendingSessionContext = context;
    if (WindowController.pendingTimerId !== -1) {
      clearTimeout(WindowController.pendingTimerId);
    }
    WindowController.pendingTimerId = setTimeout(() => {
      WindowController.pendingTimerId = -1;
      WindowController.terminatePendingSession();
    }, 3000);
    const want: Want = {
      bundleName: BUNDLE_NAME,
      abilityName: ENTRY_ABILITY
    };
    context.startAbility(want).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'restore main window failed: %{public}s',
        JSON.stringify(error));
      WindowController.terminatePendingSession();
    });
    return true;
  }

  static onMainWindowReady(): void {
    if (WindowController.pendingTimerId !== -1) {
      clearTimeout(WindowController.pendingTimerId);
      WindowController.pendingTimerId = -1;
    }
    WindowController.terminatePendingSession();
  }

  private static terminatePendingSession(): void {
    WindowController.mainHidden = false;
    const context: common.UIAbilityContext | undefined =
      WindowController.pendingSessionContext;
    if (context === undefined) {
      return;
    }
    WindowController.pendingSessionContext = undefined;
    context.terminateSelf().catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'terminate session failed: %{public}s',
        JSON.stringify(error));
    });
  }

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
    try {
      mainWindow.on('windowStatusChange', (status: window.WindowStatusType) => {
        if (status === window.WindowStatusType.MAXIMIZE) {
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
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'watch window status failed: %{public}s', JSON.stringify(error));
    }
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

  static acquireWindow(context: common.Context): Promise<window.Window | undefined> {
    return window.getLastWindow(context).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'getLastWindow failed: %{public}s', JSON.stringify(error));
      return undefined;
    });
  }

  static isFullscreen(win: window.Window): boolean {
    try {
      return win.getWindowStatus() === window.WindowStatusType.FULL_SCREEN;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'get window status failed: %{public}s', JSON.stringify(error));
      return false;
    }
  }

  static isMinimized(win: window.Window): boolean {
    try {
      return win.getWindowStatus() === window.WindowStatusType.MINIMIZE;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'get window status failed: %{public}s', JSON.stringify(error));
      return false;
    }
  }

  static watchStatusChange(win: window.Window,
    onChange: (status: window.WindowStatusType) => void): void {
    try {
      win.on('windowStatusChange', onChange);
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'watch window status failed: %{public}s', JSON.stringify(error));
    }
  }

  // HIDDEN counts as unfocused too: a hidden session must release the inputs it still sees as held.
  static watchFocus(win: window.Window, onChange: (focused: boolean) => void): void {
    win.on('windowEvent', (data: window.WindowEventType) => {
      if (data === window.WindowEventType.WINDOW_ACTIVE) {
        onChange(true);
      } else if (data === window.WindowEventType.WINDOW_INACTIVE
        || data === window.WindowEventType.WINDOW_HIDDEN) {
        onChange(false);
      }
    });
  }

  // Only fully covered counts: a partly covered window still reports visible
  // (doc_agent/session-layer.md §6).
  static watchVisibility(win: window.Window, onChange: (visible: boolean) => void): void {
    try {
      win.on('windowVisibilityChange', onChange);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'watch window visibility failed: %{public}s',
        JSON.stringify(error));
    }
  }

  static minimize(win: window.Window): void {
    win.minimize().catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'minimize failed: %{public}s', JSON.stringify(error));
    });
  }

  static toggleFullscreen(win: window.Window, isFullscreen: boolean): void {
    if (isFullscreen) {
      win.recover().catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'recover failed: %{public}s', JSON.stringify(error));
      });
      return;
    }
    win.maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER)
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'maximize failed: %{public}s', JSON.stringify(error));
      });
  }

  private static resizeAndCenter(win: window.Window, width: number, height: number): void {
    const disp: display.Display | undefined = defaultDisplay();
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
}
