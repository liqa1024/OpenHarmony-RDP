/*
 * HmRdp - application preferences (non-sensitive settings).
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';

const DEFAULT_TOOLBAR_HOVER_DELAY: number = 350;
const DEFAULT_TOOLBAR_HIDE_DELAY: number = 250;
const MAIN_WINDOW_RATIO: number = 0.45;
const SESSION_WINDOW_RATIO: number = 0.67;

export class AppSettings {
  toolbarHoverDelay: number = DEFAULT_TOOLBAR_HOVER_DELAY;
  toolbarHideDelay: number = DEFAULT_TOOLBAR_HIDE_DELAY;
  mainWindowSizeEnabled: boolean = false;
  mainWindowWidth: number = 0;
  mainWindowHeight: number = 0;
  sessionWindowSizeEnabled: boolean = false;
  sessionWindowWidth: number = 0;
  sessionWindowHeight: number = 0;
}

export class SettingsStore {
  private static store: preferences.Preferences | undefined = undefined;

  static init(context: common.Context): void {
    if (SettingsStore.store === undefined) {
      SettingsStore.store = preferences.getPreferencesSync(context, { name: 'hmrdp_settings' });
    }
  }

  /**
   * Default window sizes derived from the current screen resolution instead of
   * hard-coded pixel values. If the screen cannot be queried, window resizing
   * is disabled rather than falling back to a made-up resolution.
   */
  static defaults(): AppSettings {
    const settings: AppSettings = new AppSettings();
    settings.toolbarHoverDelay = DEFAULT_TOOLBAR_HOVER_DELAY;
    settings.toolbarHideDelay = DEFAULT_TOOLBAR_HIDE_DELAY;
    const disp: display.Display | undefined = SettingsStore.displayOrUndefined();
    if (disp !== undefined) {
      settings.mainWindowSizeEnabled = true;
      settings.mainWindowWidth = Math.round(disp.width * MAIN_WINDOW_RATIO);
      settings.mainWindowHeight = Math.round(disp.height * MAIN_WINDOW_RATIO);
      settings.sessionWindowSizeEnabled = true;
      settings.sessionWindowWidth = Math.round(disp.width * SESSION_WINDOW_RATIO);
      settings.sessionWindowHeight = Math.round(disp.height * SESSION_WINDOW_RATIO);
    }
    return settings;
  }

  static load(): AppSettings {
    const settings: AppSettings = SettingsStore.defaults();
    const store: preferences.Preferences | undefined = SettingsStore.store;
    if (store !== undefined) {
      settings.toolbarHoverDelay =
        SettingsStore.readNumber(store, 'toolbarHoverDelay', settings.toolbarHoverDelay);
      settings.toolbarHideDelay =
        SettingsStore.readNumber(store, 'toolbarHideDelay', settings.toolbarHideDelay);
      settings.mainWindowSizeEnabled =
        SettingsStore.readBoolean(store, 'mainWindowSizeEnabled', settings.mainWindowSizeEnabled);
      settings.mainWindowWidth =
        SettingsStore.readNumber(store, 'mainWindowWidth', settings.mainWindowWidth);
      settings.mainWindowHeight =
        SettingsStore.readNumber(store, 'mainWindowHeight', settings.mainWindowHeight);
      settings.sessionWindowSizeEnabled =
        SettingsStore.readBoolean(store, 'sessionWindowSizeEnabled',
          settings.sessionWindowSizeEnabled);
      settings.sessionWindowWidth =
        SettingsStore.readNumber(store, 'sessionWindowWidth', settings.sessionWindowWidth);
      settings.sessionWindowHeight =
        SettingsStore.readNumber(store, 'sessionWindowHeight', settings.sessionWindowHeight);
    }
    return settings;
  }

  static save(settings: AppSettings): void {
    const store: preferences.Preferences | undefined = SettingsStore.store;
    if (store !== undefined) {
      store.putSync('toolbarHoverDelay', settings.toolbarHoverDelay);
      store.putSync('toolbarHideDelay', settings.toolbarHideDelay);
      store.putSync('mainWindowSizeEnabled', settings.mainWindowSizeEnabled);
      store.putSync('mainWindowWidth', settings.mainWindowWidth);
      store.putSync('mainWindowHeight', settings.mainWindowHeight);
      store.putSync('sessionWindowSizeEnabled', settings.sessionWindowSizeEnabled);
      store.putSync('sessionWindowWidth', settings.sessionWindowWidth);
      store.putSync('sessionWindowHeight', settings.sessionWindowHeight);
      store.flush();
    }
  }

  private static displayOrUndefined(): display.Display | undefined {
    try {
      return display.getDefaultDisplaySync();
    } catch (error) {
      return undefined;
    }
  }

  private static readNumber(store: preferences.Preferences, key: string,
    fallback: number): number {
    const value: preferences.ValueType = store.getSync(key, fallback);
    if (typeof value === 'number') {
      return value as number;
    }
    return fallback;
  }

  private static readBoolean(store: preferences.Preferences, key: string,
    fallback: boolean): boolean {
    const value: preferences.ValueType = store.getSync(key, fallback);
    if (typeof value === 'boolean') {
      return value as boolean;
    }
    return fallback;
  }
}
