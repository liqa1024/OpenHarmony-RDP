/*
 * HmRdp - application preferences (non-sensitive settings).
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AdvancedProfile, DisplayProfile, SavedConnection } from '../model/RdpModels';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSettings';

const DEFAULT_TOOLBAR_HOVER_DELAY: number = 700;
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
  // KeyCode remapped to Win/Meta for the remote session (0 = disabled). Lets
  // Win combinations work even though the shell keeps the real Win key.
  winKeySubstitute: number = 0;
  mainWindowSizeEnabled: boolean = false;
  mainWindowWidth: number = 0;
  mainWindowHeight: number = 0;
  sessionWindowSizeEnabled: boolean = false;
  sessionWindowWidth: number = 0;
  sessionWindowHeight: number = 0;
  sessionFullscreen: boolean = false;
  // When true the main window is minimized while a session window is open and
  // restored once the last session window closes.
  autoHideMainWindow: boolean = false;
  // Global display defaults: automatic resolution / scaling by default, with an
  // optional manual override shared by every connection.
  resolutionAuto: boolean = true;
  manualWidth: number = 0;
  manualHeight: number = 0;
  scaleAuto: boolean = true;
  manualScalePercent: number = 100;
  // Global advanced (protocol feature) defaults. Connections follow these
  // unless they opt into their own overrides in the editor.
  enableClipboard: boolean = true;
  enableAudio: boolean = true;
  enableGfx: boolean = true;
  enableH264: boolean = true;
  ignoreCertificate: boolean = true;
}

export class SettingsStore {
  private static store: preferences.Preferences | undefined = undefined;

  static init(context: common.Context): void {
    if (SettingsStore.store !== undefined) {
      return;
    }
    try {
      SettingsStore.store = preferences.getPreferencesSync(context, { name: 'hmrdp_settings' });
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'open settings store failed: %{public}s', JSON.stringify(error));
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
      settings.winKeySubstitute =
        SettingsStore.readNumber(store, 'winKeySubstitute', settings.winKeySubstitute);
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
      settings.sessionFullscreen =
        SettingsStore.readBoolean(store, 'sessionFullscreen', settings.sessionFullscreen);
      settings.autoHideMainWindow =
        SettingsStore.readBoolean(store, 'autoHideMainWindow', settings.autoHideMainWindow);
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
      settings.enableClipboard =
        SettingsStore.readBoolean(store, 'enableClipboard', settings.enableClipboard);
      settings.enableAudio =
        SettingsStore.readBoolean(store, 'enableAudio', settings.enableAudio);
      settings.enableGfx =
        SettingsStore.readBoolean(store, 'enableGfx', settings.enableGfx);
      settings.enableH264 =
        SettingsStore.readBoolean(store, 'enableH264', settings.enableH264);
      settings.ignoreCertificate =
        SettingsStore.readBoolean(store, 'ignoreCertificate', settings.ignoreCertificate);
    }
    return settings;
  }

  static save(settings: AppSettings): void {
    const store: preferences.Preferences | undefined = SettingsStore.store;
    if (store === undefined) {
      return;
    }
    try {
      store.putSync('toolbarHoverDelay', settings.toolbarHoverDelay);
      store.putSync('toolbarHideDelay', settings.toolbarHideDelay);
      store.putSync('touchpadScrollUnit', settings.touchpadScrollUnit);
      store.putSync('pinchZoomStepPercent', settings.pinchZoomStepPercent);
      store.putSync('winKeySubstitute', settings.winKeySubstitute);
      store.putSync('mainWindowSizeEnabled', settings.mainWindowSizeEnabled);
      store.putSync('mainWindowWidth', settings.mainWindowWidth);
      store.putSync('mainWindowHeight', settings.mainWindowHeight);
      store.putSync('sessionWindowSizeEnabled', settings.sessionWindowSizeEnabled);
      store.putSync('sessionWindowWidth', settings.sessionWindowWidth);
      store.putSync('sessionWindowHeight', settings.sessionWindowHeight);
      store.putSync('sessionFullscreen', settings.sessionFullscreen);
      store.putSync('autoHideMainWindow', settings.autoHideMainWindow);
      store.putSync('resolutionAuto', settings.resolutionAuto);
      store.putSync('manualWidth', settings.manualWidth);
      store.putSync('manualHeight', settings.manualHeight);
      store.putSync('scaleAuto', settings.scaleAuto);
      store.putSync('manualScalePercent', settings.manualScalePercent);
      store.putSync('enableClipboard', settings.enableClipboard);
      store.putSync('enableAudio', settings.enableAudio);
      store.putSync('enableGfx', settings.enableGfx);
      store.putSync('enableH264', settings.enableH264);
      store.putSync('ignoreCertificate', settings.ignoreCertificate);
      store.flush();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'save settings failed: %{public}s', JSON.stringify(error));
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
    profile.scalePercent = conn.scalePercent > 0 ? conn.scalePercent : global.scalePercent;
    return profile;
  }

  /** Global advanced (protocol feature) defaults shared by every connection. */
  static globalAdvancedProfile(): AdvancedProfile {
    return SettingsStore.advancedFor(SettingsStore.load());
  }

  /**
   * Effective advanced profile for a connection: global settings unless the
   * connection opted into its own overrides.
   */
  static resolveAdvanced(conn: SavedConnection): AdvancedProfile {
    if (conn.useGlobalAdvanced) {
      return SettingsStore.globalAdvancedProfile();
    }
    const profile: AdvancedProfile = new AdvancedProfile();
    profile.enableClipboard = conn.enableClipboard;
    profile.enableAudio = conn.enableAudio;
    profile.enableGfx = conn.enableGfx;
    profile.enableH264 = conn.enableH264;
    profile.ignoreCertificate = conn.ignoreCertificate;
    return profile;
  }

  private static advancedFor(settings: AppSettings): AdvancedProfile {
    const profile: AdvancedProfile = new AdvancedProfile();
    profile.enableClipboard = settings.enableClipboard;
    profile.enableAudio = settings.enableAudio;
    profile.enableGfx = settings.enableGfx;
    profile.enableH264 = settings.enableH264;
    profile.ignoreCertificate = settings.ignoreCertificate;
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
      profile.scalePercent = settings.manualScalePercent;
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
    try {
      const value: preferences.ValueType = store.getSync(key, fallback);
      if (typeof value === 'number') {
        return value as number;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'read %{public}s failed: %{public}s', key, JSON.stringify(error));
    }
    return fallback;
  }

  private static readBoolean(store: preferences.Preferences, key: string,
    fallback: boolean): boolean {
    try {
      const value: preferences.ValueType = store.getSync(key, fallback);
      if (typeof value === 'boolean') {
        return value as boolean;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'read %{public}s failed: %{public}s', key, JSON.stringify(error));
    }
    return fallback;
  }
}
