/*
 * HmRdp - global settings.
 */
import { common } from '@kit.AbilityKit';
import { DisplayProfile, ML_BITRATE_DEFAULT_KBPS,
  ML_BITRATE_MAX_KBPS, ML_BITRATE_MIN_KBPS, ML_CODEC_AUTO, ML_CODEC_AV1, ML_CODEC_H264,
  ML_CODEC_HEVC, ML_FPS_PRESETS, ML_RES_FIXED, ML_RES_FOLLOW_SCREEN,
  PROTOCOL_MOONLIGHT, PROTOCOL_RDP, SR_BACKEND_FSR, SR_BACKEND_XENGINE } from '../model/RdpModels';
import { ConfigTransfer, ConfigTransferResult } from '../services/ConfigTransfer';
import { Capability, CODEC_SUPPORT_NONE, DeviceCapabilities } from '../services/DeviceCapabilities';
import { RdpNative } from '../services/RdpNative';
import { AppSettings, DEFAULT_PINCH_TOUCH_DISTANCE, ML_CURSOR_HIDE_HOST, ML_CURSOR_HIDE_SYSTEM,
  ML_CURSOR_OFF, PINCH_MODE_TOUCH, PINCH_MODE_WHEEL,
  PINCH_MODE_ZOOM, SCALE_PRESETS, SR_RATIO_PRESETS,
  SettingsStore, srRatioLabel } from '../services/SettingsStore';
import { DEFAULT_SPEED_SCALE, MAX_SPEED_SCALE, MIN_SPEED_SCALE } from '../services/TouchpadWheel';
import { showConfirm } from '../utils/UiUtil';
import { KeyMapper } from '../utils/KeyMapper';
import { defaultDisplay } from '../utils/DisplayUtil';
import { isScalePreset, MAX_DESKTOP_SIZE, parsedTextValue, parseScaleValue, parseSizeValue } from '../utils/ScaleParse';
import { moonlightCodecSoftware, moonlightCodecSupport, moonlightCodecUsable } from '../utils/MoonlightCodec';
import { TextAction } from '../components/TextAction';
import { ChoiceChip } from '../components/ChoiceChip';
import { HeaderBar } from '../components/HeaderBar';
import { MaterialCard } from '../components/MaterialCard';
import { ProtocolTabs, TAB_COMMON } from '../components/ProtocolTabs';
import { SettingSliderRow } from '../components/SettingSliderRow';
import { SettingToggleRow } from '../components/SettingToggleRow';

// @Builder parameters are only reactive when passed as a single object literal
// (by reference); passing primitives by value would freeze the displayed value.
class SliderItemConfig {
  title: string = '';
  subtitle: string = '';
  value: number = 0;
  min: number = 0;
  max: number = 100;
  step: number = 1;
  unit: string = '';
  // Decimal places used to display the value and to snap slider output.
  precision: number = 0;
  // A disabled slider is shown greyed out with a reason below the subtitle.
  enabled: boolean = true;
  disabledReason: string = '';
  onChange: (v: number) => void = (v: number) => {};
}

// Same reasoning as SliderItemConfig: the builder must receive a single object
// literal (by reference) so a reset / import refreshes the switch.
class ToggleItemConfig {
  title: string = '';
  subtitle: string = '';
  value: boolean = false;
  // A device that does not provide the feature shows the row greyed out with
  // `disabledReason` instead of an active switch.
  enabled: boolean = true;
  disabledReason: string = '';
  onChange: (v: boolean) => void = (v: boolean) => {};
}

@Entry
@Component
struct SettingsPage {
  @State hoverDelay: number = 700;
  @State hideDelay: number = 250;
  @State touchpadScrollSpeed: number = DEFAULT_SPEED_SCALE;
  @State pinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  @State pinchMode: number = PINCH_MODE_WHEEL;
  @State touchpadMiddleDoubleTap: boolean = false;
  @State pinchTouchAngle: number = 30;
  @State pinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  @State touchHighRate: boolean = false;
  @State useRdpCursor: boolean = true;
  @State reconnectOnForeground: boolean = false;
  @State winKeySubstitute: number = 0;
  @State winSubstituteCapturing: boolean = false;
  @State shortcutPassthrough: boolean = false;
  @State resolutionAuto: boolean = true;
  @State manualWidthText: string = '';
  @State manualHeightText: string = '';
  @State scaleAuto: boolean = true;
  @State manualScaleText: string = '100';
  @State detectedText: string = '';
  @State mainSizeEnabled: boolean = false;
  @State sessionSizeEnabled: boolean = false;
  @State sessionFullscreen: boolean = false;
  @State autoHideMainWindow: boolean = false;
  @State audioSupported: boolean = true;
  private audioDisabledReason: string = '';
  @State enableAudio: boolean = true;
  @State ignoreCertificate: boolean = true;
  @State hardwareAccel: boolean = true;
  @State hardwareAccelSupported: boolean = true;
  private hardwareAccelReason: string = '';
  @State srEnabled: boolean = false;
  @State srRatioPercent: number = SR_RATIO_PRESETS[0];
  @State srBackend: number = SR_BACKEND_XENGINE;
  @State srSupported: boolean = true;
  private srReason: string = '';
  // Developer options: reveals the dev-only tools below (and the replay entry /
  // toolbar telemetry elsewhere). Off for a normal install.
  @State developerMode: boolean = false;
  // Dev-only RemoteFX stream capture (doc_agent/gfx-engine.md §6).
  @State gfxCapture: boolean = false;
  // Active settings tab: PROTOCOL_RDP, PROTOCOL_MOONLIGHT or TAB_COMMON. The
  // protocol tabs render the shared interaction section from that protocol's own
  // stored values; the common tab holds the app-level (window / developer) items.
  @State protocol: number = PROTOCOL_RDP;
  // Moonlight stream defaults; each connection can still override them.
  @State mlResMode: number = ML_RES_FOLLOW_SCREEN;
  @State mlWidthText: string = '';
  @State mlHeightText: string = '';
  @State mlFps: number = 60;
  @State mlBitrateText: string = ML_BITRATE_DEFAULT_KBPS.toString();
  @State mlCodec: number = ML_CODEC_AUTO;
  @State mlCursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  // Per-codec decoder support (CODEC_SUPPORT_*) of this device, read once so
  // the chips can grey what cannot run and warn about software-only codecs.
  private mlH264Support: number = CODEC_SUPPORT_NONE;
  private mlHevcSupport: number = CODEC_SUPPORT_NONE;
  private mlAv1Support: number = CODEC_SUPPORT_NONE;
  @State mainWidthText: string = '';
  @State mainHeightText: string = '';
  @State sessionWidthText: string = '';
  @State sessionHeightText: string = '';
  @State transferBusy: boolean = false;
  @State transferMessage: string = '';
  private defaultMainWidth: number = 0;
  private defaultMainHeight: number = 0;
  private defaultSessionWidth: number = 0;
  private defaultSessionHeight: number = 0;
  private detectedWidth: number = 0;
  private detectedHeight: number = 0;

