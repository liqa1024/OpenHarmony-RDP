import { common } from '@kit.AbilityKit';
import { DisplayProfile, SR_BACKEND_FSR, SR_BACKEND_XENGINE } from '../model/Profiles';
import { ML_BITRATE_DEFAULT_KBPS,
  ML_BITRATE_MAX_KBPS, ML_BITRATE_MIN_KBPS, ML_CODEC_AUTO, ML_CODEC_AV1, ML_CODEC_H264,
  ML_CODEC_HEVC, ML_COLORSPACE_REC601, ML_COLORSPACE_REC709, ML_FPS_PRESETS, ML_RES_FIXED,
  ML_RES_FOLLOW_SCREEN, ML_SCALE_AUTO, ML_SCALE_MANUAL } from '../moonlight/MoonlightModels';
import { PROTOCOL_MOONLIGHT, PROTOCOL_RDP } from '../model/Connections';
import { ConfigTransfer, ConfigTransferResult } from '../services/ConfigTransfer';
import { Capability, CODEC_SUPPORT_NONE, DeviceCapabilities } from '../services/DeviceCapabilities';
import { RdpNative } from '../services/RdpNative';
import { AppSettings, DEFAULT_PINCH_TOUCH_DISTANCE, DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT,
  ML_CURSOR_HIDE_HOST, ML_CURSOR_HIDE_SYSTEM,
  ML_CURSOR_OFF, PINCH_MODE_NATIVE, PINCH_MODE_TOUCH, PINCH_MODE_WHEEL,
  PINCH_MODE_ZOOM, SR_RATIO_PRESETS,
  SettingsStore, srRatioLabel } from '../services/SettingsStore';
import { DEFAULT_SPEED_SCALE, MAX_SPEED_SCALE, MIN_SPEED_SCALE } from '../services/TouchpadWheel';
import { MAX_RELATIVE_SPEED, MIN_RELATIVE_SPEED } from '../services/input/RelativeUnits';
import { showConfirm } from '../utils/UiUtil';
import { KeyMapper } from '../utils/KeyMapper';
import { defaultDisplay } from '../utils/DisplayUtil';
import { MAX_DESKTOP_SIZE, parseScaleValue, parseSizeValue } from '../utils/ScaleParse';
import { moonlightCodecSoftware, moonlightCodecSupport, moonlightCodecUsable } from '../utils/MoonlightCodec';
import { clampFps, kbpsToMbps, mbpsToKbps, recommendedBitrateKbps } from '../utils/MoonlightQuality';
import { TextAction } from '../components/TextAction';
import { ChoiceChip } from '../components/ChoiceChip';
import { HeaderBar } from '../components/HeaderBar';
import { MaterialCard } from '../components/MaterialCard';
import { ProtocolTabs, TAB_COMMON } from '../components/ProtocolTabs';
import { ScaleSetting } from '../components/ScaleSetting';
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
  precision: number = 0;
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
  enabled: boolean = true;
  disabledReason: string = '';
  onChange: (v: boolean) => void = (v: boolean) => {};
}

