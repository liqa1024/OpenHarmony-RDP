/*
 * HmRdp - application preferences (non-sensitive settings).
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { AdvancedProfile, DisplayProfile, SavedConnection, SrProfile } from '../model/RdpModels';
import { DEFAULT_SPEED_SCALE } from './TouchpadWheel';
import { DeviceCapabilities } from './DeviceCapabilities';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSettings';

const DEFAULT_TOOLBAR_HOVER_DELAY: number = 700;
const DEFAULT_TOOLBAR_HIDE_DELAY: number = 250;
// Initial separation of the two synthetic touch-pinch contacts, as a percentage
// of the smaller desktop side (window-relative, not a fixed resolution) at 1.0x
// gesture ratio. Shared so the settings placeholder and the session's own
// fallback cannot drift from the stored default.
export const DEFAULT_PINCH_TOUCH_DISTANCE: number = 8;
const MAIN_WINDOW_RATIO: number = 0.45;
const SESSION_WINDOW_RATIO: number = 0.67;
const FALLBACK_WIDTH: number = 1920;
const FALLBACK_HEIGHT: number = 1080;

// Windows only applies a fixed set of DPI scaling presets; other values can
// cause rendering problems, so the scale is always snapped to one of these.
export const SCALE_PRESETS: number[] = [100, 125, 150, 175, 200, 225];

// Frame-rate cap presets in frames per second; 0 means "no cap". The cap lowers
// how many frames the server sends (the client holds each frame back before its
// decode, and the frame acknowledge is written when that frame ends), so it also
// lowers the decode and bandwidth cost of a busy desktop. 20 sits between 15 and
// 30 because that gap is wide enough to be worth a preset of its own.
export const FPS_PRESETS: number[] = [0, 15, 20, 30, 45, 60];

/** Label shown for an FPS_PRESETS entry. */
export function fpsLabel(preset: number): string {
  return preset > 0 ? `${preset} fps` : '不限制';
}

// 超分辨率倍率 (output ÷ session) presets in percent. The platform *recommends*
// up to 1.5× for its GPU spatial upscale; larger ratios save more bandwidth but
// the sharper edges of a text-heavy desktop start to ring, so those presets are
// labelled as such in the UI. There is no hard API limit on the ratio.
export const SR_RATIO_PRESETS: number[] = [125, 150, 175, 200];

/** Label shown for an SR_RATIO_PRESETS entry ("1.25x"), and in the toolbar. */
export function srRatioLabel(preset: number): string {
  if (preset % 100 === 0) {
    return `${preset / 100}x`;
  }
  if (preset % 10 === 0) {
    return `${(preset / 100).toFixed(1)}x`;
  }
  return `${(preset / 100).toFixed(2)}x`;
}

