/*
 * HmRdp - application preferences (non-sensitive settings).
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';

export class AppSettings {
  toolbarHoverDelay: number = 350;
  toolbarHideDelay: number = 250;
}

export class SettingsStore {
  private static store: preferences.Preferences | undefined = undefined;

  static init(context: common.Context): void {
    if (SettingsStore.store === undefined) {
      SettingsStore.store = preferences.getPreferencesSync(context, { name: 'hmrdp_settings' });
    }
  }

  static load(): AppSettings {
    const settings: AppSettings = new AppSettings();
    const store: preferences.Preferences | undefined = SettingsStore.store;
    if (store !== undefined) {
      settings.toolbarHoverDelay =
        SettingsStore.readNumber(store, 'toolbarHoverDelay', 350);
      settings.toolbarHideDelay =
        SettingsStore.readNumber(store, 'toolbarHideDelay', 250);
    }
    return settings;
  }

  static save(settings: AppSettings): void {
    const store: preferences.Preferences | undefined = SettingsStore.store;
    if (store !== undefined) {
      store.putSync('toolbarHoverDelay', settings.toolbarHoverDelay);
      store.putSync('toolbarHideDelay', settings.toolbarHideDelay);
      store.flush();
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
}
