import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  AdvancedProfile, DisplayProfile, SR_BACKEND_FSR, SR_BACKEND_XENGINE, SrProfile, SrSetting
} from '../model/Profiles';
import {
  MoonlightConnection, PROTOCOL_MOONLIGHT, PROTOCOL_RDP, RdpConnection
} from '../model/Connections';
import {
  ML_BITRATE_DEFAULT_KBPS, ML_CODEC_AUTO, ML_COLORSPACE_REC709, ML_RES_FOLLOW_SCREEN,
  ML_SCALE_AUTO, MoonlightProfile
} from '../moonlight/MoonlightModels';
import { DEFAULT_SPEED_SCALE } from './TouchpadWheel';
import { DeviceCapabilities } from './DeviceCapabilities';
import { defaultDisplay } from '../utils/DisplayUtil';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSettings';

const DEFAULT_TOOLBAR_HOVER_DELAY: number = 700;
const DEFAULT_TOOLBAR_HIDE_DELAY: number = 250;
export const DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT: number = 50;

export const PINCH_MODE_WHEEL: number = 0;
export const PINCH_MODE_TOUCH: number = 1;
export const PINCH_MODE_ZOOM: number = 2;
export const PINCH_MODE_NATIVE: number = 3;

export const ML_CURSOR_OFF: number = 0;
export const ML_CURSOR_HIDE_SYSTEM: number = 1;
export const ML_CURSOR_HIDE_HOST: number = 2;
export const DEFAULT_PINCH_TOUCH_DISTANCE: number = 8;
const MAIN_WINDOW_RATIO: number = 0.45;
const SESSION_WINDOW_RATIO: number = 0.67;
const FALLBACK_WIDTH: number = 1920;
const FALLBACK_HEIGHT: number = 1080;

export const SCALE_PRESETS: number[] = [100, 125, 150, 175, 200, 225];

export const AUTO_SCALE_BASE: number = 1600;

