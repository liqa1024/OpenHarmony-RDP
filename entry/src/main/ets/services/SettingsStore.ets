/*
 * HmRdp - application preferences (non-sensitive settings).
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import { DisplayProfile, SavedConnection } from '../model/RdpModels';

const DEFAULT_TOOLBAR_HOVER_DELAY: number = 350;
const DEFAULT_TOOLBAR_HIDE_DELAY: number = 250;
const DEFAULT_TOUCHPAD_SCROLL_UNIT: number = 40;
const DEFAULT_PINCH_ZOOM_STEP_PERCENT: number = 5;
const MAIN_WINDOW_RATIO: number = 0.45;
const SESSION_WINDOW_RATIO: number = 0.67;
const FALLBACK_WIDTH: number = 1920;
const FALLBACK_HEIGHT: number = 1080;

// Windows only applies a fixed set of DPI scaling presets; other values can
// cause rendering problems, so the scale is always snapped to one of these.
export const SCALE_PRESETS: number[] = [100, 125, 150, 175, 200, 225];

export class AppSettings {
  toolbarHoverDelay: number = DEFAULT_TOOLBAR_HOVER_DELAY;
  toolbarHideDelay: number = DEFAULT_TOOLBAR_HIDE_DELAY;
  touchpadScrollUnit: number = DEFAULT_TOUCHPAD_SCROLL_UNIT;
  pinchZoomStepPercent: number = DEFAULT_PINCH_ZOOM_STEP_PERCENT;
  mainWindowSizeEnabled: boolean = false;
  mainWindowWidth: number = 0;
  mainWindowHeight: number = 0;
  sessionWindowSizeEnabled: boolean = false;
  sessionWindowWidth: number = 0;
  sessionWindowHeight: number = 0;
  // Global display defaults: automatic resolution / scaling by default, with an
  // optional manual override shared by every connection.
  resolutionAuto: boolean = true;
  manualWidth: number = 0;
  manualHeight: number = 0;
  scaleAuto: boolean = true;
  manualScalePercent: number = 100;
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
    settings.touchpadScrollUnit = DEFAULT_TOUCHPAD_SCROLL_UNIT;
    settings.pinchZoomStepPercent = DEFAULT_PINCH_ZOOM_STEP_PERCENT;
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
      settings.touchpadScrollUnit =
        SettingsStore.readNumber(store, 'touchpadScrollUnit', settings.touchpadScrollUnit);
      settings.pinchZoomStepPercent =
        SettingsStore.readNumber(store, 'pinchZoomStepPercent', settings.pinchZoomStepPercent);
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
      settings.resolutionAuto =
        SettingsStore.readBoolean(store, 'resolutionAuto', settings.resolutionAuto);
      settings.manualWidth =
        SettingsStore.readNumber(store, 'manualWidth', settings.manualWidth);
      settings.manualHeight =
        SettingsStore.readNumber(store, 'manualHeight', settings.manualHeight);
      settings.scaleAuto =
        SettingsStore.readBoolean(store, 'scaleAuto', settings.scaleAuto);
      settings.manualScalePercent =
        SettingsStore.readNumber(store, 'manualScalePercent', settings.manualScalePercent);
    }
    return settings;
  }

  static save(settings: AppSettings): void {
    const store: preferences.Preferences | undefined = SettingsStore.store;
    if (store !== undefined) {
      store.putSync('toolbarHoverDelay', settings.toolbarHoverDelay);
      store.putSync('toolbarHideDelay', settings.toolbarHideDelay);
      store.putSync('touchpadScrollUnit', settings.touchpadScrollUnit);
      store.putSync('pinchZoomStepPercent', settings.pinchZoomStepPercent);
      store.putSync('mainWindowSizeEnabled', settings.mainWindowSizeEnabled);
      store.putSync('mainWindowWidth', settings.mainWindowWidth);
      store.putSync('mainWindowHeight', settings.mainWindowHeight);
      store.putSync('sessionWindowSizeEnabled', settings.sessionWindowSizeEnabled);
      store.putSync('sessionWindowWidth', settings.sessionWindowWidth);
      store.putSync('sessionWindowHeight', settings.sessionWindowHeight);
      store.putSync('resolutionAuto', settings.resolutionAuto);
      store.putSync('manualWidth', settings.manualWidth);
      store.putSync('manualHeight', settings.manualHeight);
      store.putSync('scaleAuto', settings.scaleAuto);
      store.putSync('manualScalePercent', settings.manualScalePercent);
      store.flush();
    }
  }

  /**
   * Current monitor resolution, used as the automatic session size. Returns an
   * invalid profile when the display cannot be queried so callers can decide.
   */
  static detectedDisplay(): DisplayProfile {
    const profile: DisplayProfile = new DisplayProfile();
    const disp: display.Display | undefined = SettingsStore.displayOrUndefined();
    if (disp !== undefined && disp.width > 0 && disp.height > 0) {
      profile.width = disp.width;
      profile.height = disp.height;
      profile.detected = true;
    }
    return profile;
  }

  /**
   * Recommended RDP scale factor derived from the display density: HarmonyOS
   * uses 160 DPI as 100%, so densityPixels maps directly to a percentage.
   */
  static recommendedScalePercent(): number {
    const disp: display.Display | undefined = SettingsStore.displayOrUndefined();
    if (disp === undefined || disp.densityPixels <= 0) {
      return SCALE_PRESETS[0];
    }
    return SettingsStore.nearestScalePreset(Math.round(disp.densityPixels * 100));
  }

  /** Snaps an arbitrary percentage to the nearest supported Windows preset. */
  static nearestScalePreset(percent: number): number {
    let best: number = SCALE_PRESETS[0];
    let bestDiff: number = Math.abs(percent - best);
    for (const preset of SCALE_PRESETS) {
      const diff: number = Math.abs(percent - preset);
      if (diff < bestDiff) {
        bestDiff = diff;
        best = preset;
      }
    }
    return best;
  }

  /**
   * Global display profile (auto detected resolution / recommended scale, or the
   * manual values when the user has overridden them).
   */
  static globalDisplayProfile(): DisplayProfile {
    return SettingsStore.profileFor(SettingsStore.load());
  }

  /**
   * Effective display profile for a connection: global settings unless the
   * connection opted into a per-connection override.
   */
  static resolveDisplay(conn: SavedConnection): DisplayProfile {
    const global: DisplayProfile = SettingsStore.profileFor(SettingsStore.load());
    if (conn.useGlobalDisplay) {
      return global;
    }
    const profile: DisplayProfile = new DisplayProfile();
    if (conn.width > 0 && conn.height > 0) {
      profile.width = conn.width;
      profile.height = conn.height;
    } else {
      profile.width = global.width;
      profile.height = global.height;
      profile.detected = global.detected;
    }
    profile.scalePercent = conn.scalePercent > 0
      ? SettingsStore.nearestScalePreset(conn.scalePercent) : global.scalePercent;
    return profile;
  }

  private static profileFor(settings: AppSettings): DisplayProfile {
    const profile: DisplayProfile = new DisplayProfile();
    if (!settings.resolutionAuto && settings.manualWidth > 0 && settings.manualHeight > 0) {
      profile.width = settings.manualWidth;
      profile.height = settings.manualHeight;
    } else {
      const detected: DisplayProfile = SettingsStore.detectedDisplay();
      if (detected.valid()) {
        profile.width = detected.width;
        profile.height = detected.height;
        profile.detected = true;
      } else {
        profile.width = FALLBACK_WIDTH;
        profile.height = FALLBACK_HEIGHT;
      }
    }
    if (!settings.scaleAuto && settings.manualScalePercent > 0) {
      profile.scalePercent = SettingsStore.nearestScalePreset(settings.manualScalePercent);
    } else {
      profile.scalePercent = SettingsStore.recommendedScalePercent();
    }
    return profile;
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