  aboutToAppear(): void {
    this.mlH264Support = DeviceCapabilities.codecSupport(ML_CODEC_H264);
    this.mlHevcSupport = DeviceCapabilities.codecSupport(ML_CODEC_HEVC);
    this.mlAv1Support = DeviceCapabilities.codecSupport(ML_CODEC_AV1);
    const defaults: AppSettings = SettingsStore.defaults();
    this.defaultMainWidth = defaults.mainWindowWidth;
    this.defaultMainHeight = defaults.mainWindowHeight;
    this.defaultSessionWidth = defaults.sessionWindowWidth;
    this.defaultSessionHeight = defaults.sessionWindowHeight;
    const detected: DisplayProfile = SettingsStore.detectedDisplay();
    this.detectedText = detected.valid()
      ? `${detected.width} × ${detected.height}` : '无法获取显示器分辨率';
    this.detectedWidth = detected.width;
    this.detectedHeight = detected.height;
    this.syncFromStore();
  }

  /** Pulls the persisted settings back into the page state (also after import). */
  private syncFromStore(): void {
    const settings: AppSettings = SettingsStore.load();
    const audioCapability: Capability = DeviceCapabilities.audio();
    this.audioSupported = audioCapability.supported;
    this.audioDisabledReason = audioCapability.reason;
    const hwCapability: Capability = DeviceCapabilities.hardwareAccel();
    this.hardwareAccelSupported = hwCapability.supported;
    this.hardwareAccelReason = hwCapability.reason;
    const srCapability: Capability = DeviceCapabilities.superResolution();
    this.srSupported = srCapability.supported;
    this.srReason = srCapability.reason;

    // App-level surfaces: window sizing, the developer section and its tools are
    // the same on every tab, so they load regardless of the active one.
    this.applyWindowSettings(settings);
    this.applyDeveloperSettings(settings);

    if (this.protocol === PROTOCOL_RDP) {
      this.useRdpCursor = settings.useRdpCursor;
      RdpNative.applyRdpCursor(this.useRdpCursor);
      this.resolutionAuto = settings.resolutionAuto;
      this.manualWidthText = this.sizeText(settings.manualWidth);
      this.manualHeightText = this.sizeText(settings.manualHeight);
      this.scaleAuto = settings.scaleAuto;
      // An unsupported device never requests audio; drop a previously stored
      // "on" value so the setting matches the greyed-out switch.
      this.enableAudio = this.audioSupported ? settings.enableAudio : false;
      // An unsupported device cannot present through Vulkan; force the value off so
      // the setting matches the greyed-out switch.
      this.hardwareAccel = this.hardwareAccelSupported ? settings.hardwareAccel : false;
      // 超分 rides on the Vulkan presenter, so it is only offered where both the
      // device and the 硬件加速 setting allow it; a stored "on" value that no longer
      // can apply is dropped, exactly like the audio / hardware-acceleration rows.
      this.srEnabled = (this.hardwareAccelSupported && this.srSupported)
        ? settings.srEnabled : false;
      this.srRatioPercent = SettingsStore.snapSrRatioPreset(settings.srRatioPercent);
      this.srBackend = settings.srBackend;
      this.manualScaleText = settings.manualScalePercent > 0
        ? settings.manualScalePercent.toString() : this.effectiveDefaultScale().toString();
      this.ignoreCertificate = settings.ignoreCertificate;
      RdpNative.applyHardwareAccel(this.hardwareAccel && this.hardwareAccelSupported);
      this.applyInteraction(settings, true);
    } else if (this.protocol === PROTOCOL_MOONLIGHT) {
      this.mlResMode = settings.mlResMode;
      this.mlWidthText = this.sizeText(settings.mlWidth > 0 ? settings.mlWidth : this.detectedWidth);
      this.mlHeightText = this.sizeText(settings.mlHeight > 0
        ? settings.mlHeight : this.detectedHeight);
      this.mlFps = settings.mlFps;
      this.mlBitrateText = settings.mlBitrateKbps.toString();
      this.mlCodec = settings.mlCodec;
      this.mlCursorMode = settings.mlCursorMode;
      this.clampMlCodec();
      this.applyInteraction(settings, false);
    }

    if (this.protocol === PROTOCOL_RDP) {
      if (!this.audioSupported && settings.enableAudio) {
        this.persist();
      }
      if (!this.hardwareAccelSupported && settings.hardwareAccel) {
        this.persist();
      }
      if ((!this.hardwareAccelSupported || !this.srSupported) && settings.srEnabled) {
        this.persist();
      }
    }
    if (!this.developerMode && settings.gfxCapture) {
      this.persist();
    }
    // Keep the native capture in step with what the page now shows: with developer
    // options off it must not be running, whatever the stored value was.
    this.applyGfxCaptureSetting();
  }

  /** Loads the app-level window settings (a single value, shared by both protocols). */
  private applyWindowSettings(settings: AppSettings): void {
    this.mainSizeEnabled = settings.mainWindowSizeEnabled;
    this.mainWidthText = this.sizeText(settings.mainWindowWidth);
    this.mainHeightText = this.sizeText(settings.mainWindowHeight);
    this.autoHideMainWindow = settings.autoHideMainWindow;
    this.sessionSizeEnabled = settings.sessionWindowSizeEnabled;
    this.sessionWidthText = this.sizeText(settings.sessionWindowWidth);
    this.sessionHeightText = this.sizeText(settings.sessionWindowHeight);
    this.sessionFullscreen = settings.sessionFullscreen;
  }

  /** Loads the app-level developer section (gate, its tools and the toolbar delays). */
  private applyDeveloperSettings(settings: AppSettings): void {
    this.developerMode = settings.developerMode;
    // With developer options off, the dev-only tools are not shown and must not
    // keep running: a capture left on from a previous session is switched off.
    this.gfxCapture = this.developerMode ? settings.gfxCapture : false;
    this.touchHighRate = settings.touchHighRate;
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
  }

  /**
   * Loads the shared interaction settings (touchpad / keyboard / foreground
   * reconnect) from the RDP copy or the Moonlight copy. The two protocols keep
   * separate values; only this UI code is shared.
   */
  private applyInteraction(settings: AppSettings, rdp: boolean): void {
    this.touchpadScrollSpeed = rdp ? settings.touchpadScrollSpeed : settings.mlTouchpadScrollSpeed;
    this.pinchZoomSpeed = rdp ? settings.pinchZoomSpeed : settings.mlPinchZoomSpeed;
    this.touchpadMiddleDoubleTap = rdp
      ? settings.touchpadMiddleDoubleTap : settings.mlTouchpadMiddleDoubleTap;
    this.pinchMode = rdp ? settings.pinchMode : settings.mlPinchMode;
    this.pinchTouchAngle = rdp ? settings.pinchTouchAngle : settings.mlPinchTouchAngle;
    this.pinchTouchDistance = rdp ? settings.pinchTouchDistance : settings.mlPinchTouchDistance;
    this.reconnectOnForeground = rdp
      ? settings.reconnectOnForeground : settings.mlReconnectOnForeground;
    this.winKeySubstitute = rdp ? settings.winKeySubstitute : settings.mlWinKeySubstitute;
    this.shortcutPassthrough = rdp ? settings.shortcutPassthrough : settings.mlShortcutPassthrough;
  }