@Entry
@Component
struct SettingsPage {
  @State hoverDelay: number = 700;
  @State hideDelay: number = 250;
  @State triggerWidthPercent: number = DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT;
  @State touchpadScrollSpeed: number = DEFAULT_SPEED_SCALE;
  @State touchpadAlwaysEngine: boolean = false;
  @State pinchZoomSpeed: number = DEFAULT_SPEED_SCALE;
  @State pinchMode: number = PINCH_MODE_WHEEL;
  @State touchpadMiddleDoubleTap: boolean = false;
  @State scrollEngineFallback: boolean = false;
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
  @State srEnabled: boolean = false;
  @State srRatioPercent: number = SR_RATIO_PRESETS[0];
  @State srBackend: number = SR_BACKEND_XENGINE;
  @State srSupported: boolean = true;
  private srReason: string = '';
  // Moonlight tab: its own global SR values, and the extra video-import gate it needs.
  @State mlSrEnabled: boolean = false;
  @State mlSrRatioPercent: number = SR_RATIO_PRESETS[0];
  @State mlSrBackend: number = SR_BACKEND_XENGINE;
  @State mlSrSupported: boolean = true;
  private mlSrReason: string = '';
  @State developerMode: boolean = false;
  @State gfxCapture: boolean = false;
  @State protocol: number = PROTOCOL_RDP;
  @State mlResMode: number = ML_RES_FOLLOW_SCREEN;
  @State mlWidthText: string = '';
  @State mlHeightText: string = '';
  @State mlScaleMode: number = ML_SCALE_AUTO;
  @State mlScaleText: string = '100';
  @State mlFps: number = 60;
  @State mlFpsText: string = '60';
  @State mlFpsCustom: boolean = false;
  @State mlBitrateAuto: boolean = true;
  @State mlBitrateText: string = kbpsToMbps(ML_BITRATE_DEFAULT_KBPS).toString();
  @State mlColorSpace: number = ML_COLORSPACE_REC709;
  @State mlCodec: number = ML_CODEC_AUTO;
  @State mlCursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  @State mlRelativeMouse: boolean = false;
  @State relativeSpeed: number = DEFAULT_SPEED_SCALE;
  @State mlLowLatencyAudio: boolean = true;
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
    const savedTab: number = SettingsStore.lastSettingsTab();
    this.protocol = savedTab >= 0 ? savedTab : TAB_COMMON;
    this.syncFromStore();
  }

  private syncFromStore(): void {
    const settings: AppSettings = SettingsStore.load();
    const audioCapability: Capability = DeviceCapabilities.audio();
    this.audioSupported = audioCapability.supported;
    this.audioDisabledReason = audioCapability.reason;
    const hwCapability: Capability = DeviceCapabilities.hardwareAccel();
    const srCapability: Capability = DeviceCapabilities.superResolution();
    // The RDP 超分 path needs the Vulkan presenter; Moonlight's also needs the NV12 video import.
    this.srSupported = hwCapability.supported && srCapability.supported;
    this.srReason = hwCapability.supported ? srCapability.reason : hwCapability.reason;
    const videoImport: boolean = DeviceCapabilities.srVideoImport();
    this.mlSrSupported = this.srSupported && videoImport;
    this.mlSrReason = this.srSupported
      ? (videoImport ? '' : '本设备不支持从解码器直接导入视频帧，Moonlight 超分辨率不可用')
      : this.srReason;

    this.applyWindowSettings(settings);
    this.applyDeveloperSettings(settings);

    if (this.protocol === PROTOCOL_RDP) {
      this.useRdpCursor = settings.useRdpCursor;
      RdpNative.applyRdpCursor(this.useRdpCursor);
      this.resolutionAuto = settings.resolutionAuto;
      this.manualWidthText = this.sizeText(settings.manualWidth);
      this.manualHeightText = this.sizeText(settings.manualHeight);
      this.scaleAuto = settings.scaleAuto;
      this.enableAudio = this.audioSupported ? settings.enableAudio : false;
      this.srEnabled = this.srSupported ? settings.srEnabled : false;
      this.srRatioPercent = SettingsStore.snapSrRatioPreset(settings.srRatioPercent);
      this.srBackend = settings.srBackend;
      this.manualScaleText = settings.manualScalePercent > 0
        ? settings.manualScalePercent.toString() : this.effectiveDefaultScale().toString();
      this.ignoreCertificate = settings.ignoreCertificate;
      this.applyInteraction(settings, true);
    } else if (this.protocol === PROTOCOL_MOONLIGHT) {
      this.mlResMode = settings.mlResMode;
      this.mlWidthText = this.sizeText(settings.mlWidth > 0 ? settings.mlWidth : this.detectedWidth);
      this.mlHeightText = this.sizeText(settings.mlHeight > 0
        ? settings.mlHeight : this.detectedHeight);
      this.mlScaleMode = settings.mlScaleMode;
      this.mlScaleText = settings.mlScalePercent.toString();
      this.mlFps = settings.mlFps;
      this.mlFpsText = settings.mlFps.toString();
      this.mlFpsCustom = ML_FPS_PRESETS.indexOf(settings.mlFps) < 0;
      this.mlBitrateAuto = settings.mlBitrateAuto;
      this.mlBitrateText = kbpsToMbps(settings.mlBitrateKbps).toString();
      this.mlColorSpace = settings.mlColorSpace;
      this.mlCodec = settings.mlCodec;
      this.mlSrEnabled = this.mlSrSupported ? settings.mlSrEnabled : false;
      this.mlSrRatioPercent = SettingsStore.snapSrRatioPreset(settings.mlSrRatioPercent);
      this.mlSrBackend = settings.mlSrBackend;
    this.mlCursorMode = settings.mlCursorMode;
    this.mlRelativeMouse = settings.mlRelativeMouse;
    this.relativeSpeed = settings.mlRelativeSpeed;
      this.mlLowLatencyAudio = this.audioSupported ? settings.mlLowLatencyAudio : false;
      this.clampMlCodec();
      this.applyInteraction(settings, false);
    }

    if (this.protocol === PROTOCOL_RDP) {
      if (!this.audioSupported && settings.enableAudio) {
        this.persist();
      }
      if (!this.srSupported && settings.srEnabled) {
        this.persist();
      }
    }
    if (this.protocol === PROTOCOL_MOONLIGHT) {
      if (!this.audioSupported && settings.mlLowLatencyAudio) {
        this.persist();
      }
      if (!this.mlSrSupported && settings.mlSrEnabled) {
        this.persist();
      }
    }
    if (!this.developerMode && settings.gfxCapture) {
      this.persist();
    }
    this.applyGfxCaptureSetting();
  }

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

  private applyDeveloperSettings(settings: AppSettings): void {
    this.developerMode = settings.developerMode;
    this.gfxCapture = this.developerMode ? settings.gfxCapture : false;
    this.touchHighRate = settings.touchHighRate;
    this.scrollEngineFallback = settings.scrollEngineFallback;
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.triggerWidthPercent = settings.toolbarTriggerWidthPercent;
  }

  private applyInteraction(settings: AppSettings, rdp: boolean): void {
    this.touchpadScrollSpeed = rdp ? settings.touchpadScrollSpeed : settings.mlTouchpadScrollSpeed;
    this.touchpadAlwaysEngine = !rdp && settings.mlTouchpadScrollAlwaysEngine;
    this.pinchZoomSpeed = rdp ? settings.pinchZoomSpeed : settings.mlPinchZoomSpeed;
    this.touchpadMiddleDoubleTap = rdp
      ? settings.touchpadMiddleDoubleTap : settings.mlTouchpadMiddleDoubleTap;
    this.pinchMode = rdp ? settings.pinchMode : settings.mlPinchMode;
    // Direct zoom drives the RDP presenter's view transform; Moonlight has no such view.
    if (!rdp && this.pinchMode === PINCH_MODE_ZOOM) {
      this.pinchMode = PINCH_MODE_WHEEL;
    }
    // Native touchpad forwarding is a Moonlight-only target.
    if (rdp && this.pinchMode === PINCH_MODE_NATIVE) {
      this.pinchMode = PINCH_MODE_WHEEL;
    }
    this.pinchTouchAngle = rdp ? settings.pinchTouchAngle : settings.mlPinchTouchAngle;
    this.pinchTouchDistance = rdp ? settings.pinchTouchDistance : settings.mlPinchTouchDistance;
    this.reconnectOnForeground = rdp
      ? settings.reconnectOnForeground : settings.mlReconnectOnForeground;
    this.winKeySubstitute = rdp ? settings.winKeySubstitute : settings.mlWinKeySubstitute;
    this.shortcutPassthrough = rdp ? settings.shortcutPassthrough : settings.mlShortcutPassthrough;
  }

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

  private effectiveMlBitrate(): number {
    const mbps: number = Number.parseFloat(this.mlBitrateText);
    const kbps: number = Number.isNaN(mbps) ? ML_BITRATE_DEFAULT_KBPS : mbpsToKbps(mbps);
    return Math.min(Math.max(kbps, ML_BITRATE_MIN_KBPS), ML_BITRATE_MAX_KBPS);
  }

  private mlPreviewWidth(): number {
    return this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlWidthText, this.detectedWidth, MAX_DESKTOP_SIZE) : this.detectedWidth;
  }

  private mlPreviewHeight(): number {
    return this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlHeightText, this.detectedHeight, MAX_DESKTOP_SIZE) : this.detectedHeight;
  }

  private mlAutoBitrateKbps(): number {
    return recommendedBitrateKbps(this.mlPreviewWidth(), this.mlPreviewHeight(), this.mlFps,
      this.mlCodec);
  }

  private mlAutoScalePercent(): number {
    return SettingsStore.scalePresetForResolution(this.mlPreviewWidth(), this.mlPreviewHeight());
  }

  private effectiveDefaultScale(): number {
    return SettingsStore.autoScalePercent(this.outputWidth(), this.outputHeight(),
      this.srEnabled, this.srRatioPercent);
  }

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

  private sessionSizeText(): string {
    const width: number = SettingsStore.srSessionSize(this.outputWidth(), this.srRatioPercent);
    const height: number = SettingsStore.srSessionSize(this.outputHeight(), this.srRatioPercent);
    return `${width} × ${height}`;
  }

  private srSupportedFor(protocol: number): boolean {
    return protocol === PROTOCOL_MOONLIGHT ? this.mlSrSupported : this.srSupported;
  }

  private srReasonFor(protocol: number): string {
    return protocol === PROTOCOL_MOONLIGHT ? this.mlSrReason : this.srReason;
  }

  // The output resolution the 超分 upscales back to: the RDP display size, or Moonlight's stream
  // resolution preview.
  private srOutputWidth(protocol: number): number {
    return protocol === PROTOCOL_MOONLIGHT ? this.mlPreviewWidth() : this.outputWidth();
  }

  private srOutputHeight(protocol: number): number {
    return protocol === PROTOCOL_MOONLIGHT ? this.mlPreviewHeight() : this.outputHeight();
  }

  private sessionSizeTextFor(protocol: number): string {
    const ratio: number = this.srRatioFor(protocol);
    const width: number = SettingsStore.srSessionSize(this.srOutputWidth(protocol), ratio);
    const height: number = SettingsStore.srSessionSize(this.srOutputHeight(protocol), ratio);
    return `${width} × ${height}`;
  }

  private persist(): void {
    const disp = defaultDisplay();
    const maxWidth: number = disp !== undefined ? disp.width : Number.MAX_SAFE_INTEGER;
    const maxHeight: number = disp !== undefined ? disp.height : Number.MAX_SAFE_INTEGER;
    const settings: AppSettings = SettingsStore.load();
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
    settings.scrollEngineFallback = this.scrollEngineFallback;
    settings.toolbarHoverDelay = this.hoverDelay;
    settings.toolbarHideDelay = this.hideDelay;
    settings.toolbarTriggerWidthPercent = this.triggerWidthPercent;
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
      settings.mlBitrateAuto = this.mlBitrateAuto;
      settings.mlBitrateKbps = this.effectiveMlBitrate();
      settings.mlColorSpace = this.mlColorSpace;
      settings.mlCodec = this.mlCodec;
      settings.mlSrEnabled = this.mlSrEnabled && this.mlSrSupported;
      settings.mlSrRatioPercent = SettingsStore.snapSrRatioPreset(this.mlSrRatioPercent);
      settings.mlSrBackend = this.mlSrBackend;
      settings.mlScaleMode = this.mlScaleMode;
      settings.mlScalePercent = this.parseScale(this.mlScaleText, 100);
      settings.mlTouchpadScrollSpeed = this.touchpadScrollSpeed;
      settings.mlTouchpadScrollAlwaysEngine = this.touchpadAlwaysEngine;
      settings.mlPinchZoomSpeed = this.pinchZoomSpeed;
      settings.mlPinchMode = this.pinchMode;
      settings.mlTouchpadMiddleDoubleTap = this.touchpadMiddleDoubleTap;
      settings.mlPinchTouchAngle = this.pinchTouchAngle;
      settings.mlPinchTouchDistance = this.pinchTouchDistance;
      settings.mlCursorMode = this.mlCursorMode;
      settings.mlRelativeMouse = this.mlRelativeMouse;
      settings.mlRelativeSpeed = this.relativeSpeed;
      settings.mlLowLatencyAudio = this.mlLowLatencyAudio && this.audioSupported;
      settings.mlReconnectOnForeground = this.reconnectOnForeground;
      settings.mlWinKeySubstitute = this.winKeySubstitute;
      settings.mlShortcutPassthrough = this.shortcutPassthrough;
      // Audio is one global switch shared by both protocols (see the Moonlight
      // audio toggle), so it is persisted from this branch too.
      settings.enableAudio = this.enableAudio;
    }
    SettingsStore.save(settings);
    if (this.protocol === PROTOCOL_RDP) {
      RdpNative.applyRdpCursor(this.useRdpCursor);
    }
  }

  private confirmReset(): void {
    showConfirm(this.getUIContext(), {
      title: '恢复默认设置',
      message: '将全部设置（RDP、Moonlight 与通用）恢复为默认值，自定义设置会丢失，'
        + '已保存的连接不受影响。是否继续？',
      confirmLabel: '恢复默认',
      danger: true,
      onConfirm: () => {
        this.resetDefaults();
      }
    });
  }

  private resetDefaults(): void {
    const defaults: AppSettings = SettingsStore.defaults();
    if (!this.audioSupported) {
      defaults.enableAudio = false;
      defaults.mlLowLatencyAudio = false;
    }
    SettingsStore.save(defaults);
    this.syncFromStore();
  }

  private confirmExport(): void {
    showConfirm(this.getUIContext(), {
      title: '导出配置',
      message: '将导出全局设置与全部连接；密码保存在系统安全存储中，不会导出。是否继续？',
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
      message: '将应用全局设置，并按连接 ID 覆盖或新增连接；密码需重新输入。是否继续？',
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

  private beginWinCapture(): void {
    this.winSubstituteCapturing = true;
    try {
      this.getUIContext().getFocusController().requestFocus('winSubstituteCapture');
    } catch (error) {
    }
  }

  private captureWinKey(event: KeyEvent): void {
    if (!this.winSubstituteCapturing || event.type !== KeyType.Down) {
      return;
    }
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

  private pinchModeDescription(): string {
    if (this.pinchMode === PINCH_MODE_TOUCH) {
      return '在鼠标位置合成两个原生触点，远端像真实双指一样缩放';
    }
    if (this.pinchMode === PINCH_MODE_ZOOM) {
      return '只在本机放大画面，不向远端发送数据；以光标为中心，最小 100%';
    }
    if (this.pinchMode === PINCH_MODE_NATIVE) {
      return '转发为宿主原生触控板手势，由 Windows 系统处理捏合；仅基地版 Sunshine 支持';
    }
    return '映射为 Ctrl+滚轮，由远端应用自行缩放（会触发远端重绘）';
  }

  // 超分 state is per protocol tab (protocol === PROTOCOL_RDP / PROTOCOL_MOONLIGHT), so one set of
  // builders serves both and only the data source changes.
  private srEnabledFor(protocol: number): boolean {
    return protocol === PROTOCOL_MOONLIGHT ? this.mlSrEnabled : this.srEnabled;
  }

  private srRatioFor(protocol: number): number {
    return protocol === PROTOCOL_MOONLIGHT ? this.mlSrRatioPercent : this.srRatioPercent;
  }

  private srBackendFor(protocol: number): number {
    return protocol === PROTOCOL_MOONLIGHT ? this.mlSrBackend : this.srBackend;
  }

  private srChoiceOf(protocol: number): number {
    if (!this.srEnabledFor(protocol)) {
      return 0;
    }
    return this.srBackendFor(protocol) === SR_BACKEND_FSR ? 2 : 1;
  }

  private srChoiceUsableFor(choice: number, protocol: number): boolean {
    if (choice === 0) {
      return true;
    }
    const backend: number = choice === 2 ? SR_BACKEND_FSR : SR_BACKEND_XENGINE;
    return SettingsStore.srBackendUsable(backend, protocol);
  }

  private srSetRatio(protocol: number, preset: number): void {
    if (protocol === PROTOCOL_MOONLIGHT) {
      this.mlSrRatioPercent = preset;
    } else {
      this.srRatioPercent = preset;
    }
    this.persist();
  }

  private srSetChoice(protocol: number, choice: number): void {
    const enabled: boolean = choice !== 0;
    const backend: number = choice === 2 ? SR_BACKEND_FSR : SR_BACKEND_XENGINE;
    if (protocol === PROTOCOL_MOONLIGHT) {
      this.mlSrEnabled = enabled;
      if (enabled) {
        this.mlSrBackend = backend;
      }
    } else {
      this.srEnabled = enabled;
      if (enabled) {
        this.srBackend = backend;
      }
    }
    this.persist();
  }

  @Builder
  srRatioChip(protocol: number, preset: number) {
    ChoiceChip({
      label: srRatioLabel(preset),
      selected: this.srRatioFor(protocol) === preset,
      onTap: () => {
        this.srSetRatio(protocol, preset);
      }
    })
  }

  @Builder
  srChoiceChip(protocol: number, choice: number) {
    ChoiceChip({
      label: choice === 0 ? '关闭' : (choice === 1 ? 'XEngine' : 'FSR'),
      selected: this.srChoiceOf(protocol) === choice,
      chipEnabled: this.srChoiceUsableFor(choice, protocol),
      onTap: () => {
        this.srSetChoice(protocol, choice);
      }
    })
  }

  // The 超分 block is one shared section; the two protocol tabs only switch the data source.
  @Builder
  srSection(protocol: number) {
    Column() {
      Column() {
        Text('超分辨率')
          .fontSize(14)
          .fontColor(this.srSupportedFor(protocol) ? $r('app.color.text_primary')
            : $r('app.color.text_tertiary'))
        Text('远端按「分辨率 ÷ 倍率」出画，本机用 GPU 上采样回输出分辨率：省带宽与解码')
          .fontSize(11)
          .fontColor($r('app.color.text_tertiary'))
          .margin({ top: 2 })
        if (!this.srSupportedFor(protocol)) {
          Text(this.srReasonFor(protocol))
            .fontSize(11)
            .fontColor($r('app.color.danger'))
            .margin({ top: 2 })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .width('100%')

      Flex({ wrap: FlexWrap.Wrap }) {
        this.srChoiceChip(protocol, 0)
        this.srChoiceChip(protocol, 1)
        this.srChoiceChip(protocol, 2)
      }
      .width('100%').margin({ top: 10 })

      if (this.srEnabledFor(protocol) && this.srSupportedFor(protocol)) {
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(SR_RATIO_PRESETS, (preset: number) => {
            this.srRatioChip(protocol, preset)
          }, (preset: number) => preset.toString())
        }
        .width('100%').margin({ top: 10 })

        Text('输出 ' + this.srOutputWidth(protocol) + ' × ' + this.srOutputHeight(protocol)
          + ' · 会话 ' + this.sessionSizeTextFor(protocol))
          .fontSize(11).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 2 })
      }
    }
    .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)
  }

  @Builder
  mlFpsChip(preset: number) {
    ChoiceChip({
      label: preset.toString(),
      selected: !this.mlFpsCustom && this.mlFps === preset,
      onTap: () => {
        this.mlFpsCustom = false;
        this.mlFps = preset;
        this.mlFpsText = preset.toString();
        this.persist();
      }
    })
  }

  @Builder
  mlFpsCustomChip() {
    ChoiceChip({
      label: '自定义',
      selected: this.mlFpsCustom,
      onTap: () => {
        this.mlFpsCustom = true;
        this.persist();
      }
    })
  }

  private syncFpsText(): void {
    const value: number = Number.parseInt(this.mlFpsText, 10);
    this.mlFps = clampFps(Number.isNaN(value) ? this.mlFps : value);
  }

  @Builder
  mlColorSpaceChip(space: number, label: string) {
    ChoiceChip({
      label: label,
      selected: this.mlColorSpace === space,
      onTap: () => {
        this.mlColorSpace = space;
        this.persist();
      }
    })
  }

  @Builder
  mlBitrateModeChip(label: string, auto: boolean, leftMargin: number) {
    Text(label)
      .fontSize(13)
      .fontColor(this.mlBitrateAuto === auto ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.mlBitrateAuto === auto
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.mlBitrateAuto = auto;
        this.persist();
      })
  }

  private mlCodecSupportOf(codec: number): number {
    return moonlightCodecSupport(codec, this.mlH264Support, this.mlHevcSupport, this.mlAv1Support);
  }

  private mlCodecUsable(codec: number): boolean {
    return moonlightCodecUsable(codec, this.mlH264Support, this.mlHevcSupport, this.mlAv1Support);
  }

  private mlCodecSoftware(codec: number): boolean {
    return moonlightCodecSoftware(codec, this.mlH264Support, this.mlHevcSupport, this.mlAv1Support);
  }

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
  mlResModeChip(mode: number, label: string, leftMargin: number) {
    Text(label)
      .fontSize(13)
      .fontColor(this.mlResMode === mode ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.mlResMode === mode
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.mlResMode = mode;
        this.persist();
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

  private cursorModeDescription(): string {
    if (this.mlCursorMode === ML_CURSOR_OFF) {
      return '不处理光标：画面里是主机画的光标，本机也画一个（可能双重）';
    }
    if (this.mlCursorMode === ML_CURSOR_HIDE_HOST) {
      return '主机光标形状实时同步为本机系统光标（顺滑、无双重）';
    }
    return '隐藏本机系统光标，只显示画面里主机画的光标';
  }

  private switchProtocol(protocol: number): void {
    if (this.protocol === protocol) {
      return;
    }
    this.persist();
    this.protocol = protocol;
    SettingsStore.rememberSettingsTab(protocol);
    this.syncFromStore();
  }

  @Builder
  mlResolutionSection() {
    Column() {
      Row() {
        Column() {
          Text('分辨率').fontSize(14).fontColor($r('app.color.text_primary'))
          Text('自动使用当前显示器的分辨率')
            .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Row() {
          this.mlResModeChip(ML_RES_FOLLOW_SCREEN, '自动', 0)
          this.mlResModeChip(ML_RES_FIXED, '手动', 8)
        }
      }
      .width('100%')

      if (this.mlResMode === ML_RES_FOLLOW_SCREEN) {
        Text('自动检测：' + this.detectedText)
          .fontSize(11).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 10 })
      } else {
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

        Text('自动检测：' + this.detectedText + '，可参考输入')
          .fontSize(11).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 6 })
      }
    }
    .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)
  }

  @Builder
  mlScaleSection() {
    Column() {
      ScaleSetting({
        mode: this.mlScaleMode,
        allowNone: true,
        allowCustomInput: false,
        percentText: this.mlScaleText,
        autoPercent: this.mlAutoScalePercent(),
        subtitle: '修改主机虚拟显示器的显示缩放',
        noneHint: '不向主机发送缩放设置，交给虚拟屏自行决定',
        warning: '仅基地版 Sunshine 有效',
        onModeChange: (mode: number) => {
          this.mlScaleMode = mode;
          this.persist();
        },
        onPercentTextChange: (text: string) => {
          this.mlScaleText = text;
          this.persist();
        }
      })
    }
    .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)
  }

  // 画质项（帧率 / 码率 / 编码器 / 色彩空间）并入「显示」段：它们是同一组画面参数，只是分属
  // 显示链路与编码链路，不另起一个段。
  @Builder
  mlStreamQualityItems() {
    Column() {
      Column() {
        Text('帧率').fontSize(14).fontColor($r('app.color.text_primary'))
        Text('默认 60，可选档位或自定义')
          .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(ML_FPS_PRESETS, (preset: number) => {
            this.mlFpsChip(preset)
          }, (preset: number) => preset.toString())
          this.mlFpsCustomChip()
        }
        .width('100%').margin({ top: 8 })

        if (this.mlFpsCustom) {
          Row() {
            TextInput({ placeholder: '60', text: this.mlFpsText })
              .type(InputType.Number).fontSize(14).height(40).width(140)
              .backgroundColor($r('app.color.input_bg')).borderRadius(10)
              .onChange((value: string) => {
                this.mlFpsText = value;
                this.syncFpsText();
                this.persist();
              })
            Text('fps')
              .fontSize(14).fontColor($r('app.color.text_tertiary'))
              .margin({ left: 10 })
          }
          .width('100%').margin({ top: 4 })
        }
      }
      .width('100%').padding({ top: 8, bottom: 8 }).alignItems(HorizontalAlign.Start)

      Divider().color($r('app.color.divider'))

      Column() {
        Row() {
          Column() {
            Text('码率').fontSize(14).fontColor($r('app.color.text_primary'))
            Text('自动按分辨率与帧率计算，或手动指定')
              .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          Row() {
            this.mlBitrateModeChip('自动', true, 0)
            this.mlBitrateModeChip('手动', false, 8)
          }
        }
        .width('100%')

        if (this.mlBitrateAuto) {
          Text('自动：约 ' + kbpsToMbps(this.mlAutoBitrateKbps()) + ' Mbps')
            .fontSize(11).fontColor($r('app.color.text_tertiary'))
            .width('100%').margin({ top: 10 })
        } else {
          Row() {
            TextInput({ placeholder: '20', text: this.mlBitrateText })
              .type(InputType.Number).fontSize(14).height(40).width(140)
              .backgroundColor($r('app.color.input_bg')).borderRadius(10)
              .onChange((value: string) => {
                this.mlBitrateText = value;
                this.persist();
              })
            Text('Mbps')
              .fontSize(14).fontColor($r('app.color.text_tertiary'))
              .margin({ left: 10 })
          }
          .width('100%').margin({ top: 10 })
        }
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

      Divider().color($r('app.color.divider'))

      Column() {
        Text('色彩空间').fontSize(14).fontColor($r('app.color.text_primary'))
        Flex({ wrap: FlexWrap.Wrap }) {
          this.mlColorSpaceChip(ML_COLORSPACE_REC709, 'Rec709')
          this.mlColorSpaceChip(ML_COLORSPACE_REC601, 'Rec601')
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

            Text('画面参数：新连接默认使用这里的值；可在单个连接的「高级设置」中改为独立配置')
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .margin({ bottom: 8 })

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
              ScaleSetting({
                mode: this.scaleAuto ? ML_SCALE_AUTO : ML_SCALE_MANUAL,
                allowNone: false,
                allowCustomInput: true,
                percentText: this.manualScaleText,
                autoPercent: this.effectiveDefaultScale(),
                subtitle: this.srEnabled
                  ? '超分辨率前会话桌面的缩放；自动档按会话分辨率相对屏幕换算'
                  : '按分辨率相对屏幕的比例自动选择推荐缩放比例',
                onModeChange: (mode: number) => {
                  this.scaleAuto = mode === ML_SCALE_AUTO;
                  this.persist();
                },
                onPercentTextChange: (text: string) => {
                  this.manualScaleText = text;
                  this.persist();
                }
              })
            }
            .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            this.srSection(PROTOCOL_RDP)
            } else if (this.protocol === PROTOCOL_MOONLIGHT) {
              Text('显示')
                .fontSize(13)
                .fontColor($r('app.color.text_secondary'))
                .margin({ bottom: 4 })

              Text('画面与编码参数：新连接默认使用这里的值；可在单个连接的「高级设置」中改为独立配置')
                .fontSize(11)
                .fontColor($r('app.color.text_tertiary'))
                .width('100%')
                .margin({ bottom: 8 })

              Column() {
                Text('光标模式')
                  .fontSize(14)
                  .fontColor($r('app.color.text_primary'))
                Text(this.cursorModeDescription())
                  .fontSize(11)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ top: 2 })
                if (this.mlCursorMode === ML_CURSOR_HIDE_HOST) {
                  Text('此功能仅在基地版 Sunshine（foundation-sunshine）有效；其它主机会回退为「关闭」')
                    .fontSize(11)
                    .fontColor($r('app.color.danger'))
                    .margin({ top: 2 })
                }

                Row() {
                  this.mlCursorModeChip(ML_CURSOR_OFF, '关闭')
                  this.mlCursorModeChip(ML_CURSOR_HIDE_SYSTEM, '隐藏系统光标')
                  this.mlCursorModeChip(ML_CURSOR_HIDE_HOST, '同步系统光标')
                }
                .width('100%')
                .margin({ top: 10, bottom: 6 })

                if (this.mlCursorMode === ML_CURSOR_HIDE_SYSTEM) {
                  SettingToggleRow({
                    title: '相对鼠标模式',
                    subtitle: '发送相对量来移动指针，对于部分游戏很有用。由于系统限制可能会存在二次鼠标加速',
                    value: this.mlRelativeMouse,
                    onToggle: (v: boolean) => {
                      this.mlRelativeMouse = v;
                      this.persist();
                    }
                  })

                  if (this.mlRelativeMouse) {
                    this.sliderItem({
                      title: '相对移动速度',
                      subtitle: '在主机显示缩放的基础上的二次调整，1.0× 为基准',
                      value: this.relativeSpeed,
                      min: MIN_RELATIVE_SPEED,
                      max: MAX_RELATIVE_SPEED,
                      step: 0.05,
                      unit: '×',
                      precision: 2,
                      enabled: true,
                      disabledReason: '',
                      onChange: (v: number) => {
                        this.relativeSpeed = v;
                        this.persist();
                      }
                    })
                  }
                }
              }
              .width('100%')
              .padding({ top: 10, bottom: 14 })
              .alignItems(HorizontalAlign.Start)

              Divider().color($r('app.color.divider'))

              this.mlResolutionSection()

              Divider().color($r('app.color.divider'))

              this.mlScaleSection()

              Divider().color($r('app.color.divider'))

              this.srSection(PROTOCOL_MOONLIGHT)

              Divider().color($r('app.color.divider'))

              this.mlStreamQualityItems()
            }

            if (this.protocol !== TAB_COMMON) {
            Divider().color($r('app.color.divider'))

            Text('触控板')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            if (this.protocol === PROTOCOL_MOONLIGHT) {
              this.toggleItem({
                title: '总是使用系统滑动组件',
                subtitle: '关：主机支持触控板通道（基地版 Sunshine）时走模拟两指滑动，滚动交给 Windows 原生处理；'
                  + '开：一律用系统滚动组件 + 滚轮通道',
                value: this.touchpadAlwaysEngine,
                enabled: true,
                disabledReason: '',
                onChange: (v: boolean) => {
                  this.touchpadAlwaysEngine = v;
                  this.persist();
                }
              })

              Divider().color($r('app.color.divider'))
            }

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
            if (this.pinchMode === PINCH_MODE_NATIVE) {
              Text('此功能仅在基地版 Sunshine（foundation-sunshine）有效；其它主机会回退为「Ctrl 滚轮」')
                .fontSize(11)
                .fontColor($r('app.color.danger'))
                .margin({ top: 2 })
            }

            Row() {
              this.pinchModeChip(PINCH_MODE_WHEEL, 'Ctrl 滚轮')
              this.pinchModeChip(PINCH_MODE_TOUCH, '触摸模拟')
              if (this.protocol === PROTOCOL_RDP) {
                this.pinchModeChip(PINCH_MODE_ZOOM, '直接缩放')
              } else {
                this.pinchModeChip(PINCH_MODE_NATIVE, '原生转发')
              }
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
              subtitle: '触控板双指连续轻触两次发送鼠标中键；开启后双指轻触的右键会有约 300ms 判定延迟，按住右键（拖动）不受影响',
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
            } else if (this.protocol === PROTOCOL_MOONLIGHT) {
            Divider().color($r('app.color.divider'))

            Text('连接特性')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            // One global audio switch shared with RDP (persisted from either
            // protocol branch); gates the Moonlight Opus audio stream.
            this.toggleItem({
              title: '音频重定向',
              subtitle: '播放主机的声音；关闭则音频留在主机本地播放',
              value: this.enableAudio,
              enabled: this.audioSupported,
              disabledReason: this.audioDisabledReason,
              onChange: (v: boolean) => {
                this.enableAudio = v;
                this.persist();
              }
            })

            // Only meaningful while audio is redirected; the system falls back
            // to the normal path when FAST is unavailable.
            if (this.enableAudio) {
              Divider().color($r('app.color.divider'))

              this.toggleItem({
                title: '低延迟音频',
                subtitle: '用系统低时延通路播放主机声音；关闭则与 RDP 走同一条播放链路',
                value: this.mlLowLatencyAudio,
                enabled: this.audioSupported,
                disabledReason: this.audioDisabledReason,
                onChange: (v: boolean) => {
                  this.mlLowLatencyAudio = v;
                  this.persist();
                }
              })
            }
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

            if (this.protocol === TAB_COMMON) {
            Divider().color($r('app.color.divider'))

            Text('重置')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ top: 4, bottom: 4 })

            Text('恢复全部设置（RDP / Moonlight / 通用）为默认值，不影响已保存的连接。')
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .margin({ bottom: 10 })

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

            if (this.protocol === PROTOCOL_RDP && this.developerMode) {
            Divider().color($r('app.color.divider'))

            Text('开发者')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

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

              this.toggleItem({
                title: '滚动：回退直译映射',
                subtitle: '默认由系统滚动组件负责触控板与滚轮的跟手滚动、惯性抛滑与打断（两协议、全部轴向）。'
                  + '仅当需要逐齿精确滚动（特殊操作）时回退：改由内置映射直译，无惯性',
                value: this.scrollEngineFallback,
                enabled: true,
                disabledReason: '',
                onChange: (v: boolean) => {
                  this.scrollEngineFallback = v;
                  this.persist();
                }
              })

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
                title: '工具栏触发区域宽度',
                subtitle: '屏幕顶部中间多宽的一条带触发工具栏，两端（贴边 / 角落）不触发，避免误触',
                value: this.triggerWidthPercent,
                min: 10,
                max: 100,
                step: 10,
                unit: ' %',
                precision: 0,
                enabled: true,
                disabledReason: '',
                onChange: (v: number) => {
                  this.triggerWidthPercent = v;
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
