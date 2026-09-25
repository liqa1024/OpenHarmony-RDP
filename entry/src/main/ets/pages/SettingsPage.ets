/*
 * HmRdp - global settings.
 */
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import { DisplayProfile, SR_BACKEND_FSR, SR_BACKEND_XENGINE } from '../model/RdpModels';
import { ConfigTransfer, ConfigTransferResult } from '../services/ConfigTransfer';
import { Capability, DeviceCapabilities } from '../services/DeviceCapabilities';
import { RdpNative } from '../services/RdpNative';
import { AppSettings, DEFAULT_PINCH_TOUCH_DISTANCE, SCALE_PRESETS, SR_RATIO_PRESETS,
  SettingsStore, srRatioLabel } from '../services/SettingsStore';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { DEFAULT_SPEED_SCALE, MAX_SPEED_SCALE, MIN_SPEED_SCALE } from '../services/TouchpadWheel';
import { KeyMapper } from '../utils/KeyMapper';
import { TextAction } from '../components/TextAction';

// Remote desktop sizes are not limited to the local screen, so manual
// resolution is capped only to a sane upper bound.
const MAX_DESKTOP_SIZE: number = 16384;

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
  @State pinchAsTouch: boolean = false;
  @State pinchTouchAngle: number = 30;
  @State pinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  @State touchHighRate: boolean = false;
  @State useRdpCursor: boolean = true;
  @State reconnectOnForeground: boolean = false;
  @State winKeySubstitute: number = 0;
  @State winSubstituteCapturing: boolean = false;
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
  @State enableGfx: boolean = true;
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
  @State rfxDump: boolean = false;
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
  // Immersive system material (沉浸光感) for the settings card and the two
  // confirmation dialogs; both fall back to the flat colours below on a system
  // without the material capability.
  private cardMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.CARD));
  private dialogMaterial: SystemUiMaterial | undefined = SystemMaterial.of(MaterialSurface.OVERLAY);

  aboutToAppear(): void {
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
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.touchpadScrollSpeed = settings.touchpadScrollSpeed;
    this.pinchZoomSpeed = settings.pinchZoomSpeed;
    this.pinchAsTouch = settings.pinchAsTouch;
    this.pinchTouchAngle = settings.pinchTouchAngle;
    this.pinchTouchDistance = settings.pinchTouchDistance;
    this.touchHighRate = settings.touchHighRate;
    this.useRdpCursor = settings.useRdpCursor;
    this.reconnectOnForeground = settings.reconnectOnForeground;
    RdpNative.applyRdpCursor(this.useRdpCursor);
    this.winKeySubstitute = settings.winKeySubstitute;
    this.resolutionAuto = settings.resolutionAuto;
    this.manualWidthText = this.sizeText(settings.manualWidth);
    this.manualHeightText = this.sizeText(settings.manualHeight);
    this.scaleAuto = settings.scaleAuto;
    this.mainSizeEnabled = settings.mainWindowSizeEnabled;
    this.sessionSizeEnabled = settings.sessionWindowSizeEnabled;
    this.sessionFullscreen = settings.sessionFullscreen;
    this.autoHideMainWindow = settings.autoHideMainWindow;
    // An unsupported device never requests audio; drop a previously stored
    // "on" value so the setting matches the greyed-out switch.
    this.enableAudio = this.audioSupported ? settings.enableAudio : false;
    this.enableGfx = settings.enableGfx;
    // An unsupported device never asks for GPU decoding; drop a previously stored
    // "on" value so the setting matches the greyed-out switch.
    this.hardwareAccel = this.hardwareAccelSupported ? settings.hardwareAccel : false;
    // 超分 rides on the Vulkan presenter, so it is only offered where both the
    // device and the 硬件加速 setting allow it; a stored "on" value that no longer
    // can apply is dropped, exactly like the audio / hardware-acceleration rows.
    this.srEnabled = (this.hardwareAccelSupported && this.srSupported) ? settings.srEnabled : false;
    this.srRatioPercent = SettingsStore.snapSrRatioPreset(settings.srRatioPercent);
    this.srBackend = settings.srBackend;
    this.manualScaleText = settings.manualScalePercent > 0
      ? settings.manualScalePercent.toString() : this.effectiveDefaultScale().toString();
    this.ignoreCertificate = settings.ignoreCertificate;
    this.developerMode = settings.developerMode;
    // With developer options off, the dev-only tools are not shown and must not
    // keep running: a capture left on from a previous session is switched off.
    this.rfxDump = this.developerMode ? settings.rfxDump : false;
    RdpNative.applyHardwareAccel(this.hardwareAccel && this.hardwareAccelSupported);
    this.mainWidthText = this.sizeText(settings.mainWindowWidth);
    this.mainHeightText = this.sizeText(settings.mainWindowHeight);
    this.sessionWidthText = this.sizeText(settings.sessionWindowWidth);
    this.sessionHeightText = this.sizeText(settings.sessionWindowHeight);
    if (!this.audioSupported && settings.enableAudio) {
      this.persist();
    }
    if (!this.hardwareAccelSupported && settings.hardwareAccel) {
      this.persist();
    }
    if ((!this.hardwareAccelSupported || !this.srSupported) && settings.srEnabled) {
      this.persist();
    }
    if (!this.developerMode && settings.rfxDump) {
      this.persist();
    }
    // Keep the native capture in step with what the page now shows: with developer
    // options off it must not be running, whatever the stored value was.
    this.applyRfxDumpSetting();
  }

  // Applies the dev capture toggle immediately (before the next session) so the
  // cmd stream is captured from the first frame; a no-op when the value is
  // unchanged on the native side.
  private applyRfxDumpSetting(): void {
    let dir: string = '';
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx !== undefined) {
        dir = ctx.filesDir;
      }
    } catch (error) {
      dir = '';
    }
    RdpNative.applyRfxDump(this.rfxDump, dir);
  }

  private sizeText(value: number): string {
    return value > 0 ? value.toString() : '';
  }

  private parseSize(text: string, fallback: number, max: number): number {
    const value: number = Number.parseInt(text, 10);
    if (Number.isNaN(value) || value <= 0) {
      return fallback;
    }
    return Math.min(value, max);
  }

  private parseScale(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    if (Number.isNaN(value) || value < 100) {
      return fallback;
    }
    return Math.min(value, 500);
  }

  private parsedManualScale(): number {
    const value: number = Number.parseInt(this.manualScaleText, 10);
    return Number.isNaN(value) ? 0 : value;
  }

  private isPresetScale(value: number): boolean {
    return SCALE_PRESETS.indexOf(value) >= 0;
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

  private displayOrUndefined(): display.Display | undefined {
    try {
      return display.getDefaultDisplaySync();
    } catch (error) {
      return undefined;
    }
  }

  private persist(): void {
    const disp: display.Display | undefined = this.displayOrUndefined();
    const maxWidth: number = disp !== undefined ? disp.width : Number.MAX_SAFE_INTEGER;
    const maxHeight: number = disp !== undefined ? disp.height : Number.MAX_SAFE_INTEGER;
    const settings: AppSettings = new AppSettings();
    settings.toolbarHoverDelay = this.hoverDelay;
    settings.toolbarHideDelay = this.hideDelay;
    settings.touchpadScrollSpeed = this.touchpadScrollSpeed;
    settings.pinchZoomSpeed = this.pinchZoomSpeed;
    settings.pinchAsTouch = this.pinchAsTouch;
    settings.pinchTouchAngle = this.pinchTouchAngle;
    settings.pinchTouchDistance = this.pinchTouchDistance;
    settings.touchHighRate = this.touchHighRate;
    settings.useRdpCursor = this.useRdpCursor;
    settings.reconnectOnForeground = this.reconnectOnForeground;
    settings.winKeySubstitute = this.winKeySubstitute;
    settings.resolutionAuto = this.resolutionAuto;
    settings.manualWidth =
      this.parseSize(this.manualWidthText, this.detectedWidth, MAX_DESKTOP_SIZE);
    settings.manualHeight =
      this.parseSize(this.manualHeightText, this.detectedHeight, MAX_DESKTOP_SIZE);
    settings.scaleAuto = this.scaleAuto;
    settings.manualScalePercent = this.parseScale(this.manualScaleText, this.effectiveDefaultScale());
    settings.sessionFullscreen = this.sessionFullscreen;
    settings.autoHideMainWindow = this.autoHideMainWindow;
    settings.enableAudio = this.enableAudio;
    settings.enableGfx = this.enableGfx;
    settings.ignoreCertificate = this.ignoreCertificate;
    settings.hardwareAccel = this.hardwareAccel;
    settings.srEnabled = this.srEnabled && this.srSupported;
    settings.srRatioPercent = SettingsStore.snapSrRatioPreset(this.srRatioPercent);
    settings.srBackend = this.srBackend;
    settings.developerMode = this.developerMode;
    settings.rfxDump = this.rfxDump;
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
    SettingsStore.save(settings);
    // Apply immediately so a toggle takes effect for sessions opened next,
    // without waiting for an app restart.
    RdpNative.applyRdpCursor(this.useRdpCursor);
    RdpNative.applyHardwareAccel(this.hardwareAccel);
  }

  private resetDefaults(): void {
    const defaults: AppSettings = SettingsStore.defaults();
    this.hoverDelay = defaults.toolbarHoverDelay;
    this.hideDelay = defaults.toolbarHideDelay;
    this.touchpadScrollSpeed = defaults.touchpadScrollSpeed;
    this.pinchZoomSpeed = defaults.pinchZoomSpeed;
    this.pinchAsTouch = defaults.pinchAsTouch;
    this.pinchTouchAngle = defaults.pinchTouchAngle;
    this.pinchTouchDistance = defaults.pinchTouchDistance;
    this.touchHighRate = defaults.touchHighRate;
    this.useRdpCursor = defaults.useRdpCursor;
    this.reconnectOnForeground = defaults.reconnectOnForeground;
    this.winKeySubstitute = defaults.winKeySubstitute;
    this.resolutionAuto = true;
    this.manualWidthText = this.sizeText(this.detectedWidth);
    this.manualHeightText = this.sizeText(this.detectedHeight);
    this.scaleAuto = true;
    this.mainSizeEnabled = defaults.mainWindowSizeEnabled;
    this.sessionSizeEnabled = defaults.sessionWindowSizeEnabled;
    this.sessionFullscreen = defaults.sessionFullscreen;
    this.autoHideMainWindow = defaults.autoHideMainWindow;
    this.enableAudio = this.audioSupported ? defaults.enableAudio : false;
    this.enableGfx = defaults.enableGfx;
    this.ignoreCertificate = defaults.ignoreCertificate;
    this.hardwareAccel = defaults.hardwareAccel;
    this.srEnabled = defaults.srEnabled;
    this.srRatioPercent = defaults.srRatioPercent;
    this.srBackend = defaults.srBackend;
    this.manualScaleText = this.effectiveDefaultScale().toString();
    this.developerMode = defaults.developerMode;
    this.rfxDump = defaults.rfxDump;
    this.mainWidthText = this.sizeText(defaults.mainWindowWidth);
    this.mainHeightText = this.sizeText(defaults.mainWindowHeight);
    this.sessionWidthText = this.sizeText(defaults.sessionWindowWidth);
    this.sessionHeightText = this.sizeText(defaults.sessionWindowHeight);
    this.persist();
    // A reset turns developer options off: stop a capture that was running.
    this.applyRfxDumpSetting();
  }

  private confirmExport(): void {
    this.getUIContext().showAlertDialog({
      title: '导出配置',
      message: '将导出全局设置和全部连接。密码保存在系统安全存储中，出于安全考虑不会导出。',
      systemMaterial: this.dialogMaterial,
      primaryButton: {
        value: '取消',
        action: () => {
        }
      },
      secondaryButton: {
        value: '导出',
        fontColor: $r('app.color.accent'),
        action: () => {
          this.handleExport();
        }
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
    this.getUIContext().showAlertDialog({
      title: '导入配置',
      message: '导入会应用全局设置，并按连接 ID 覆盖或新增连接。密码不会被导入，需重新输入。是否继续？',
      systemMaterial: this.dialogMaterial,
      primaryButton: {
        value: '取消',
        action: () => {
        }
      },
      secondaryButton: {
        value: '导入',
        fontColor: $r('app.color.accent'),
        action: () => {
          this.handleImport();
        }
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

  @Builder
  srRatioChip(preset: number) {
    Text(srRatioLabel(preset))
      .fontSize(13)
      .fontColor(this.srRatioPercent === preset ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.srRatioPercent === preset
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.srRatioPercent = preset;
        this.persist();
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
    Text(choice === 0 ? '关闭' : (choice === 1 ? 'XEngine' : 'FSR'))
      .fontSize(13)
      .fontColor(this.srChoice() === choice ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.srChoice() === choice
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .opacity(this.srChoiceUsable(choice) ? 1.0 : 0.4)
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        if (!this.srChoiceUsable(choice)) {
          return;
        }
        this.srEnabled = choice !== 0;
        if (choice !== 0) {
          this.srBackend = choice === 2 ? SR_BACKEND_FSR : SR_BACKEND_XENGINE;
        }
        this.persist();
      })
  }

  @Builder
  scaleChip(preset: number) {
    Text(preset.toString() + '%')
      .fontSize(13)
      .fontColor(this.parsedManualScale() === preset ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.parsedManualScale() === preset
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.manualScaleText = preset.toString();
        this.persist();
      })
  }

  @Builder
  toggleItem(item: ToggleItemConfig) {
    Row() {
      Column() {
        Text(item.title)
          .fontSize(14)
          .fontColor(item.enabled ? $r('app.color.text_primary') : $r('app.color.text_tertiary'))
        if (item.subtitle.length > 0) {
          Text(item.subtitle).fontSize(11).fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
        if (!item.enabled && item.disabledReason.length > 0) {
          Text(item.disabledReason)
            .fontSize(11)
            .fontColor($r('app.color.danger'))
            .margin({ top: 2 })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Toggle({ type: ToggleType.Switch, isOn: item.value })
        .selectedColor($r('app.color.accent'))
        .enabled(item.enabled)
        .onChange((isOn: boolean) => {
          if (!item.enabled) {
            return;
          }
          item.onChange(isOn);
        })
    }
    .width('100%')
    .height((item.subtitle.length > 0 ? 58 : 48) +
      (!item.enabled && item.disabledReason.length > 0 ? 20 : 0))
  }

  @Builder
  sliderItem(item: SliderItemConfig) {
    Column() {
      Row() {
        Column() {
          Text(item.title).fontSize(14)
            .fontColor(item.enabled ? $r('app.color.text_primary') : $r('app.color.text_tertiary'))
          Text(item.subtitle).fontSize(11).fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
          if (!item.enabled && item.disabledReason.length > 0) {
            Text(item.disabledReason).fontSize(11).fontColor($r('app.color.danger'))
              .margin({ top: 2 })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(item.value.toFixed(item.precision) + item.unit)
          .fontSize(14)
          .fontColor(item.enabled ? $r('app.color.accent') : $r('app.color.text_tertiary'))
      }
      .width('100%')

      Slider({ value: item.value, min: item.min, max: item.max, step: item.step })
        .enabled(item.enabled)
        .width('100%')
        .selectedColor(item.enabled ? $r('app.color.accent') : $r('app.color.text_tertiary'))
        .trackColor(item.enabled ? $r('app.color.input_bg') : $r('app.color.divider'))
        .blockColor(item.enabled ? $r('app.color.accent') : $r('app.color.text_tertiary'))
        .margin({ top: 6 })
        .onChange((newValue: number, mode: SliderChangeMode) => {
          const factor: number = Math.pow(10, item.precision);
          item.onChange(Math.round(newValue * factor) / factor);
        })
    }
    .width('100%')
    .padding({ top: 14, bottom: 14 })
  }

  build() {
    Column() {
      Row() {
        TextAction({
          label: '返回',
          onTap: () => {
            this.getUIContext().getRouter().back();
          }
        })
        Text('设置')
          .fontSize(17)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
          .textAlign(TextAlign.Center)
        TextAction({
          label: '重置',
          onTap: () => {
            this.resetDefaults();
          }
        })
      }
      .width('100%')
      .padding({ left: 24, right: 24, top: 16, bottom: 12 })

      Divider().color($r('app.color.divider'))

      Scroll() {
        Column() {
          Column() {
            Text('会话交互')
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
              enabled: !this.pinchAsTouch,
              disabledReason: '触摸模拟模式下为 1:1 缩放，此速度不生效',
              onChange: (v: number) => {
                this.pinchZoomSpeed = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            this.toggleItem({
              title: '使用触摸模拟触控板捏合',
              subtitle: '在鼠标位置用原生双指触摸缩放；关闭后则映射为 Ctrl+滚轮',
              value: this.pinchAsTouch,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.pinchAsTouch = v;
                this.persist();
              }
            })

            if (this.pinchAsTouch) {
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

            Text('显示')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

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

            Divider().color($r('app.color.divider'))

            Text('窗口大小')
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

            Text('将全局设置和全部连接导出为一个文件，或从文件导入并覆盖。密码保存在系统安全存储中，不会被导出，导入后需重新输入。')
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

            Divider().color($r('app.color.divider'))

            Text('开发者选项')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ top: 4, bottom: 4 })

            Text('调试与性能分析用的功能；正常使用保持关闭')
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .margin({ bottom: 8 })

            this.toggleItem({
              title: '开发者选项',
              subtitle: '显示码流抓取、解码并行宽度、录像回放入口、工具栏性能信息与下面这些实验/调参项',
              value: this.developerMode,
              enabled: true,
              disabledReason: '',
              onChange: (v: boolean) => {
                this.developerMode = v;
                // Turning the gate off also stops the tools it revealed, so a
                // capture cannot keep running invisibly.
                if (!v && this.rfxDump) {
                  this.rfxDump = false;
                  this.persist();
                  this.applyRfxDumpSetting();
                } else {
                  this.persist();
                }
              }
            })

            if (this.developerMode) {
              Divider().color($r('app.color.divider'))

              this.toggleItem({
                title: '抓取 RFX 码流（测试）',
                subtitle: '抓取会话的 RFX 压缩流（hmrdp_rfx.bin）与全部 GFX 命令流（hmrdp_gfx.bin），供开发/回放使用',
                value: this.rfxDump,
                enabled: true,
                disabledReason: '',
                onChange: (v: boolean) => {
                  this.rfxDump = v;
                  this.persist();
                  this.applyRfxDumpSetting();
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
          .width('100%')
          .constraintSize({ maxWidth: 720 })
          .padding(24)
          .backgroundColor(SystemMaterial.supported ? Color.Transparent : $r('app.color.card_bg'))
          .borderRadius(20)
          .alignItems(HorizontalAlign.Start)
          .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
          .attributeModifier(this.cardMaterial)
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