export class AppSettings {
  toolbarHoverDelay: number = DEFAULT_TOOLBAR_HOVER_DELAY;
  toolbarHideDelay: number = DEFAULT_TOOLBAR_HIDE_DELAY;
  touchpadScrollSpeed: number = DEFAULT_SPEED_SCALE;
  pinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  // When true the touchpad pinch is emulated as a native two-finger touch pinch
  // at the pointer; when false it stays Ctrl+wheel (the default).
  pinchAsTouch: boolean = false;
  // Tilt (degrees) of the synthetic contact pair for touch pinch: positive is
  // clockwise on screen (left hand), negative flips it (right hand, e.g. -30).
  // ArkUI cannot report the real finger angle, so this stands in for the hand.
  pinchTouchAngle: number = 30;
  // Initial separation of the two synthetic touch-pinch contacts, as a
  // percentage of the smaller desktop side (window-relative, not a fixed
  // resolution) at 1.0x gesture ratio.
  pinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  // Experimental: forward RDPEI touch at the full input rate instead of the
  // upstream ~20ms (50Hz) coalescing. Off keeps the default behaviour.
  touchHighRate: boolean = false;
  // When true the remote cursor shape is mapped onto the HarmonyOS system
  // cursor. Turn off to fall back to the plain default cursor.
  useRdpCursor: boolean = true;
  // When true a session window that comes back to the foreground and finds its
  // session already dropped reconnects on its own, instead of waiting for the
  // user to press 重试. A system that freezes and then aborts the app's sockets
  // while it is away leaves a session that looks connected but no longer is, so
  // this is a safety net for coming back, not a keep-alive.
  reconnectOnForeground: boolean = false;
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
  enableAudio: boolean = true;
  enableGfx: boolean = true;
  enableH264: boolean = true;
  ignoreCertificate: boolean = true;
  // Frame-rate cap in frames per second shared by every connection that follows
  // the global advanced settings; 0 leaves the frame rate to the server.
  maxFps: number = 0;
  // 超分 (global default; per-connection override in the editor). The resolution
  // setting is the *output* resolution, the session renders at output ÷ ratio and
  // the Vulkan presenter upscales it back. Off by default: it changes what the
  // server is asked to render, so it is opt-in.
  srEnabled: boolean = false;
  srRatioPercent: number = SR_RATIO_PRESETS[0];
  // Present frames through Vulkan; off keeps the session on the GLES presenter
  // with no Vulkan at all (see doc_agent/settings-and-storage.md §3).
  hardwareAccel: boolean = true;
  // Developer options: reveals the dev-only tools (recorded-stream replay entry,
  // RFX stream capture, decode-worker knob) and the session toolbar's performance
  // telemetry. Off for a normal install; nothing dev-only is reachable until it
  // is turned on.
  developerMode: boolean = false;
  // Dev-only capture of the RemoteFX/Progressive stream (doc_agent/gfx-engine.md §6).
  rfxDump: boolean = false;
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
    settings.touchpadScrollSpeed = DEFAULT_SPEED_SCALE;
    settings.pinchZoomSpeed = DEFAULT_SPEED_SCALE;
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
      settings.touchpadScrollSpeed =
        SettingsStore.readNumber(store, 'touchpadScrollSpeed', settings.touchpadScrollSpeed);
      settings.pinchZoomSpeed =
        SettingsStore.readNumber(store, 'pinchZoomSpeed', settings.pinchZoomSpeed);
      settings.pinchAsTouch =
        SettingsStore.readBoolean(store, 'pinchAsTouch', settings.pinchAsTouch);
      settings.pinchTouchAngle =
        SettingsStore.readNumber(store, 'pinchTouchAngle', settings.pinchTouchAngle);
      settings.pinchTouchDistance =
        SettingsStore.readNumber(store, 'pinchTouchDistance', settings.pinchTouchDistance);
      settings.touchHighRate =
        SettingsStore.readBoolean(store, 'touchHighRate', settings.touchHighRate);
      settings.useRdpCursor =
        SettingsStore.readBoolean(store, 'useRdpCursor', settings.useRdpCursor);
      settings.reconnectOnForeground =
        SettingsStore.readBoolean(store, 'reconnectOnForeground', settings.reconnectOnForeground);
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
      settings.enableAudio =
        SettingsStore.readBoolean(store, 'enableAudio', settings.enableAudio);
      settings.enableGfx =
        SettingsStore.readBoolean(store, 'enableGfx', settings.enableGfx);
      settings.enableH264 =
        SettingsStore.readBoolean(store, 'enableH264', settings.enableH264);
      settings.ignoreCertificate =
        SettingsStore.readBoolean(store, 'ignoreCertificate', settings.ignoreCertificate);
      settings.maxFps = SettingsStore.readNumber(store, 'maxFps', settings.maxFps);
      settings.srEnabled = SettingsStore.readBoolean(store, 'srEnabled', settings.srEnabled);
      settings.srRatioPercent =
        SettingsStore.readNumber(store, 'srRatioPercent', settings.srRatioPercent);
      settings.hardwareAccel =
        SettingsStore.readBoolean(store, 'hardwareAccel', settings.hardwareAccel);
      settings.developerMode =
        SettingsStore.readBoolean(store, 'developerMode', settings.developerMode);
      settings.rfxDump = SettingsStore.readBoolean(store, 'rfxDump', settings.rfxDump);
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
      store.putSync('touchpadScrollSpeed', settings.touchpadScrollSpeed);
      store.putSync('pinchZoomSpeed', settings.pinchZoomSpeed);
      store.putSync('pinchAsTouch', settings.pinchAsTouch);
      store.putSync('pinchTouchAngle', settings.pinchTouchAngle);
      store.putSync('pinchTouchDistance', settings.pinchTouchDistance);
      store.putSync('touchHighRate', settings.touchHighRate);
      store.putSync('useRdpCursor', settings.useRdpCursor);
      store.putSync('reconnectOnForeground', settings.reconnectOnForeground);
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
      store.putSync('enableAudio', settings.enableAudio);
      store.putSync('enableGfx', settings.enableGfx);
      store.putSync('enableH264', settings.enableH264);
      store.putSync('ignoreCertificate', settings.ignoreCertificate);
      store.putSync('maxFps', settings.maxFps);
      store.putSync('srEnabled', settings.srEnabled);
      store.putSync('srRatioPercent', settings.srRatioPercent);
      store.putSync('hardwareAccel', settings.hardwareAccel);
      store.putSync('developerMode', settings.developerMode);
      store.putSync('rfxDump', settings.rfxDump);
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