export const SR_RATIO_PRESETS: number[] = [125, 150, 175, 200];

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
  toolbarTriggerWidthPercent: number = DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT;
  touchpadScrollSpeed: number = DEFAULT_SPEED_SCALE;
  pinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  touchpadMiddleDoubleTap: boolean = false;
  // Dev fallback: the system scroll engine (transparent Scroll overlay) owns wheel/touchpad scrolling; this turns it off.
  scrollEngineFallback: boolean = false;
  pinchMode: number = PINCH_MODE_WHEEL;
  pinchTouchAngle: number = 30;
  pinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  touchHighRate: boolean = false;
  useRdpCursor: boolean = true;
  reconnectOnForeground: boolean = false;
  winKeySubstitute: number = 0;
  shortcutPassthrough: boolean = false;
  mainWindowSizeEnabled: boolean = false;
  mainWindowWidth: number = 0;
  mainWindowHeight: number = 0;
  sessionWindowSizeEnabled: boolean = false;
  sessionWindowWidth: number = 0;
  sessionWindowHeight: number = 0;
  sessionFullscreen: boolean = false;
  autoHideMainWindow: boolean = false;
  resolutionAuto: boolean = true;
  manualWidth: number = 0;
  manualHeight: number = 0;
  scaleAuto: boolean = true;
  manualScalePercent: number = 100;
  enableAudio: boolean = true;
  ignoreCertificate: boolean = true;
  srEnabled: boolean = false;
  srRatioPercent: number = SR_RATIO_PRESETS[0];
  srBackend: number = SR_BACKEND_XENGINE;
  developerMode: boolean = false;
  gfxCapture: boolean = false;
  mlFps: number = 60;
  mlBitrateAuto: boolean = true;
  mlBitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  mlCodec: number = ML_CODEC_AUTO;
  mlColorSpace: number = ML_COLORSPACE_REC709;
  mlSrEnabled: boolean = false;
  mlSrRatioPercent: number = SR_RATIO_PRESETS[0];
  mlSrBackend: number = SR_BACKEND_XENGINE;
  mlResMode: number = ML_RES_FOLLOW_SCREEN;
  mlWidth: number = 0;
  mlHeight: number = 0;
  mlScaleMode: number = ML_SCALE_AUTO;
  mlScalePercent: number = 100;
  lastNewProtocol: number = PROTOCOL_RDP;
  lastSettingsTab: number = -1;
  wolHelpShown: boolean = false;
  mlTouchpadScrollSpeed: number = DEFAULT_SPEED_SCALE;
  // Off: the host precision-touchpad channel carries touchpad scroll; on: system scroll component + wheel.
  mlTouchpadScrollAlwaysEngine: boolean = false;
  mlPinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  mlTouchpadMiddleDoubleTap: boolean = false;
  mlPinchMode: number = PINCH_MODE_WHEEL;
  mlPinchTouchAngle: number = 30;
  mlPinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  mlCursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  // HIDE_SYSTEM only: the host cursor owns its position and Windows applies its own pointer curve.
  mlRelativeMouse: boolean = false;
  // Relative mode fine-tune on top of the host scale (see services/input/RelativeUnits.ets).
  mlRelativeSpeed: number = DEFAULT_SPEED_SCALE;
  mlLowLatencyAudio: boolean = true;
  mlReconnectOnForeground: boolean = false;
  mlWinKeySubstitute: number = 0;
  mlShortcutPassthrough: boolean = false;
  mlVmouseHintShown: boolean = false;
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

  static defaults(): AppSettings {
    const settings: AppSettings = new AppSettings();
    settings.toolbarHoverDelay = DEFAULT_TOOLBAR_HOVER_DELAY;
    settings.toolbarHideDelay = DEFAULT_TOOLBAR_HIDE_DELAY;
    settings.touchpadScrollSpeed = DEFAULT_SPEED_SCALE;
    settings.pinchZoomSpeed = DEFAULT_SPEED_SCALE;
    settings.pinchMode = PINCH_MODE_WHEEL;
    const disp: display.Display | undefined = defaultDisplay();
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
      settings.toolbarTriggerWidthPercent =
        SettingsStore.readNumber(store, 'toolbarTriggerWidthPercent',
          settings.toolbarTriggerWidthPercent);
      settings.touchpadScrollSpeed =
        SettingsStore.readNumber(store, 'touchpadScrollSpeed', settings.touchpadScrollSpeed);
      settings.scrollEngineFallback = SettingsStore.readBoolean(store,
        'scrollEngineFallback', settings.scrollEngineFallback);
      settings.pinchZoomSpeed =
        SettingsStore.readNumber(store, 'pinchZoomSpeed', settings.pinchZoomSpeed);
      const legacyPinchTouch: boolean = SettingsStore.readBoolean(store, 'pinchAsTouch', false);
      settings.touchpadMiddleDoubleTap = SettingsStore.readBoolean(store,
        'touchpadMiddleDoubleTap', settings.touchpadMiddleDoubleTap);
      settings.pinchMode = SettingsStore.readNumber(store, 'pinchMode',
        legacyPinchTouch ? PINCH_MODE_TOUCH : PINCH_MODE_WHEEL);
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
      settings.shortcutPassthrough =
        SettingsStore.readBoolean(store, 'shortcutPassthrough', settings.shortcutPassthrough);
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
      settings.ignoreCertificate =
        SettingsStore.readBoolean(store, 'ignoreCertificate', settings.ignoreCertificate);
      settings.srEnabled = SettingsStore.readBoolean(store, 'srEnabled', settings.srEnabled);
      settings.srRatioPercent =
        SettingsStore.readNumber(store, 'srRatioPercent', settings.srRatioPercent);
      settings.srBackend = SettingsStore.readNumber(store, 'srBackend', settings.srBackend);
      settings.developerMode =
        SettingsStore.readBoolean(store, 'developerMode', settings.developerMode);
      settings.gfxCapture = SettingsStore.readBoolean(store, 'gfxCapture', settings.gfxCapture);
      settings.mlFps = SettingsStore.readNumber(store, 'mlFps', settings.mlFps);
      settings.mlBitrateKbps =
        SettingsStore.readNumber(store, 'mlBitrateKbps', settings.mlBitrateKbps);
      settings.mlBitrateAuto =
        SettingsStore.readBoolean(store, 'mlBitrateAuto', settings.mlBitrateAuto);
      settings.mlCodec = SettingsStore.readNumber(store, 'mlCodec', settings.mlCodec);
      settings.mlColorSpace =
        SettingsStore.readNumber(store, 'mlColorSpace', settings.mlColorSpace);
      settings.mlSrEnabled = SettingsStore.readBoolean(store, 'mlSrEnabled', settings.mlSrEnabled);
      settings.mlSrRatioPercent =
        SettingsStore.readNumber(store, 'mlSrRatioPercent', settings.mlSrRatioPercent);
      settings.mlSrBackend =
        SettingsStore.readNumber(store, 'mlSrBackend', settings.mlSrBackend);
      settings.mlResMode = SettingsStore.readNumber(store, 'mlResMode', settings.mlResMode);
      settings.mlWidth = SettingsStore.readNumber(store, 'mlWidth', settings.mlWidth);
      settings.mlHeight = SettingsStore.readNumber(store, 'mlHeight', settings.mlHeight);
      settings.mlScaleMode =
        SettingsStore.readNumber(store, 'mlScaleMode', settings.mlScaleMode);
      settings.mlScalePercent =
        SettingsStore.readNumber(store, 'mlScalePercent', settings.mlScalePercent);
      settings.lastNewProtocol =
        SettingsStore.readNumber(store, 'lastNewProtocol', settings.lastNewProtocol);
      settings.lastSettingsTab =
        SettingsStore.readNumber(store, 'lastSettingsTab', settings.lastSettingsTab);
      settings.wolHelpShown =
        SettingsStore.readBoolean(store, 'wolHelpShown', settings.wolHelpShown);
      settings.mlTouchpadScrollSpeed = SettingsStore.readNumber(store, 'mlTouchpadScrollSpeed',
        settings.mlTouchpadScrollSpeed);
      settings.mlTouchpadScrollAlwaysEngine = SettingsStore.readBoolean(store,
        'mlTouchpadScrollAlwaysEngine', settings.mlTouchpadScrollAlwaysEngine);
      settings.mlPinchZoomSpeed =
        SettingsStore.readNumber(store, 'mlPinchZoomSpeed', settings.mlPinchZoomSpeed);
      settings.mlTouchpadMiddleDoubleTap = SettingsStore.readBoolean(store,
        'mlTouchpadMiddleDoubleTap', settings.mlTouchpadMiddleDoubleTap);
      settings.mlPinchMode = SettingsStore.readNumber(store, 'mlPinchMode', settings.mlPinchMode);
      settings.mlPinchTouchAngle =
        SettingsStore.readNumber(store, 'mlPinchTouchAngle', settings.mlPinchTouchAngle);
      settings.mlPinchTouchDistance =
        SettingsStore.readNumber(store, 'mlPinchTouchDistance', settings.mlPinchTouchDistance);
      settings.mlCursorMode =
        SettingsStore.readNumber(store, 'mlCursorMode', settings.mlCursorMode);
      settings.mlRelativeMouse = SettingsStore.readBoolean(store, 'mlRelativeMouse',
        settings.mlRelativeMouse);
      settings.mlRelativeSpeed = SettingsStore.readNumber(store, 'mlRelativeSpeed',
        settings.mlRelativeSpeed);
      settings.mlLowLatencyAudio = SettingsStore.readBoolean(store, 'mlLowLatencyAudio',
        settings.mlLowLatencyAudio);
      settings.mlReconnectOnForeground = SettingsStore.readBoolean(store,
        'mlReconnectOnForeground', settings.mlReconnectOnForeground);
      settings.mlWinKeySubstitute =
        SettingsStore.readNumber(store, 'mlWinKeySubstitute', settings.mlWinKeySubstitute);
      settings.mlShortcutPassthrough = SettingsStore.readBoolean(store,
        'mlShortcutPassthrough', settings.mlShortcutPassthrough);
      settings.mlVmouseHintShown = SettingsStore.readBoolean(store, 'mlVmouseHintShown',
        settings.mlVmouseHintShown);
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
      store.putSync('toolbarTriggerWidthPercent', settings.toolbarTriggerWidthPercent);
      store.putSync('touchpadScrollSpeed', settings.touchpadScrollSpeed);
      store.putSync('scrollEngineFallback', settings.scrollEngineFallback);
      store.putSync('pinchZoomSpeed', settings.pinchZoomSpeed);
      store.putSync('touchpadMiddleDoubleTap', settings.touchpadMiddleDoubleTap);
      store.putSync('pinchMode', settings.pinchMode);
      store.putSync('pinchTouchAngle', settings.pinchTouchAngle);
      store.putSync('pinchTouchDistance', settings.pinchTouchDistance);
      store.putSync('touchHighRate', settings.touchHighRate);
      store.putSync('useRdpCursor', settings.useRdpCursor);
      store.putSync('reconnectOnForeground', settings.reconnectOnForeground);
      store.putSync('winKeySubstitute', settings.winKeySubstitute);
      store.putSync('shortcutPassthrough', settings.shortcutPassthrough);
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
      store.putSync('ignoreCertificate', settings.ignoreCertificate);
      store.putSync('srEnabled', settings.srEnabled);
      store.putSync('srRatioPercent', settings.srRatioPercent);
      store.putSync('srBackend', settings.srBackend);
      store.putSync('developerMode', settings.developerMode);
      store.putSync('gfxCapture', settings.gfxCapture);
      store.putSync('mlFps', settings.mlFps);
      store.putSync('mlBitrateKbps', settings.mlBitrateKbps);
      store.putSync('mlBitrateAuto', settings.mlBitrateAuto);
      store.putSync('mlCodec', settings.mlCodec);
      store.putSync('mlColorSpace', settings.mlColorSpace);
      store.putSync('mlSrEnabled', settings.mlSrEnabled);
      store.putSync('mlSrRatioPercent', settings.mlSrRatioPercent);
      store.putSync('mlSrBackend', settings.mlSrBackend);
      store.putSync('mlResMode', settings.mlResMode);
      store.putSync('mlWidth', settings.mlWidth);
      store.putSync('mlHeight', settings.mlHeight);
      store.putSync('mlScaleMode', settings.mlScaleMode);
      store.putSync('mlScalePercent', settings.mlScalePercent);
      store.putSync('lastNewProtocol', settings.lastNewProtocol);
      store.putSync('lastSettingsTab', settings.lastSettingsTab);
      store.putSync('wolHelpShown', settings.wolHelpShown);
      store.putSync('mlTouchpadScrollSpeed', settings.mlTouchpadScrollSpeed);
      store.putSync('mlTouchpadScrollAlwaysEngine', settings.mlTouchpadScrollAlwaysEngine);
      store.putSync('mlPinchZoomSpeed', settings.mlPinchZoomSpeed);
      store.putSync('mlTouchpadMiddleDoubleTap', settings.mlTouchpadMiddleDoubleTap);
      store.putSync('mlPinchMode', settings.mlPinchMode);
      store.putSync('mlPinchTouchAngle', settings.mlPinchTouchAngle);
      store.putSync('mlPinchTouchDistance', settings.mlPinchTouchDistance);
      store.putSync('mlCursorMode', settings.mlCursorMode);
      store.putSync('mlRelativeMouse', settings.mlRelativeMouse);
      store.putSync('mlRelativeSpeed', settings.mlRelativeSpeed);
      store.putSync('mlLowLatencyAudio', settings.mlLowLatencyAudio);
      store.putSync('mlReconnectOnForeground', settings.mlReconnectOnForeground);
      store.putSync('mlWinKeySubstitute', settings.mlWinKeySubstitute);
      store.putSync('mlShortcutPassthrough', settings.mlShortcutPassthrough);
      store.putSync('mlVmouseHintShown', settings.mlVmouseHintShown);
      store.flush();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'save settings failed: %{public}s', JSON.stringify(error));
    }
  }

  static detectedDisplay(): DisplayProfile {
    const profile: DisplayProfile = new DisplayProfile();
    const disp: display.Display | undefined = defaultDisplay();
    if (disp !== undefined && disp.width > 0 && disp.height > 0) {
      profile.width = disp.width;
      profile.height = disp.height;
      profile.detected = true;
    }
    return profile;
  }

  static scalePresetForResolution(width: number, height: number): number {
    const longest: number = Math.max(width, height);
    return SettingsStore.nearestScalePreset(Math.round(longest * 100 / AUTO_SCALE_BASE));
  }

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

  static srSessionSize(output: number, ratioPercent: number): number {
    if (output <= 0 || ratioPercent <= 100) {
      return output;
    }
    const scaled: number = Math.round(output * 100 / ratioPercent);
    const even: number = scaled - (scaled % 2);
    return even > 0 ? even : 2;
  }

  static autoScalePercent(outputWidth: number, outputHeight: number,
    srEnabled: boolean, ratioPercent: number): number {
    if (outputWidth <= 0 || outputHeight <= 0) {
      return SCALE_PRESETS[0];
    }
    const base: number = SettingsStore.scalePresetForResolution(outputWidth, outputHeight);
    if (!srEnabled || ratioPercent <= 100) {
      return base;
    }
    return SettingsStore.nearestScalePreset(Math.round(base * 100 / ratioPercent));
  }

  static globalSrProfile(protocol: number): SrProfile {
    const settings: AppSettings = SettingsStore.load();
    const moonlight: boolean = protocol === PROTOCOL_MOONLIGHT;
    const profile: SrProfile = new SrProfile();
    profile.enabled = moonlight ? settings.mlSrEnabled : settings.srEnabled;
    profile.ratioPercent = SettingsStore.snapSrRatioPreset(moonlight ? settings.mlSrRatioPercent
                                                                     : settings.srRatioPercent);
    profile.backend = moonlight ? settings.mlSrBackend : settings.srBackend;
    return SettingsStore.gateSr(profile, protocol);
  }

  // 超分 for a protocol: the connection's own values when it overrides, else that protocol's globals,
  // then gated by what the device can actually do (Moonlight also needs the video import).
  static resolveSr(protocol: number, setting: SrSetting): SrProfile {
    if (setting.useGlobalSr) {
      return SettingsStore.globalSrProfile(protocol);
    }
    const profile: SrProfile = new SrProfile();
    profile.enabled = setting.srEnabled;
    profile.ratioPercent = SettingsStore.snapSrRatioPreset(setting.srRatioPercent);
    profile.backend = setting.srBackend;
    return SettingsStore.gateSr(profile, protocol);
  }

  private static gateSr(profile: SrProfile, protocol: number): SrProfile {
    if (profile.enabled && !SettingsStore.srBackendUsable(profile.backend, protocol)) {
      const other: number = profile.backend === SR_BACKEND_XENGINE ? SR_BACKEND_FSR
                                                                   : SR_BACKEND_XENGINE;
      if (SettingsStore.srBackendUsable(other, protocol)) {
        profile.backend = other;
      } else {
        profile.enabled = false;
      }
    }
    if (profile.ratioPercent <= 100) {
      profile.ratioPercent = SR_RATIO_PRESETS[0];
    }
    return profile;
  }

  private static srBackendToken(backend: number): string {
    return backend === SR_BACKEND_FSR ? 'fsr' : 'xengine';
  }

  static srBackendUsable(backend: number, protocol: number = PROTOCOL_RDP): boolean {
    if (protocol === PROTOCOL_MOONLIGHT && !DeviceCapabilities.srVideoImport()) {
      return false;
    }
    return DeviceCapabilities.srBackends().indexOf(SettingsStore.srBackendToken(backend)) >= 0;
  }

  static superResolutionUsable(protocol: number = PROTOCOL_RDP): boolean {
    return SettingsStore.srBackendUsable(SR_BACKEND_XENGINE, protocol)
      || SettingsStore.srBackendUsable(SR_BACKEND_FSR, protocol);
  }

  static globalDisplayProfile(): DisplayProfile {
    const settings: AppSettings = SettingsStore.load();
    return SettingsStore.profileFor(settings, SettingsStore.globalSrProfile(PROTOCOL_RDP));
  }

  static resolveDisplay(conn: RdpConnection): DisplayProfile {
    const settings: AppSettings = SettingsStore.load();
    const sr: SrProfile = SettingsStore.resolveSr(PROTOCOL_RDP, conn);
    const global: DisplayProfile = SettingsStore.profileFor(settings, sr);
    if (conn.useGlobalDisplay) {
      return global;
    }
    let width: number = 0;
    let height: number = 0;
    let detected: boolean = global.detected;
    if (conn.resolutionAuto) {
      const screen: DisplayProfile = SettingsStore.detectedDisplay();
      if (screen.valid()) {
        width = screen.width;
        height = screen.height;
        detected = true;
      } else {
        width = global.width;
        height = global.height;
      }
    } else {
      const hasOwnSize: boolean = conn.width > 0 && conn.height > 0;
      width = hasOwnSize ? conn.width : global.width;
      height = hasOwnSize ? conn.height : global.height;
    }
    const autoScale: number = SettingsStore.autoScalePercent(width, height,
      sr.enabled && sr.ratioPercent > 100, sr.ratioPercent);
    const scale: number = conn.scaleAuto || conn.scalePercent <= 0 ? autoScale : conn.scalePercent;
    return SettingsStore.makeProfile(width, height, detected, scale, sr);
  }

  static globalAdvancedProfile(): AdvancedProfile {
    return SettingsStore.advancedFor(SettingsStore.load());
  }

  static resolveAdvanced(conn: RdpConnection): AdvancedProfile {
    if (conn.useGlobalAdvanced) {
      return SettingsStore.globalAdvancedProfile();
    }
    const profile: AdvancedProfile = new AdvancedProfile();
    profile.enableAudio = conn.enableAudio;
    profile.ignoreCertificate = conn.ignoreCertificate;
    return profile;
  }

  static globalStreamProfile(): MoonlightProfile {
    const settings: AppSettings = SettingsStore.load();
    const profile: MoonlightProfile = new MoonlightProfile();
    profile.resMode = settings.mlResMode;
    profile.width = settings.mlWidth;
    profile.height = settings.mlHeight;
    profile.fps = settings.mlFps;
    profile.bitrateAuto = settings.mlBitrateAuto;
    profile.bitrateKbps = settings.mlBitrateKbps;
    profile.codec = settings.mlCodec;
    profile.colorSpace = settings.mlColorSpace;
    profile.scaleMode = settings.mlScaleMode;
    profile.scalePercent = settings.mlScalePercent;
    return profile;
  }

  static resolveStream(conn: MoonlightConnection): MoonlightProfile {
    if (conn.useGlobalStream) {
      return SettingsStore.globalStreamProfile();
    }
    const profile: MoonlightProfile = new MoonlightProfile();
    profile.resMode = conn.mlResMode;
    profile.width = conn.mlWidth;
    profile.height = conn.mlHeight;
    profile.fps = conn.mlFps;
    profile.bitrateAuto = conn.mlBitrateAuto;
    profile.bitrateKbps = conn.mlBitrateKbps;
    profile.codec = conn.mlCodec;
    profile.colorSpace = conn.mlColorSpace;
    profile.scaleMode = conn.mlScaleMode;
    profile.scalePercent = conn.mlScalePercent;
    return profile;
  }

  static lastNewProtocol(): number {
    return SettingsStore.load().lastNewProtocol;
  }

  static rememberNewProtocol(protocol: number): void {
    const settings: AppSettings = SettingsStore.load();
    settings.lastNewProtocol = protocol;
    SettingsStore.save(settings);
  }

  static lastSettingsTab(): number {
    return SettingsStore.load().lastSettingsTab;
  }

  static rememberSettingsTab(tab: number): void {
    const settings: AppSettings = SettingsStore.load();
    settings.lastSettingsTab = tab;
    SettingsStore.save(settings);
  }

  static rememberWolHelpShown(): void {
    const settings: AppSettings = SettingsStore.load();
    settings.wolHelpShown = true;
    SettingsStore.save(settings);
  }

  static rememberVmouseHintShown(): void {
    const settings: AppSettings = SettingsStore.load();
    settings.mlVmouseHintShown = true;
    SettingsStore.save(settings);
  }

  private static advancedFor(settings: AppSettings): AdvancedProfile {
    const profile: AdvancedProfile = new AdvancedProfile();
    profile.enableAudio = settings.enableAudio;
    profile.ignoreCertificate = settings.ignoreCertificate;
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
    const defaultScale: number =
      SettingsStore.autoScalePercent(width, height, enabled, sr.ratioPercent);
    const scale: number = (!settings.scaleAuto && settings.manualScalePercent > 0)
      ? settings.manualScalePercent : defaultScale;
    return SettingsStore.makeProfile(width, height, detected, scale, sr);
  }

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
    profile.srBackend = enabled ? sr.backend : SR_BACKEND_XENGINE;
    profile.sessionWidth = enabled ? SettingsStore.srSessionSize(width, sr.ratioPercent) : width;
    profile.sessionHeight = enabled ? SettingsStore.srSessionSize(height, sr.ratioPercent) : height;
    return profile;
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