  // Applies the dev capture toggle immediately (before the next session) so the
  // cmd stream is captured from the first frame; a no-op when the value is
  // unchanged on the native side.
  private applyGfxCaptureSetting(): void {
    let dir: string = '';
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx !== undefined) {
        dir = ctx.filesDir;
      }
    } catch (error) {
      dir = '';
    }
    RdpNative.applyGfxCapture(this.gfxCapture, dir);
  }

  private sizeText(value: number): string {
    return value > 0 ? value.toString() : '';
  }

  private parseSize(text: string, fallback: number, max: number): number {
    return parseSizeValue(text, fallback, max);
  }

  private parseScale(text: string, fallback: number): number {
    return parseScaleValue(text, fallback);
  }

  private parsedManualScale(): number {
    return parsedTextValue(this.manualScaleText);
  }

  private isPresetScale(value: number): boolean {
    return isScalePreset(value);
  }

  private effectiveMlBitrate(): number {
    const value: number = Number.parseInt(this.mlBitrateText, 10);
    if (Number.isNaN(value)) {
      return ML_BITRATE_DEFAULT_KBPS;
    }
    return Math.min(Math.max(value, ML_BITRATE_MIN_KBPS), ML_BITRATE_MAX_KBPS);
  }

  /** The preset a 自动 缩放 resolves to right now (it follows 分辨率 and 超分 ratio). */
  private effectiveDefaultScale(): number {
    return SettingsStore.autoScalePercent(this.outputWidth(), this.outputHeight(),
      this.srEnabled, this.srRatioPercent);
  }

  /** Output resolution the page would use, i.e. what 分辨率 resolves to. */
  private outputWidth(): number {
    return this.resolutionAuto
      ? this.detectedWidth
      : this.parseSize(this.manualWidthText, this.detectedWidth, MAX_DESKTOP_SIZE);
  }

  private outputHeight(): number {
    return this.resolutionAuto
      ? this.detectedHeight
      : this.parseSize(this.manualHeightText, this.detectedHeight, MAX_DESKTOP_SIZE);
  }

  /** Session (pre-超分) resolution the server would be asked to render. */
  private sessionSizeText(): string {
    const width: number = SettingsStore.srSessionSize(this.outputWidth(), this.srRatioPercent);
    const height: number = SettingsStore.srSessionSize(this.outputHeight(), this.srRatioPercent);
    return `${width} × ${height}`;
  }

  private persist(): void {
    const disp = defaultDisplay();
    const maxWidth: number = disp !== undefined ? disp.width : Number.MAX_SAFE_INTEGER;
    const maxHeight: number = disp !== undefined ? disp.height : Number.MAX_SAFE_INTEGER;
    // Start from what is stored so the other scopes are preserved.
    const settings: AppSettings = SettingsStore.load();
    // App-level surfaces shared by every tab: the main / session window sizing,
    // the auto-hide behaviour and the developer section with its tools.
    settings.autoHideMainWindow = this.autoHideMainWindow;
    settings.mainWindowSizeEnabled = this.mainSizeEnabled;
    settings.mainWindowWidth =
      this.parseSize(this.mainWidthText, this.defaultMainWidth, maxWidth);
    settings.mainWindowHeight =
      this.parseSize(this.mainHeightText, this.defaultMainHeight, maxHeight);
    settings.sessionWindowSizeEnabled = this.sessionSizeEnabled;
    settings.sessionWindowWidth =
      this.parseSize(this.sessionWidthText, this.defaultSessionWidth, maxWidth);
    settings.sessionWindowHeight =
      this.parseSize(this.sessionHeightText, this.defaultSessionHeight, maxHeight);
    settings.sessionFullscreen = this.sessionFullscreen;
    settings.developerMode = this.developerMode;
    settings.gfxCapture = this.gfxCapture;
    settings.touchHighRate = this.touchHighRate;
    settings.toolbarHoverDelay = this.hoverDelay;
    settings.toolbarHideDelay = this.hideDelay;
    if (this.protocol === PROTOCOL_RDP) {
      settings.touchpadScrollSpeed = this.touchpadScrollSpeed;
      settings.pinchZoomSpeed = this.pinchZoomSpeed;
      settings.pinchMode = this.pinchMode;
      settings.touchpadMiddleDoubleTap = this.touchpadMiddleDoubleTap;
      settings.pinchTouchAngle = this.pinchTouchAngle;
      settings.pinchTouchDistance = this.pinchTouchDistance;
      settings.useRdpCursor = this.useRdpCursor;
      settings.reconnectOnForeground = this.reconnectOnForeground;
      settings.winKeySubstitute = this.winKeySubstitute;
      settings.shortcutPassthrough = this.shortcutPassthrough;
      settings.resolutionAuto = this.resolutionAuto;
      settings.manualWidth =
        this.parseSize(this.manualWidthText, this.detectedWidth, MAX_DESKTOP_SIZE);
      settings.manualHeight =
        this.parseSize(this.manualHeightText, this.detectedHeight, MAX_DESKTOP_SIZE);
      settings.scaleAuto = this.scaleAuto;
      settings.manualScalePercent =
        this.parseScale(this.manualScaleText, this.effectiveDefaultScale());
      settings.enableAudio = this.enableAudio;
      settings.ignoreCertificate = this.ignoreCertificate;
      settings.hardwareAccel = this.hardwareAccel;
      settings.srEnabled = this.srEnabled && this.srSupported;
      settings.srRatioPercent = SettingsStore.snapSrRatioPreset(this.srRatioPercent);
      settings.srBackend = this.srBackend;
    } else if (this.protocol === PROTOCOL_MOONLIGHT) {
      settings.mlResMode = this.mlResMode;
      settings.mlWidth = this.mlResMode === ML_RES_FIXED
        ? this.parseSize(this.mlWidthText, this.detectedWidth, MAX_DESKTOP_SIZE) : 0;
      settings.mlHeight = this.mlResMode === ML_RES_FIXED
        ? this.parseSize(this.mlHeightText, this.detectedHeight, MAX_DESKTOP_SIZE) : 0;
      settings.mlFps = this.mlFps;
      settings.mlBitrateKbps = this.effectiveMlBitrate();
      settings.mlCodec = this.mlCodec;
      settings.mlTouchpadScrollSpeed = this.touchpadScrollSpeed;
      settings.mlPinchZoomSpeed = this.pinchZoomSpeed;
      settings.mlPinchMode = this.pinchMode;
      settings.mlTouchpadMiddleDoubleTap = this.touchpadMiddleDoubleTap;
      settings.mlPinchTouchAngle = this.pinchTouchAngle;
      settings.mlPinchTouchDistance = this.pinchTouchDistance;
      settings.mlCursorMode = this.mlCursorMode;
      settings.mlReconnectOnForeground = this.reconnectOnForeground;
      settings.mlWinKeySubstitute = this.winKeySubstitute;
      settings.mlShortcutPassthrough = this.shortcutPassthrough;
    }
    SettingsStore.save(settings);
    // Apply immediately so a toggle takes effect for sessions opened next,
    // without waiting for an app restart.
    if (this.protocol === PROTOCOL_RDP) {
      RdpNative.applyRdpCursor(this.useRdpCursor);
      RdpNative.applyHardwareAccel(this.hardwareAccel);
    }
  }

  private confirmReset(): void {
    showConfirm(this.getUIContext(), {
      title: '恢复默认设置',
      message: '确定要把全部设置（RDP、Moonlight 与通用）恢复为默认值？各处的自定义设置都会丢失，'
        + '已保存的连接不受影响。',
      confirmLabel: '恢复默认',
      danger: true,
      onConfirm: () => {
        this.resetDefaults();
      }
    });
  }

  /**
   * Resets everything: both protocol scopes and the common scope are written
   * straight from the defaults, then the page reloads. Saving the whole default
   * object avoids going through `persist()`, which only writes the active tab.
   */
  private resetDefaults(): void {
    const defaults: AppSettings = SettingsStore.defaults();
    // Match the greyed-out rows on a device that cannot provide the feature, so
    // the stored value never asks for what is unavailable here.
    if (!this.audioSupported) {
      defaults.enableAudio = false;
    }
    if (!this.hardwareAccelSupported) {
      defaults.hardwareAccel = false;
    }
    SettingsStore.save(defaults);
    this.syncFromStore();
  }

  private confirmExport(): void {
    showConfirm(this.getUIContext(), {
      title: '导出配置',
      message: '将导出全局设置和全部连接。密码保存在系统安全存储中，出于安全考虑不会导出。',
      confirmLabel: '导出',
      onConfirm: () => {
        this.handleExport();
      }
    });
  }

  private async handleExport(): Promise<void> {
    if (this.transferBusy) {
      return;
    }
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      this.transferMessage = '无法获取应用上下文';
      return;
    }
    this.transferBusy = true;
    this.transferMessage = '';
    const result: ConfigTransferResult =
      await ConfigTransfer.exportToFile(context as common.UIAbilityContext);
    this.transferBusy = false;
    this.transferMessage = result.message;
  }

  private confirmImport(): void {
    showConfirm(this.getUIContext(), {
      title: '导入配置',
      message: '导入会应用全局设置，并按连接 ID 覆盖或新增连接。密码不会被导入，需重新输入。是否继续？',
      confirmLabel: '导入',
      onConfirm: () => {
        this.handleImport();
      }
    });
  }

  private async handleImport(): Promise<void> {
    if (this.transferBusy) {
      return;
    }
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      this.transferMessage = '无法获取应用上下文';
      return;
    }
    this.transferBusy = true;
    this.transferMessage = '';
    const result: ConfigTransferResult =
      await ConfigTransfer.importFromFile(context as common.UIAbilityContext);
    this.transferBusy = false;
    this.transferMessage = result.message;
    if (result.success) {
      this.syncFromStore();
    }
  }

  // Win key substitute: the user presses the key to use instead of Win.
  private beginWinCapture(): void {
    this.winSubstituteCapturing = true;
    try {
      this.getUIContext().getFocusController().requestFocus('winSubstituteCapture');
    } catch (error) {
      // Best effort; clicking the control already focused it.
    }
  }

  private captureWinKey(event: KeyEvent): void {
    if (!this.winSubstituteCapturing || event.type !== KeyType.Down) {
      return;
    }
    // Escape cancels the capture without changing the binding.
    if (event.keyCode === 2070) {
      this.winSubstituteCapturing = false;
      return;
    }
    this.winKeySubstitute = event.keyCode;
    this.winSubstituteCapturing = false;
    this.persist();
  }

  private clearWinSubstitute(): void {
    this.winKeySubstitute = 0;
    this.winSubstituteCapturing = false;
    this.persist();
  }

  private winSubstituteText(): string {
    if (this.winSubstituteCapturing) {
      return '请按下一个键…';
    }
    return this.winKeySubstitute === 0 ? '未设置' : KeyMapper.describe(this.winKeySubstitute);
  }

  @Builder
  resolutionModeChip(label: string, auto: boolean, leftMargin: number) {
    Text(label)
      .fontSize(13)
      .fontColor(this.resolutionAuto === auto ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.resolutionAuto === auto
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.resolutionAuto = auto;
        this.persist();
      })
  }

  @Builder
  scaleModeChip(label: string, auto: boolean, leftMargin: number) {
    Text(label)
      .fontSize(13)
      .fontColor(this.scaleAuto === auto ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.scaleAuto === auto
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.scaleAuto = auto;
        this.persist();
      })
  }

  /** One choice of the touchpad pinch mode; the accent marks the active one. */
  @Builder
  pinchModeChip(mode: number, label: string) {
    Text(label)
      .fontSize(13)
      .fontColor(this.pinchMode === mode ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.pinchMode === mode
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.pinchMode = mode;
        this.persist();
      })
  }

  /** One-line explanation of the selected touchpad pinch mode. */
  private pinchModeDescription(): string {
    if (this.pinchMode === PINCH_MODE_TOUCH) {
      return '在鼠标位置合成两个原生触点，远端像真实双指一样缩放';
    }
    if (this.pinchMode === PINCH_MODE_ZOOM) {
      return '只在本机放大画面，不向远端发送数据；以光标为中心，最小 100%';
    }
    return '映射为 Ctrl+滚轮，由远端应用自行缩放（会触发远端重绘）';
  }

  @Builder
  srRatioChip(preset: number) {
    ChoiceChip({
      label: srRatioLabel(preset),
      selected: this.srRatioPercent === preset,
      onTap: () => {
        this.srRatioPercent = preset;
        this.persist();
      }
    })
  }

  /** The 超分 choice as one value: 0 = off, 1 = XEngine, 2 = FSR. */
  private srChoice(): number {
    if (!this.srEnabled) {
      return 0;
    }
    return this.srBackend === SR_BACKEND_FSR ? 2 : 1;
  }

  /** Whether a 超分 choice can be picked here (超分 rides the Vulkan presenter). */
  private srChoiceUsable(choice: number): boolean {
    if (choice === 0) {
      return true;
    }
    if (!this.hardwareAccel || !this.hardwareAccelSupported) {
      return false;
    }
    const backend: number = choice === 2 ? SR_BACKEND_FSR : SR_BACKEND_XENGINE;
    const token: string = backend === SR_BACKEND_FSR ? 'fsr' : 'xengine';
    return DeviceCapabilities.srBackends().indexOf(token) >= 0;
  }

  @Builder
  srChoiceChip(choice: number) {
    ChoiceChip({
      label: choice === 0 ? '关闭' : (choice === 1 ? 'XEngine' : 'FSR'),
      selected: this.srChoice() === choice,
      chipEnabled: this.srChoiceUsable(choice),
      onTap: () => {
        this.srEnabled = choice !== 0;
        if (choice !== 0) {
          this.srBackend = choice === 2 ? SR_BACKEND_FSR : SR_BACKEND_XENGINE;
        }
        this.persist();
      }
    })
  }

  @Builder
  scaleChip(preset: number) {
    ChoiceChip({
      label: preset.toString() + '%',
      selected: this.parsedManualScale() === preset,
      onTap: () => {
        this.manualScaleText = preset.toString();
        this.persist();
      }
    })
  }

  @Builder
  mlFpsChip(preset: number) {
    ChoiceChip({
      label: preset.toString(),
      selected: this.mlFps === preset,
      onTap: () => {
        this.mlFps = preset;
        this.persist();
      }
    })
  }

  /** Decoder support (CODEC_SUPPORT_*) of an ML_CODEC_* choice; auto takes the
   * best any codec offers. */
  private mlCodecSupportOf(codec: number): number {
    return moonlightCodecSupport(codec, this.mlH264Support, this.mlHevcSupport, this.mlAv1Support);
  }

  /** Whether a codec choice can run on this device (auto included). */
  private mlCodecUsable(codec: number): boolean {
    return moonlightCodecUsable(codec, this.mlH264Support, this.mlHevcSupport, this.mlAv1Support);
  }

  /** Whether a codec choice would decode in software only. */
  private mlCodecSoftware(codec: number): boolean {
    return moonlightCodecSoftware(codec, this.mlH264Support, this.mlHevcSupport, this.mlAv1Support);
  }

  /** Falls back to auto when the stored choice cannot run here. */
  private clampMlCodec(): void {
    if (!this.mlCodecUsable(this.mlCodec)) {
      this.mlCodec = ML_CODEC_AUTO;
    }
  }

  @Builder
  mlCodecChip(codec: number, label: string) {
    ChoiceChip({
      label: label,
      selected: this.mlCodec === codec,
      chipEnabled: this.mlCodecUsable(codec),
      onTap: () => {
        this.mlCodec = codec;
        this.persist();
      }
    })
  }

  @Builder
  mlResModeChip(mode: number, label: string) {
    ChoiceChip({
      label: label,
      selected: this.mlResMode === mode,
      onTap: () => {
        this.mlResMode = mode;
        this.persist();
      }
    })
  }

  @Builder
  mlCursorModeChip(mode: number, label: string) {
    ChoiceChip({
      label: label,
      selected: this.mlCursorMode === mode,
      onTap: () => {
        this.mlCursorMode = mode;
        this.persist();
      }
    })
  }

  /** One-line explanation of the selected Moonlight cursor mode. */
  private cursorModeDescription(): string {
    if (this.mlCursorMode === ML_CURSOR_OFF) {
      return '不处理光标：画面里是主机画的光标，本机也画一个（可能双重）';
    }
    if (this.mlCursorMode === ML_CURSOR_HIDE_HOST) {
      return '主机不画光标，本机按主机形状绘制（顺滑、无双重）';
    }
    return '隐藏本机系统光标，只显示画面里主机画的光标';
  }

  /** Switches the protocol tab, saving the current one and loading the other. */
  private switchProtocol(protocol: number): void {
    if (this.protocol === protocol) {
      return;
    }
    this.persist();
    this.protocol = protocol;
    this.syncFromStore();
  }

  /**
   * Moonlight 显示: the stream resolution, next to the cursor mode (mirrors the
   * RDP 显示 section, which also holds 分辨率).
   */
  @Builder
  mlResolutionSection() {
    Column() {
      Text('分辨率').fontSize(14).fontColor($r('app.color.text_primary'))
      Text('跟随屏幕出画，或固定一个分辨率')
        .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
      Flex({ wrap: FlexWrap.Wrap }) {
        this.mlResModeChip(ML_RES_FOLLOW_SCREEN, '跟随屏幕')
        this.mlResModeChip(ML_RES_FIXED, '固定')
      }
      .width('100%').margin({ top: 8 })

      if (this.mlResMode === ML_RES_FIXED) {
        Row() {
          TextInput({ placeholder: '1920', text: this.mlWidthText })
            .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
            .backgroundColor($r('app.color.input_bg')).borderRadius(10)
            .onChange((value: string) => {
              this.mlWidthText = value;
              this.persist();
            })
          Text('×')
            .fontSize(14).fontColor($r('app.color.text_tertiary'))
            .margin({ left: 10, right: 10 })
          TextInput({ placeholder: '1080', text: this.mlHeightText })
            .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
            .backgroundColor($r('app.color.input_bg')).borderRadius(10)
            .onChange((value: string) => {
              this.mlHeightText = value;
              this.persist();
            })
        }
        .width('100%').margin({ top: 10 })
      }
    }
    .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)
  }

  /** Moonlight tab: the global stream defaults, mirrored by the per-connection overrides. */
  @Builder
  moonlightStreamSection() {
    Column() {
      Text('串流')
        .fontSize(13)
        .fontColor($r('app.color.text_secondary'))
        .margin({ bottom: 4 })

      Text('新连接默认使用的画质；可在单个连接的「高级设置」中改为独立配置')
        .fontSize(11)
        .fontColor($r('app.color.text_tertiary'))
        .width('100%')
        .margin({ bottom: 8 })

      Column() {
        Text('帧率').fontSize(14).fontColor($r('app.color.text_primary'))
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(ML_FPS_PRESETS, (preset: number) => {
            this.mlFpsChip(preset)
          }, (preset: number) => preset.toString())
        }
        .width('100%').margin({ top: 8 })
      }
      .width('100%').padding({ top: 8, bottom: 8 }).alignItems(HorizontalAlign.Start)

      Divider().color($r('app.color.divider'))

      Column() {
        Text('码率 (kbps)').fontSize(14).fontColor($r('app.color.text_primary'))
        Text('越高画质越好、越占带宽；建议 '
          + ML_BITRATE_MIN_KBPS + ' – ' + ML_BITRATE_MAX_KBPS)
          .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
        TextInput({ placeholder: '20000', text: this.mlBitrateText })
          .type(InputType.Number).fontSize(14).height(40)
          .backgroundColor($r('app.color.input_bg')).borderRadius(10).margin({ top: 8 })
          .onChange((value: string) => {
            this.mlBitrateText = value;
            this.persist();
          })
      }
      .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)

      Divider().color($r('app.color.divider'))

      Column() {
        Text('编码器').fontSize(14).fontColor($r('app.color.text_primary'))
        if (!this.mlCodecUsable(ML_CODEC_AUTO)) {
          Text('本设备无可用视频解码器，无法串流')
            .fontSize(11).fontColor($r('app.color.danger')).margin({ top: 2 })
        } else if (this.mlCodecSoftware(this.mlCodec)) {
          Text('无硬件解码支持，将使用软件解码')
            .fontSize(11).fontColor($r('app.color.danger')).margin({ top: 2 })
        }
        Flex({ wrap: FlexWrap.Wrap }) {
          this.mlCodecChip(ML_CODEC_AUTO, '自动')
          this.mlCodecChip(ML_CODEC_H264, 'H.264')
          this.mlCodecChip(ML_CODEC_HEVC, 'HEVC')
          this.mlCodecChip(ML_CODEC_AV1, 'AV1')
        }
        .width('100%').margin({ top: 8 })
      }
      .width('100%').padding({ top: 8, bottom: 8 }).alignItems(HorizontalAlign.Start)
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  toggleItem(item: ToggleItemConfig) {
    SettingToggleRow({
      title: item.title,
      subtitle: item.subtitle,
      value: item.value,
      switchEnabled: item.enabled,
      disabledReason: item.disabledReason,
      onToggle: (v: boolean) => {
        item.onChange(v);
      }
    })
  }

  @Builder
  sliderItem(item: SliderItemConfig) {
    SettingSliderRow({
      title: item.title,
      subtitle: item.subtitle,
      value: item.value,
      min: item.min,
      max: item.max,
      step: item.step,
      unit: item.unit,
      precision: item.precision,
      switchEnabled: item.enabled,
      disabledReason: item.disabledReason,
      onChange: (v: number) => {
        item.onChange(v);
      }
    })
  }

  build() {
    Column() {
      HeaderBar({
        title: '设置',
        leftLabel: '返回',
        onLeft: () => {
          this.getUIContext().getRouter().back();
        }
      }) {
        ProtocolTabs({
          protocol: this.protocol,
          showCommon: true,
          onChange: (protocol: number) => {
            this.switchProtocol(protocol);
          }
        })
      }

      Scroll() {
        Column() {
          MaterialCard({ pad: 24 }) {
            if (this.protocol === PROTOCOL_RDP) {
            Text('显示')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            this.toggleItem({
              title: '使用RDP光标',
              subtitle: '远端的光标形状同步为系统光标；若显示异常可关闭，回退为默认箭头',
              value: this.useRdpCursor,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.useRdpCursor = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            Column() {
              Row() {
                Column() {
                  Text('分辨率').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text(this.srEnabled
                    ? '超分辨率后的输出分辨率；会话按倍率取更低的桌面分辨率'
                    : '自动使用当前显示器的分辨率')
                    .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Row() {
                  this.resolutionModeChip('自动', true, 0)
                  this.resolutionModeChip('手动', false, 8)
                }
              }
              .width('100%')

              if (this.resolutionAuto) {
                Text('自动检测：' + this.detectedText)
                  .fontSize(11).fontColor($r('app.color.text_tertiary'))
                  .width('100%').margin({ top: 10 })
              } else {
                Row() {
                  TextInput({ placeholder: '1920', text: this.manualWidthText })
                    .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
                    .backgroundColor($r('app.color.input_bg')).borderRadius(10)
                    .onChange((value: string) => {
                      this.manualWidthText = value;
                      this.persist();
                    })

                  Text('×')
                    .fontSize(14).fontColor($r('app.color.text_tertiary'))
                    .margin({ left: 10, right: 10 })

                  TextInput({ placeholder: '1080', text: this.manualHeightText })
                    .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
                    .backgroundColor($r('app.color.input_bg')).borderRadius(10)
                    .onChange((value: string) => {
                      this.manualHeightText = value;
                      this.persist();
                    })
                }
                .width('100%').margin({ top: 10 })

                Text('自动检测：' + this.detectedText + '，可参考输入')
                  .fontSize(11).fontColor($r('app.color.text_tertiary'))
                  .width('100%').margin({ top: 6 })
              }
            }
            .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            Column() {
              Row() {
                Column() {
                  Text('缩放比例').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text(this.srEnabled
                    ? '超分辨率前会话桌面的缩放；自动档按会话分辨率相对屏幕换算'
                    : '按分辨率相对屏幕的比例自动选择推荐缩放比例')
                    .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Row() {
                  this.scaleModeChip('自动', true, 0)
                  this.scaleModeChip('手动', false, 8)
                }
              }
              .width('100%')

              if (this.scaleAuto) {
                Text('推荐：' + this.effectiveDefaultScale() + '%')
                  .fontSize(11).fontColor($r('app.color.text_tertiary'))
                  .width('100%').margin({ top: 10 })
              } else {
                Flex({ wrap: FlexWrap.Wrap }) {
                  ForEach(SCALE_PRESETS, (preset: number) => {
                    this.scaleChip(preset)
                  }, (preset: number) => preset.toString())
                }
                .width('100%').margin({ top: 10 })

                Row() {
                  TextInput({ placeholder: '100', text: this.manualScaleText })
                    .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
                    .backgroundColor($r('app.color.input_bg')).borderRadius(10)
                    .onChange((value: string) => {
                      this.manualScaleText = value;
                      this.persist();
                    })

                  Text('%')
                    .fontSize(14).fontColor($r('app.color.text_tertiary'))
                    .margin({ left: 10 })
                }
                .width('100%')

                if (this.parsedManualScale() > 0 && !this.isPresetScale(this.parsedManualScale())) {
                  Text('自定义缩放比例可能导致部分应用显示模糊，建议使用上方固定档位')
                    .fontSize(11).fontColor($r('app.color.danger'))
                    .width('100%').margin({ top: 6 })
                }
              }
            }
            .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)
            } else if (this.protocol === PROTOCOL_MOONLIGHT) {
              Text('显示')
                .fontSize(13)
                .fontColor($r('app.color.text_secondary'))
                .margin({ bottom: 4 })

              Column() {
                Text('光标模式')
                  .fontSize(14)
                  .fontColor($r('app.color.text_primary'))
                Text(this.cursorModeDescription())
                  .fontSize(11)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ top: 2 })
                // Red caveat above the options, like the RDP 超分辨率 row.
                if (this.mlCursorMode === ML_CURSOR_HIDE_HOST) {
                  Text('此功能仅在基地版 Sunshine（foundation-sunshine）有效；其它主机会回退为「关闭」')
                    .fontSize(11)
                    .fontColor($r('app.color.danger'))
                    .margin({ top: 2 })
                }

                Row() {
                  this.mlCursorModeChip(ML_CURSOR_OFF, '关闭')
                  this.mlCursorModeChip(ML_CURSOR_HIDE_SYSTEM, '隐藏系统光标')
                  this.mlCursorModeChip(ML_CURSOR_HIDE_HOST, '隐藏主机光标')
                }
                .width('100%')
                .margin({ top: 10, bottom: 6 })
              }
              .width('100%')
              .padding({ top: 10, bottom: 14 })
              .alignItems(HorizontalAlign.Start)

              Divider().color($r('app.color.divider'))

              this.mlResolutionSection()

              Divider().color($r('app.color.divider'))

              this.moonlightStreamSection()
            }

            if (this.protocol !== TAB_COMMON) {
            Divider().color($r('app.color.divider'))

            Text('触控板')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            this.sliderItem({
              title: '滚动速度',
              subtitle: '触控板双指滑动速度，1.0× 为基准',
              value: this.touchpadScrollSpeed,
              min: MIN_SPEED_SCALE,
              max: MAX_SPEED_SCALE,
              step: 0.1,
              unit: '×',
              precision: 1,
              enabled: true,
              disabledReason: '',
              onChange: (v: number) => {
                this.touchpadScrollSpeed = v;
                this.persist();
              }
            })

            Text('触控板两指捏合模式')
              .fontSize(14)
              .fontColor($r('app.color.text_primary'))
            Text(this.pinchModeDescription())
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .margin({ top: 2 })

            Row() {
              this.pinchModeChip(PINCH_MODE_WHEEL, 'Ctrl 滚轮')
              this.pinchModeChip(PINCH_MODE_TOUCH, '触摸模拟')
              this.pinchModeChip(PINCH_MODE_ZOOM, '直接缩放')
            }
            .width('100%')
            .margin({ top: 10, bottom: 6 })

            if (this.pinchMode === PINCH_MODE_WHEEL) {
              Divider().color($r('app.color.divider'))

              this.sliderItem({
                title: '缩放速度',
                subtitle: 'Ctrl+滚轮模式的缩放速度，越大越快，1.0× 为基准',
                value: this.pinchZoomSpeed,
                min: MIN_SPEED_SCALE,
                max: MAX_SPEED_SCALE,
                step: 0.1,
                unit: '×',
                precision: 1,
                enabled: true,
                disabledReason: '',
                onChange: (v: number) => {
                  this.pinchZoomSpeed = v;
                  this.persist();
                }
              })
            }

            if (this.pinchMode === PINCH_MODE_TOUCH) {
              Divider().color($r('app.color.divider'))

              this.sliderItem({
                title: '触摸捏合角度',
                subtitle: '模拟触点连线的倾角：正=顺时针（左手），负=逆时针（右手）',
                value: this.pinchTouchAngle,
                min: -180,
                max: 180,
                step: 15,
                unit: '°',
                precision: 0,
                enabled: true,
                disabledReason: '',
                onChange: (v: number) => {
                  this.pinchTouchAngle = v;
                  this.persist();
                }
              })

              Divider().color($r('app.color.divider'))

              this.sliderItem({
                title: '触点初始距离',
                subtitle: '两个模拟触点起始间距，取窗口较短边的百分比（越大越像真实双指）',
                value: this.pinchTouchDistance,
                min: 1,
                max: 20,
                step: 1,
                unit: '%',
                precision: 0,
                enabled: true,
                disabledReason: '',
                onChange: (v: number) => {
                  this.pinchTouchDistance = v;
                  this.persist();
                }
              })
            }

            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '双指双击发送中键',
              subtitle: '触控板双指连续轻触两次发送鼠标中键；开启后双指轻触的右键会有约 300ms 判定延迟',
              value: this.touchpadMiddleDoubleTap,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.touchpadMiddleDoubleTap = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            Text('键盘')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            Row() {
              Column() {
                Text('Win 键替代')
                  .fontSize(14)
                  .fontColor($r('app.color.text_primary'))
                Text(this.winSubstituteCapturing
                  ? '按下要作为 Win 的按键（Esc 取消）'
                  : '点击右侧按键框，再按下要替代 Win 的按键')
                  .fontSize(11)
                  .fontColor(this.winSubstituteCapturing
                    ? $r('app.color.accent') : $r('app.color.text_tertiary'))
                  .margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              if (this.winKeySubstitute !== 0 && !this.winSubstituteCapturing) {
                TextAction({
                  label: '清除',
                  muted: true,
                  onTap: () => {
                    this.clearWinSubstitute();
                  }
                })
              }

              Text(this.winSubstituteText())
                .fontSize(13)
                .fontColor(this.winSubstituteCapturing
                  ? $r('app.color.accent')
                  : (this.winKeySubstitute === 0
                    ? $r('app.color.text_tertiary') : $r('app.color.text_primary')))
                .padding({ left: 14, right: 14, top: 7, bottom: 7 })
                .backgroundColor($r('app.color.input_bg'))
                .borderRadius(8)
                .border({
                  width: 1,
                  color: this.winSubstituteCapturing
                    ? $r('app.color.accent') : $r('app.color.divider')
                })
                .focusable(true)
                .id('winSubstituteCapture')
                .onKeyEvent((event: KeyEvent) => {
                  this.captureWinKey(event);
                })
                .onBlur(() => {
                  this.winSubstituteCapturing = false;
                })
                .hoverEffect(HoverEffect.Highlight)
                .onClick(() => {
                  this.beginWinCapture();
                })
            }
            .width('100%')
            .padding({ top: 14, bottom: 14 })

            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '快捷键穿透',
              subtitle: '会话窗口聚焦时接管键盘，把 Win、Alt+Tab、Esc 等快捷键发送到远端；开启后本机的系统快捷键会被远端接管',
              value: this.shortcutPassthrough,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.shortcutPassthrough = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            Text('会话')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            this.toggleItem({
              title: '回到前台自动重连',
              subtitle: '切到后台再回来时，若会话已被系统断开则自动重连，不必手动点「重试」',
              value: this.reconnectOnForeground,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.reconnectOnForeground = v;
                this.persist();
              }
            })

            if (this.protocol === PROTOCOL_RDP) {
            Divider().color($r('app.color.divider'))

            Text('连接特性')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            Text('新连接默认使用的协议特性；可在单个连接的「高级设置」中改为独立配置')
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .margin({ bottom: 8 })

            this.toggleItem({
              title: '音频重定向',
              subtitle: '播放远端的声音',
              value: this.enableAudio,
              enabled: this.audioSupported,
              disabledReason: this.audioDisabledReason,
              onChange: (v: boolean) => {
                this.enableAudio = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '硬件加速',
              subtitle: '用 Vulkan 把画面贴到窗口；关闭则完全不用 Vulkan（GLES 上屏，兼容性优先）',
              // Locked on devices whose Vulkan cannot present a frame; the
              // session then runs on the GLES presenter either way (see
              // DeviceCapabilities.hardwareAccel).
              value: this.hardwareAccel,
              enabled: this.hardwareAccelSupported,
              disabledReason: this.hardwareAccelSupported ? '' : this.hardwareAccelReason,
              onChange: (v: boolean) => {
                this.hardwareAccel = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            // The ratio lives in the same section as the switch: turning 超分辨率
            // on only adds a row of presets below the switch, not a new block.
            Column() {
              Row() {
                Column() {
                  Text('超分辨率')
                    .fontSize(14)
                    .fontColor((this.hardwareAccel && this.srSupported)
                      ? $r('app.color.text_primary') : $r('app.color.text_tertiary'))
                  Text('远端按「分辨率 ÷ 倍率」出画，本机用 GPU 上采样回输出分辨率：省带宽与解码')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                  if (!this.srSupported) {
                    Text(this.srReason)
                      .fontSize(11)
                      .fontColor($r('app.color.danger'))
                      .margin({ top: 2 })
                  } else if (!this.hardwareAccel) {
                    Text('超分辨率需要开启硬件加速（Vulkan 上屏）')
                      .fontSize(11)
                      .fontColor($r('app.color.danger'))
                      .margin({ top: 2 })
                  }
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

              }
              .width('100%')

              // 超分 is a three-way choice: off, the platform's XEngine, or the
              // presenter's own FSR. A backend this build/device cannot run is
              // dimmed and unselectable.
              Flex({ wrap: FlexWrap.Wrap }) {
                this.srChoiceChip(0)
                this.srChoiceChip(1)
                this.srChoiceChip(2)
              }
              .width('100%').margin({ top: 10 })

              if (this.srEnabled && this.srSupported && this.hardwareAccel) {
                Flex({ wrap: FlexWrap.Wrap }) {
                  ForEach(SR_RATIO_PRESETS, (preset: number) => {
                    this.srRatioChip(preset)
                  }, (preset: number) => preset.toString())
                }
                .width('100%').margin({ top: 10 })

                Text('输出 ' + this.outputWidth() + ' × ' + this.outputHeight()
                  + ' · 会话 ' + this.sessionSizeText())
                  .fontSize(11).fontColor($r('app.color.text_tertiary'))
                  .width('100%').margin({ top: 2 })
              }
            }
            .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '忽略证书校验',
              subtitle: '仅用于自签名证书的测试环境，存在安全风险',
              value: this.ignoreCertificate,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.ignoreCertificate = v;
                this.persist();
              }
            })
            }
            }

            if (this.protocol === TAB_COMMON) {
            Divider().color($r('app.color.divider'))

            Text('窗口')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            Column() {
              Row() {
                Column() {
                  Text('默认主窗口尺寸').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('主窗口创建时使用的大小，下次启动生效（像素）')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.mainSizeEnabled })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.mainSizeEnabled = isOn;
                    this.persist();
                  })
              }
              .width('100%')

              Row() {
                TextInput({ text: this.mainWidthText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.mainSizeEnabled)
                  .opacity(this.mainSizeEnabled ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.mainWidthText = value;
                    this.persist();
                  })

                Text('×')
                  .fontSize(14)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ left: 10, right: 10 })

                TextInput({ text: this.mainHeightText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.mainSizeEnabled)
                  .opacity(this.mainSizeEnabled ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.mainHeightText = value;
                    this.persist();
                  })
              }
              .width('100%')
              .margin({ top: 10 })
            }
            .width('100%')
            .padding({ top: 14, bottom: 14 })
            .alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            Column() {
              Row() {
                Column() {
                  Text('默认会话窗口尺寸').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('会话窗口创建时使用的大小，下次连接生效（像素）')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.sessionSizeEnabled })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.sessionSizeEnabled = isOn;
                    this.persist();
                  })
              }
              .width('100%')

              Row() {
                TextInput({ text: this.sessionWidthText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.sessionSizeEnabled && !this.sessionFullscreen)
                  .opacity(this.sessionSizeEnabled && !this.sessionFullscreen ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.sessionWidthText = value;
                    this.persist();
                  })

                Text('×')
                  .fontSize(14)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ left: 10, right: 10 })

                TextInput({ text: this.sessionHeightText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.sessionSizeEnabled && !this.sessionFullscreen)
                  .opacity(this.sessionSizeEnabled && !this.sessionFullscreen ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.sessionHeightText = value;
                    this.persist();
                  })
              }
              .width('100%')
              .margin({ top: 10 })

              Row() {
                Column() {
                  Text('默认最大化（全屏）').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('会话窗口打开时直接最大化到全屏，忽略上面的尺寸')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.sessionFullscreen })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.sessionFullscreen = isOn;
                    this.persist();
                  })
              }
              .width('100%')
              .height(58)
              .margin({ top: 4 })

              Divider().color($r('app.color.divider'))

              Row() {
                Column() {
                  Text('自动隐藏主窗口').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('单窗口模式：会话窗口打开时隐藏主窗口，关闭后恢复（不支持多会话）')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.autoHideMainWindow })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.autoHideMainWindow = isOn;
                    this.persist();
                  })
              }
              .width('100%')
              .height(58)
              .margin({ top: 4 })
            }
            .width('100%')
            .padding({ top: 14, bottom: 14 })
            .alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            Text('配置备份')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ top: 4, bottom: 4 })

            Text('导出全局设置与全部连接，或从文件导入覆盖；密码不会被导出。')
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .margin({ bottom: 12 })

            Row() {
              Button('导出配置')
                .layoutWeight(1)
                .height(44)
                .fontSize(15)
                .fontColor($r('app.color.accent'))
                .backgroundColor($r('app.color.input_bg'))
                .borderRadius(12)
                .enabled(!this.transferBusy)
                .onClick(() => {
                  this.confirmExport();
                })

              Button('导入配置')
                .layoutWeight(1)
                .height(44)
                .fontSize(15)
                .fontColor('#FFFFFF')
                .backgroundColor($r('app.color.accent'))
                .borderRadius(12)
                .margin({ left: 12 })
                .enabled(!this.transferBusy)
                .onClick(() => {
                  this.confirmImport();
                })
            }
            .width('100%')

            if (this.transferBusy || this.transferMessage.length > 0) {
              Row() {
                if (this.transferBusy) {
                  LoadingProgress()
                    .width(16)
                    .height(16)
                    .color($r('app.color.accent'))
                }
                Text(this.transferBusy ? '处理中…' : this.transferMessage)
                  .fontSize(12)
                  .fontColor($r('app.color.text_secondary'))
                  .layoutWeight(1)
                  .margin({ left: this.transferBusy ? 8 : 0 })
              }
              .width('100%')
              .margin({ top: 10 })
            }
            }

            // "恢复默认设置" lives only in the 通用 tab; it resets everything
            // (both protocols and the common scope).
            if (this.protocol === TAB_COMMON) {
            Divider().color($r('app.color.divider'))

            Text('恢复全部设置（RDP / Moonlight / 通用）为默认值，不影响已保存的连接。')
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .margin({ top: 20, bottom: 10 })

            Button('恢复默认设置')
              .width('100%')
              .height(44)
              .fontSize(15)
              .fontColor($r('app.color.danger'))
              .backgroundColor($r('app.color.input_bg'))
              .borderRadius(12)
              .onClick(() => {
                this.confirmReset();
              })
            }

            // RDP's own developer tools. The gate lives in the 通用 tab, but the
            // tools it reveals are RDP-specific, so they appear at the bottom of
            // the RDP tab (not in 通用).
            if (this.protocol === PROTOCOL_RDP && this.developerMode) {
            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '抓取 GFX 码流（测试）',
              subtitle: '抓取会话的原始 GFX 通道码流（hmrdp_gfx.bin），供开发/回放使用',
              value: this.gfxCapture,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.gfxCapture = v;
                this.persist();
                this.applyGfxCaptureSetting();
              }
            })

            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '触屏高刷新率（实验）',
              subtitle: '把 RDPEI 50Hz 合帧提高到 125Hz 上报触点；可能增加带宽',
              value: this.touchHighRate,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.touchHighRate = v;
                this.persist();
              }
            })
            }

            if (this.protocol === TAB_COMMON) {
            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '开发者选项',
              subtitle: '显示工具栏性能信息，并揭示各协议下的实验 / 调参项',
              value: this.developerMode,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.developerMode = v;
                // Turning the gate off also stops the tools it revealed, so a
                // capture cannot keep running invisibly.
                if (!v && this.gfxCapture) {
                  this.gfxCapture = false;
                  this.persist();
                  this.applyGfxCaptureSetting();
                } else {
                  this.persist();
                }
              }
            })

            if (this.developerMode) {
              Divider().color($r('app.color.divider'))

              this.sliderItem({
                title: '工具栏触发停留时间',
                subtitle: '全屏下鼠标停留在屏幕顶部多久后弹出工具栏',
                value: this.hoverDelay,
                min: 0,
                max: 2400,
                step: 100,
                unit: ' ms',
                precision: 0,
                enabled: true,
                disabledReason: '',
                onChange: (v: number) => {
                  this.hoverDelay = v;
                  this.persist();
                }
              })

              Divider().color($r('app.color.divider'))

              this.sliderItem({
                title: '工具栏隐藏延迟',
                subtitle: '全屏下鼠标移开后多久收起工具栏',
                value: this.hideDelay,
                min: 100,
                max: 1000,
                step: 50,
                unit: ' ms',
                precision: 0,
                enabled: true,
                disabledReason: '',
                onChange: (v: number) => {
                  this.hideDelay = v;
                  this.persist();
                }
              })
            }
            }
          }
        }
        .width('100%')
        .padding({ top: 24, bottom: 32, left: 24, right: 24 })
        .alignItems(HorizontalAlign.Center)
      }
      .layoutWeight(1)
      .scrollBar(BarState.Auto)
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