  /** Snaps an arbitrary frame-rate cap to the nearest supported preset. */
  static snapFpsPreset(value: number): number {
    let best: number = FPS_PRESETS[0];
    let bestDiff: number = Math.abs(value - best);
    for (const preset of FPS_PRESETS) {
      const diff: number = Math.abs(value - preset);
      if (diff < bestDiff) {
        bestDiff = diff;
        best = preset;
      }
    }
    return best;
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

  /** Snaps an arbitrary upscale ratio to the nearest offered preset. */
  static snapSrRatioPreset(percent: number): number {
    let best: number = SR_RATIO_PRESETS[0];
    let bestDiff: number = Math.abs(percent - best);
    for (const preset of SR_RATIO_PRESETS) {
      const diff: number = Math.abs(percent - preset);
      if (diff < bestDiff) {
        bestDiff = diff;
        best = preset;
      }
    }
    return best;
  }

  /**
   * Session (pre-super-resolution) size for an output size: the size the server
   * is asked to render, so the presenter can upscale it back. Rounded to an even
   * number of pixels (odd desktop sizes render badly on some servers).
   */
  static srSessionSize(output: number, ratioPercent: number): number {
    if (output <= 0 || ratioPercent <= 100) {
      return output;
    }
    const scaled: number = Math.round(output * 100 / ratioPercent);
    const even: number = scaled - (scaled % 2);
    return even > 0 ? even : 2;
  }

  /**
   * Default DPI scale for the session desktop. Without 超分 it is the display
   * density's recommended preset; with 超分 the session desktop is smaller by the
   * ratio but is displayed at the output size, so the factor shrinks by the same
   * ratio to keep the remote UI the same physical size (and never goes below the
   * first Windows preset).
   */
  static defaultScalePercent(srEnabled: boolean, ratioPercent: number): number {
    const recommended: number = SettingsStore.recommendedScalePercent();
    if (!srEnabled || ratioPercent <= 100) {
      return recommended;
    }
    return SettingsStore.nearestScalePreset(Math.round(recommended * 100 / ratioPercent));
  }

  /** Global 超分 defaults. */
  static globalSrProfile(): SrProfile {
    const settings: AppSettings = SettingsStore.load();
    const profile: SrProfile = new SrProfile();
    profile.enabled = settings.srEnabled;
    profile.ratioPercent = SettingsStore.snapSrRatioPreset(settings.srRatioPercent);
    return SettingsStore.gateSr(profile);
  }

  /**
   * Effective 超分 for a connection: the global defaults unless the connection
   * opted into its own. Forced off where the device cannot do it (no Vulkan
   * presenter, no XEngine spatial upscale) or where 硬件加速 is off, so the
   * session never negotiates a lower resolution it cannot upscale back.
   */
  static resolveSr(conn: SavedConnection): SrProfile {
    const settings: AppSettings = SettingsStore.load();
    const profile: SrProfile = new SrProfile();
    if (conn.useGlobalSr) {
      profile.enabled = settings.srEnabled;
      profile.ratioPercent = SettingsStore.snapSrRatioPreset(settings.srRatioPercent);
    } else {
      profile.enabled = conn.srEnabled;
      profile.ratioPercent = SettingsStore.snapSrRatioPreset(conn.srRatioPercent);
    }
    return SettingsStore.gateSr(profile);
  }

  private static gateSr(profile: SrProfile): SrProfile {
    if (profile.enabled && !SettingsStore.superResolutionUsable()) {
      profile.enabled = false;
    }
    if (profile.ratioPercent <= 100) {
      profile.ratioPercent = SR_RATIO_PRESETS[0];
    }
    return profile;
  }

  /**
   * Whether 超分 can actually be used right now: the Vulkan presenter is allowed
   * (硬件加速 on) and the device provides the XEngine spatial upscale. A device
   * that misses either half must not negotiate a lower session resolution.
   */
  static superResolutionUsable(): boolean {
    const settings: AppSettings = SettingsStore.load();
    return settings.hardwareAccel && DeviceCapabilities.hardwareAccel().supported
      && DeviceCapabilities.superResolution().supported;
  }

  /**
   * Global display profile (auto detected resolution / recommended scale, or the
   * manual values when the user has overridden them), with the global 超分.
   */
  static globalDisplayProfile(): DisplayProfile {
    const settings: AppSettings = SettingsStore.load();
    return SettingsStore.profileFor(settings, SettingsStore.globalSrProfile());
  }

  /**
   * Effective display profile for a connection: global settings unless the
   * connection opted into a per-connection override. `width`/`height` are the
   * output resolution and `sessionWidth`/`sessionHeight` what the server renders.
   */
  static resolveDisplay(conn: SavedConnection): DisplayProfile {
    const settings: AppSettings = SettingsStore.load();
    const sr: SrProfile = SettingsStore.resolveSr(conn);
    const global: DisplayProfile = SettingsStore.profileFor(settings, sr);
    if (conn.useGlobalDisplay) {
      return global;
    }
    const hasOwnSize: boolean = conn.width > 0 && conn.height > 0;
    const width: number = hasOwnSize ? conn.width : global.width;
    const height: number = hasOwnSize ? conn.height : global.height;
    const scale: number = conn.scalePercent > 0 ? conn.scalePercent : global.scalePercent;
    return SettingsStore.makeProfile(width, height, global.detected, scale, sr);
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
    profile.enableAudio = conn.enableAudio;
    profile.enableGfx = conn.enableGfx;
    profile.enableH264 = conn.enableH264;
    profile.ignoreCertificate = conn.ignoreCertificate;
    profile.maxFps = conn.maxFps;
    return profile;
  }

  private static advancedFor(settings: AppSettings): AdvancedProfile {
    const profile: AdvancedProfile = new AdvancedProfile();
    profile.enableAudio = settings.enableAudio;
    profile.enableGfx = settings.enableGfx;
    profile.enableH264 = settings.enableH264;
    profile.ignoreCertificate = settings.ignoreCertificate;
    profile.maxFps = settings.maxFps;
    return profile;
  }

  private static profileFor(settings: AppSettings, sr: SrProfile): DisplayProfile {
    let width: number = 0;
    let height: number = 0;
    let detected: boolean = false;
    if (!settings.resolutionAuto && settings.manualWidth > 0 && settings.manualHeight > 0) {
      width = settings.manualWidth;
      height = settings.manualHeight;
    } else {
      const display: DisplayProfile = SettingsStore.detectedDisplay();
      if (display.valid()) {
        width = display.width;
        height = display.height;
        detected = true;
      } else {
        width = FALLBACK_WIDTH;
        height = FALLBACK_HEIGHT;
      }
    }
    const enabled: boolean = sr.enabled && sr.ratioPercent > 100;
    const defaultScale: number = SettingsStore.defaultScalePercent(enabled, sr.ratioPercent);
    const scale: number = (!settings.scaleAuto && settings.manualScalePercent > 0)
      ? settings.manualScalePercent : defaultScale;
    return SettingsStore.makeProfile(width, height, detected, scale, sr);
  }

  /**
   * Assembles a profile from the output size, its DPI scale and the 超分 choice:
   * the session size (what the server renders) and the SR fields follow from the
   * output size and the ratio.
   */
  private static makeProfile(width: number, height: number, detected: boolean,
    scalePercent: number, sr: SrProfile): DisplayProfile {
    const profile: DisplayProfile = new DisplayProfile();
    const enabled: boolean = sr.enabled && sr.ratioPercent > 100;
    profile.width = width;
    profile.height = height;
    profile.detected = detected;
    profile.scalePercent = scalePercent;
    profile.srEnabled = enabled;
    profile.srRatioPercent = enabled ? sr.ratioPercent : 0;
    profile.sessionWidth = enabled ? SettingsStore.srSessionSize(width, sr.ratioPercent) : width;
    profile.sessionHeight = enabled ? SettingsStore.srSessionSize(height, sr.ratioPercent) : height;
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
