import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  AdvancedProfile, DisplayProfile, SR_BACKEND_FSR, SR_BACKEND_XENGINE, SrProfile
} from '../model/RdpModels';
import {
  ML_BITRATE_DEFAULT_KBPS, ML_CODEC_AUTO, ML_RES_FOLLOW_SCREEN, MoonlightConnection, MoonlightProfile,
  PROTOCOL_RDP, RdpConnection
} from '../model/Connections';
import { DEFAULT_SPEED_SCALE } from './TouchpadWheel';
import { DeviceCapabilities } from './DeviceCapabilities';
import { defaultDisplay } from '../utils/DisplayUtil';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSettings';

const DEFAULT_TOOLBAR_HOVER_DELAY: number = 700;
const DEFAULT_TOOLBAR_HIDE_DELAY: number = 250;

export const PINCH_MODE_WHEEL: number = 0;
export const PINCH_MODE_TOUCH: number = 1;
export const PINCH_MODE_ZOOM: number = 2;
// Moonlight only: forward to the host's native precision touchpad (foundation-sunshine).
export const PINCH_MODE_NATIVE: number = 3;

// Moonlight 主机通常把指针合成进视频，本地系统光标会重影；HIDE_HOST 的本机光标
// 模式需要 foundation-sunshine，否则回退 OFF。
export const ML_CURSOR_OFF: number = 0;
export const ML_CURSOR_HIDE_SYSTEM: number = 1;
export const ML_CURSOR_HIDE_HOST: number = 2;
// 共享默认值（占桌面短边百分比），让设置占位与会话侧兜底不会与存储默认脱节。
export const DEFAULT_PINCH_TOUCH_DISTANCE: number = 8;
const MAIN_WINDOW_RATIO: number = 0.45;
const SESSION_WINDOW_RATIO: number = 0.67;
const FALLBACK_WIDTH: number = 1920;
const FALLBACK_HEIGHT: number = 1080;

// Windows only applies a fixed set of DPI scaling presets; other values can
// cause rendering problems, so the scale is always snapped to one of these.
export const SCALE_PRESETS: number[] = [100, 125, 150, 175, 200, 225];

// 自动缩放中映射到 100% 的桌面最长边；1600 让常见桌面落在中间档位而非两端。
export const AUTO_SCALE_BASE: number = 1600;

// 超分倍率档位（百分比）。平台对 GPU 空域超分*推荐*不超过 1.5×；更大倍率省带宽
// 但文字边缘开始振铃（UI 已标注），API 本身无硬上限。
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
  touchpadScrollSpeed: number = DEFAULT_SPEED_SCALE;
  pinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  // 触控板双指双击发送鼠标中键（单次双指轻触仍是右键）。双击窗口从第一次轻触的按下起算，
  // 所以单次右键的判定延迟约 300ms；按下后按住不放则判为实体右键（拖动），立即按下、不受此延迟。
  touchpadMiddleDoubleTap: boolean = false;
  pinchMode: number = PINCH_MODE_WHEEL;
  // 合成触点对的倾角（度）：ArkUI 报不出真实手指角度，用此值代替手的朝向。
  pinchTouchAngle: number = 30;
  pinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  // 以 8ms 高刷新率转发 RDPEI 触摸，替代 FreeRDP 默认 ~20ms（50Hz）的合并。
  touchHighRate: boolean = false;
  useRdpCursor: boolean = true;
  // 回到前台发现会话已掉线时自动重连：系统可能在后台冻结并掐断 socket，会话看似
  // 在线实则已死；这是回前台的兜底，不是保活。
  reconnectOnForeground: boolean = false;
  // 映射为远端 Win/Meta 的 KeyCode（0 = 禁用）：shell 占着真 Win 键，用替身让 Win 组合键可用。
  winKeySubstitute: number = 0;
  // 快捷键穿透：会话窗口聚焦时接管键盘让 Win / Alt+Tab / Esc 直达远端
  // （ACL INTERCEPT_INPUT_EVENT）；会同时压制本机系统快捷键，故默认关。
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
  // 超分：会话按输出÷倍率出画、Vulkan 呈现器上采样回来；会改变请求给服务器的
  // 分辨率，故默认关（opt-in）。
  srEnabled: boolean = false;
  srRatioPercent: number = SR_RATIO_PRESETS[0];
  srBackend: number = SR_BACKEND_XENGINE;
  // 关 = 只用 GLES 呈现器、完全不碰 Vulkan（见 doc_agent/settings-and-storage.md §2）。
  hardwareAccel: boolean = true;
  developerMode: boolean = false;
  gfxCapture: boolean = false;
  mlFps: number = 60;
  mlBitrateKbps: number = ML_BITRATE_DEFAULT_KBPS;
  mlCodec: number = ML_CODEC_AUTO;
  mlResMode: number = ML_RES_FOLLOW_SCREEN;
  mlWidth: number = 0;
  mlHeight: number = 0;
  lastNewProtocol: number = PROTOCOL_RDP;
  // Moonlight 自有一份交互设置（触控板/捏合/键盘/前台重连）：两协议各存各的值，
  // 只共享设置 UI 代码；窗口尺寸、工具栏延迟与开发者选项是应用级单一值。
  mlTouchpadScrollSpeed: number = DEFAULT_SPEED_SCALE;
  mlPinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  mlTouchpadMiddleDoubleTap: boolean = false;
  mlPinchMode: number = PINCH_MODE_WHEEL;
  mlPinchTouchAngle: number = 30;
  mlPinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  mlCursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  mlReconnectOnForeground: boolean = false;
  mlWinKeySubstitute: number = 0;
  mlShortcutPassthrough: boolean = false;
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

  // 屏幕取不到时禁用窗口尺寸调整，而不是回退到一个编造的分辨率。
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
      settings.touchpadScrollSpeed =
        SettingsStore.readNumber(store, 'touchpadScrollSpeed', settings.touchpadScrollSpeed);
      settings.pinchZoomSpeed =
        SettingsStore.readNumber(store, 'pinchZoomSpeed', settings.pinchZoomSpeed);
      // 旧的两档 pinchAsTouch 布尔映射到三档模式，避免丢掉存量设置。
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
      settings.hardwareAccel =
        SettingsStore.readBoolean(store, 'hardwareAccel', settings.hardwareAccel);
      settings.developerMode =
        SettingsStore.readBoolean(store, 'developerMode', settings.developerMode);
      settings.gfxCapture = SettingsStore.readBoolean(store, 'gfxCapture', settings.gfxCapture);
      settings.mlFps = SettingsStore.readNumber(store, 'mlFps', settings.mlFps);
      settings.mlBitrateKbps =
        SettingsStore.readNumber(store, 'mlBitrateKbps', settings.mlBitrateKbps);
      settings.mlCodec = SettingsStore.readNumber(store, 'mlCodec', settings.mlCodec);
      settings.mlResMode = SettingsStore.readNumber(store, 'mlResMode', settings.mlResMode);
      settings.mlWidth = SettingsStore.readNumber(store, 'mlWidth', settings.mlWidth);
      settings.mlHeight = SettingsStore.readNumber(store, 'mlHeight', settings.mlHeight);
      settings.lastNewProtocol =
        SettingsStore.readNumber(store, 'lastNewProtocol', settings.lastNewProtocol);
      settings.mlTouchpadScrollSpeed = SettingsStore.readNumber(store, 'mlTouchpadScrollSpeed',
        settings.mlTouchpadScrollSpeed);
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
      settings.mlReconnectOnForeground = SettingsStore.readBoolean(store,
        'mlReconnectOnForeground', settings.mlReconnectOnForeground);
      settings.mlWinKeySubstitute =
        SettingsStore.readNumber(store, 'mlWinKeySubstitute', settings.mlWinKeySubstitute);
      settings.mlShortcutPassthrough = SettingsStore.readBoolean(store,
        'mlShortcutPassthrough', settings.mlShortcutPassthrough);
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
      store.putSync('hardwareAccel', settings.hardwareAccel);
      store.putSync('developerMode', settings.developerMode);
      store.putSync('gfxCapture', settings.gfxCapture);
      store.putSync('mlFps', settings.mlFps);
      store.putSync('mlBitrateKbps', settings.mlBitrateKbps);
      store.putSync('mlCodec', settings.mlCodec);
      store.putSync('mlResMode', settings.mlResMode);
      store.putSync('mlWidth', settings.mlWidth);
      store.putSync('mlHeight', settings.mlHeight);
      store.putSync('lastNewProtocol', settings.lastNewProtocol);
      store.putSync('mlTouchpadScrollSpeed', settings.mlTouchpadScrollSpeed);
      store.putSync('mlPinchZoomSpeed', settings.mlPinchZoomSpeed);
      store.putSync('mlTouchpadMiddleDoubleTap', settings.mlTouchpadMiddleDoubleTap);
      store.putSync('mlPinchMode', settings.mlPinchMode);
      store.putSync('mlPinchTouchAngle', settings.mlPinchTouchAngle);
      store.putSync('mlPinchTouchDistance', settings.mlPinchTouchDistance);
      store.putSync('mlCursorMode', settings.mlCursorMode);
      store.putSync('mlReconnectOnForeground', settings.mlReconnectOnForeground);
      store.putSync('mlWinKeySubstitute', settings.mlWinKeySubstitute);
      store.putSync('mlShortcutPassthrough', settings.mlShortcutPassthrough);
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

  // 只看远端分辨率（本机屏幕与密度不参与），同一分辨率在任何机器上缩放一致。
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

  // 取偶数像素：奇数桌面尺寸在部分服务器上渲染异常。
  static srSessionSize(output: number, ratioPercent: number): number {
    if (output <= 0 || ratioPercent <= 100) {
      return output;
    }
    const scaled: number = Math.round(output * 100 / ratioPercent);
    const even: number = scaled - (scaled % 2);
    return even > 0 ? even : 2;
  }

  // 超分时会话按输出÷倍率渲染再上采样回来，缩放因子同步除以倍率，保持远端 UI
  // 物理大小不变（且不低于第一个 Windows 档位）。
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

  static globalSrProfile(): SrProfile {
    const settings: AppSettings = SettingsStore.load();
    const profile: SrProfile = new SrProfile();
    profile.enabled = settings.srEnabled;
    profile.ratioPercent = SettingsStore.snapSrRatioPreset(settings.srRatioPercent);
    profile.backend = settings.srBackend;
    return SettingsStore.gateSr(profile);
  }

  // 设备做不了时强制关（或换到另一后端）：绝不协商出一个无法上采样回去的更低会话分辨率。
  static resolveSr(conn: RdpConnection): SrProfile {
    const settings: AppSettings = SettingsStore.load();
    const profile: SrProfile = new SrProfile();
    if (conn.useGlobalSr) {
      profile.enabled = settings.srEnabled;
      profile.ratioPercent = SettingsStore.snapSrRatioPreset(settings.srRatioPercent);
      profile.backend = settings.srBackend;
    } else {
      profile.enabled = conn.srEnabled;
      profile.ratioPercent = SettingsStore.snapSrRatioPreset(conn.srRatioPercent);
      profile.backend = conn.srBackend;
    }
    return SettingsStore.gateSr(profile);
  }

  private static gateSr(profile: SrProfile): SrProfile {
    if (profile.enabled && !SettingsStore.srBackendUsable(profile.backend)) {
      // 所选后端在此不可用（换了设备、硬件加速关闭或构建不含）：优先换另一后端，
      // 都不行就关超分，而不是协商一个无法上采样回去的低会话分辨率。
      const other: number = profile.backend === SR_BACKEND_XENGINE ? SR_BACKEND_FSR
                                                                   : SR_BACKEND_XENGINE;
      if (SettingsStore.srBackendUsable(other)) {
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

  // 原生能力探测回报的 backend token。
  private static srBackendToken(backend: number): string {
    return backend === SR_BACKEND_FSR ? 'fsr' : 'xengine';
  }

  // 超分依附 Vulkan 呈现器，故需硬件加速可用；FSR 别无要求，XEngine 还需设备的
  // XEG_spatial_upscale 特性。
  static srBackendUsable(backend: number): boolean {
    return SettingsStore.srBackendUsableFor(SettingsStore.load(), backend);
  }

  // 配置导入在保存前校验文件，拿不到已存值，故要显式传入 settings。
  static srBackendUsableFor(settings: AppSettings, backend: number): boolean {
    if (!settings.hardwareAccel || !DeviceCapabilities.hardwareAccel().supported) {
      return false;
    }
    return DeviceCapabilities.srBackends().indexOf(SettingsStore.srBackendToken(backend)) >= 0;
  }

  static superResolutionUsable(): boolean {
    return SettingsStore.srBackendUsable(SR_BACKEND_XENGINE)
      || SettingsStore.srBackendUsable(SR_BACKEND_FSR);
  }

  static globalDisplayProfile(): DisplayProfile {
    const settings: AppSettings = SettingsStore.load();
    return SettingsStore.profileFor(settings, SettingsStore.globalSrProfile());
  }

  // 覆盖模式下分辨率与缩放各有「自动」；旧记录没有这两个标志位，停留在其存储值上。
  static resolveDisplay(conn: RdpConnection): DisplayProfile {
    const settings: AppSettings = SettingsStore.load();
    const sr: SrProfile = SettingsStore.resolveSr(conn);
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
    // 自动缩放按*本连接*的分辨率推导（与全局的可能不同）。
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
    profile.bitrateKbps = settings.mlBitrateKbps;
    profile.codec = settings.mlCodec;
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
    profile.bitrateKbps = conn.mlBitrateKbps;
    profile.codec = conn.mlCodec;
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
