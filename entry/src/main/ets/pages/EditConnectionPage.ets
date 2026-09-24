/*
 * HmRdp - add / edit a saved connection.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { AdvancedProfile, DisplayProfile, EditConnectionParams, PendingConnection,
  RdpConnectOptions, RdpConnectOrigin, SavedConnection, SR_BACKEND_FSR, SR_BACKEND_XENGINE,
  SrProfile } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { Capability, DeviceCapabilities } from '../services/DeviceCapabilities';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { FPS_PRESETS, SCALE_PRESETS, SR_RATIO_PRESETS, SettingsStore, fpsLabel,
  srRatioLabel } from '../services/SettingsStore';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { TextAction } from '../components/TextAction';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpEdit';

@Entry
@Component
struct EditConnectionPage {
  @State host: string = '';
  @State port: string = '3389';
  @State username: string = '';
  @State password: string = '';
  // Password Vault (see doc_agent/settings-and-storage.md §1.1): the field types
  // below make the pair a login form the system can fill. The app never asks the
  // vault to save, so this only has to be taken away when the page is left — a
  // form that is gone must not offer itself on the way out.
  @State autoFill: boolean = true;
  @State domain: string = '';
  @State desktopWidthText: string = '1920';
  @State desktopHeightText: string = '1080';
  @State scaleText: string = '100';
  @State useGlobalDisplay: boolean = true;
  @State advancedExpanded: boolean = false;
  @State useGlobalAdvanced: boolean = true;
  @State audioSupported: boolean = true;
  private audioDisabledReason: string = '';
  @State enableAudio: boolean = true;
  @State enableGfx: boolean = true;
  // H.264 was removed; kept false so the stored model field stays inert.
  @State enableH264: boolean = false;
  @State ignoreCertificate: boolean = true;
  @State maxFps: number = 0;
  // 超分: kept out of the display group on purpose - a LAN connection can follow
  // the (off) global default while the same host reached through a gateway wants
  // the bandwidth saving, and vice versa.
  @State useGlobalSr: boolean = true;
  @State srEnabled: boolean = false;
  @State srRatioPercent: number = SR_RATIO_PRESETS[0];
  @State srBackend: number = SR_BACKEND_XENGINE;
  @State srUsable: boolean = false;
  @State errorMessage: string = '';
  @State editing: boolean = false;
  @State detectedText: string = '';
  @State connecting: boolean = false;
  private connectionId: string = '';
  // Id of the in-flight attempt (may be a brand-new id for a new connection).
  private activeConnectionId: string = '';
  private storedPassword: string = '';
  private returned: boolean = false;
  // Immersive system material (沉浸光感) for the form card; a system without the
  // material capability keeps the flat card colour below.
  private cardMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.CARD));

  // Tracks this editor's own connection attempt. On failure the page stays put
  // so the typed password is preserved and can be corrected.
  private sessionListener: SessionStateListener = (connectionId: string,
    state: ConnectionState, message: string, origin: RdpConnectOrigin): void => {
    if (origin !== RdpConnectOrigin.Editor || connectionId !== this.activeConnectionId) {
      return;
    }
    if (state === ConnectionState.Connecting) {
      this.connecting = true;
    } else if (state === ConnectionState.Failed) {
      this.connecting = false;
      this.errorMessage = message;
    } else if (state === ConnectionState.Idle || state === ConnectionState.Connected ||
      state === ConnectionState.Disconnecting) {
      this.connecting = false;
    }
  };

  aboutToAppear(): void {
    SessionManager.subscribe(this.sessionListener);
    const audioCapability: Capability = DeviceCapabilities.audio();
    this.audioSupported = audioCapability.supported;
    this.audioDisabledReason = audioCapability.reason;
    const detected: DisplayProfile = SettingsStore.detectedDisplay();
    this.detectedText = detected.valid()
      ? `${detected.width} × ${detected.height}` : '无法获取显示器分辨率';
    this.srUsable = SettingsStore.superResolutionUsable();
    this.desktopWidthText = detected.valid() ? detected.width.toString() : '1920';
    this.desktopHeightText = detected.valid() ? detected.height.toString() : '1080';
    this.scaleText = this.suggestedScale().toString();

    const params: EditConnectionParams =
      this.getUIContext().getRouter().getParams() as EditConnectionParams;
    const id: string = params?.id ?? '';
    this.connectionId = id;
    this.editing = id.length > 0;
    if (!this.editing) {
      return;
    }
    const conn: SavedConnection | undefined = ConnectionStore.get(id);
    if (conn !== undefined) {
      this.host = conn.host;
      this.port = conn.port.toString();
      this.username = conn.username;
      this.domain = conn.domain;
      this.useGlobalDisplay = conn.useGlobalDisplay;
      if (conn.width > 0 && conn.height > 0) {
        this.desktopWidthText = conn.width.toString();
        this.desktopHeightText = conn.height.toString();
      }
      if (conn.scalePercent > 0) {
        this.scaleText = conn.scalePercent.toString();
      }
      this.enableAudio = this.audioSupported && conn.enableAudio;
      this.enableGfx = conn.enableGfx;
      this.ignoreCertificate = conn.ignoreCertificate;
      this.maxFps = SettingsStore.snapFpsPreset(conn.maxFps);
      this.useGlobalAdvanced = conn.useGlobalAdvanced;
      this.useGlobalSr = conn.useGlobalSr;
      this.srEnabled = conn.srEnabled;
      this.srRatioPercent = SettingsStore.snapSrRatioPreset(conn.srRatioPercent);
      this.srBackend = conn.srBackend;
    }
    CredentialStore.loadPassword(id)
      .then((password: string) => {
        this.storedPassword = password;
        this.password = password;
      })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'load password failed: %{public}s', JSON.stringify(error));
      });
  }

  aboutToDisappear(): void {
    SessionManager.unsubscribe(this.sessionListener);
  }

  onBackPress(): boolean {
    // Leaving while connecting cancels the attempt instead of letting it run
    // against a destroyed page.
    this.cancelConnect();
    this.autoFill = false;
    return false;
  }

  private parsePort(): number {
    const value: number = Number.parseInt(this.port, 10);
    return Number.isNaN(value) || value <= 0 ? 3389 : value;
  }

  private parseSize(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    return Number.isNaN(value) || value <= 0 ? fallback : value;
  }

  private parseScale(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    if (Number.isNaN(value) || value < 100) {
      return fallback;
    }
    return Math.min(value, 500);
  }

  private parsedScale(): number {
    const value: number = Number.parseInt(this.scaleText, 10);
    return Number.isNaN(value) ? 0 : value;
  }

  private isPresetScale(value: number): boolean {
    return SCALE_PRESETS.indexOf(value) >= 0;
  }

  private onOff(value: boolean): string {
    return value ? '开' : '关';
  }

  private srSummary(profile: SrProfile): string {
    return profile.enabled ? `${this.onOff(true)} ${srRatioLabel(profile.ratioPercent)}` : '关';
  }

  private advancedSummary(profile: AdvancedProfile): string {
    return `音频${this.onOff(profile.enableAudio)}`
      + ` · 忽略证书${this.onOff(profile.ignoreCertificate)}`
      + ` · 帧率${fpsLabel(profile.maxFps)}`;
  }

  /**
   * 超分 in effect for this connection (the global defaults when it follows them,
   * its own values otherwise). Forced off where the device or the 硬件加速
   * setting cannot back it, so no attempt asks for a lower session resolution
   * than the presenter can upscale back.
   */
  private resolvedSr(): SrProfile {
    const sr: SrProfile = new SrProfile();
    if (this.useGlobalSr) {
      const global: SrProfile = SettingsStore.globalSrProfile();
      sr.enabled = global.enabled;
      sr.ratioPercent = global.ratioPercent;
      sr.backend = global.backend;
    } else {
      sr.enabled = this.srEnabled;
      sr.ratioPercent = SettingsStore.snapSrRatioPreset(this.srRatioPercent);
      sr.backend = this.srBackend;
    }
    if (!this.srUsable) {
      sr.enabled = false;
    }
    return sr;
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
    if (!this.srUsable) {
      return false;
    }
    const token: string = choice === 2 ? 'fsr' : 'xengine';
    return DeviceCapabilities.srBackends().indexOf(token) >= 0;
  }

  /**
   * The scale 自动 would resolve to for what this editor connects with: the
   * requested (output) resolution alone decides it (see
   * `SettingsStore.autoScalePercent`), with this connection's 超分. The global
   * resolution answers when the connection follows the global display, its own
   * width/height otherwise.
   */
  private suggestedScale(): number {
    const sr: SrProfile = this.resolvedSr();
    const fallback: DisplayProfile = SettingsStore.globalDisplayProfile();
    let width: number = fallback.width;
    let height: number = fallback.height;
    if (!this.useGlobalDisplay) {
      width = this.parseSize(this.desktopWidthText, fallback.width);
      height = this.parseSize(this.desktopHeightText, fallback.height);
    }
    return SettingsStore.autoScalePercent(width, height, sr.enabled, sr.ratioPercent);
  }

  /**
   * Builds the options for a connection attempt or for persistence.
   * `applyGlobalAdvanced` is true only for the live attempt: it substitutes the
   * global advanced defaults when the connection is set to follow them, while
   * the connection's own stored values are still persisted for later editing.
   */
  private buildOptions(applyGlobalAdvanced: boolean): RdpConnectOptions | undefined {
    const target: string = this.host.trim();
    if (target.length === 0) {
      this.errorMessage = '请输入主机名或 IP 地址';
      return undefined;
    }
    this.errorMessage = '';
    const global: DisplayProfile = SettingsStore.globalDisplayProfile();
    const options: RdpConnectOptions =
      RdpConnectOptions.of(target, this.parsePort(), this.username.trim(), this.password,
        this.domain.trim());
    options.width = this.useGlobalDisplay
      ? global.width : this.parseSize(this.desktopWidthText, global.width);
    options.height = this.useGlobalDisplay
      ? global.height : this.parseSize(this.desktopHeightText, global.height);
    options.scalePercent = this.useGlobalDisplay
      ? global.scalePercent : this.parseScale(this.scaleText, this.suggestedScale());
    options.enableAudio = this.enableAudio;
    options.enableGfx = this.enableGfx;
    options.enableH264 = false;
    options.ignoreCertificate = this.ignoreCertificate;
    options.maxFps = this.maxFps;
    const sr: SrProfile = this.resolvedSr();
    options.srEnabled = sr.enabled;
    options.srRatioPercent = sr.ratioPercent;
    options.srBackend = sr.backend;
    if (applyGlobalAdvanced && this.useGlobalAdvanced) {
      const advanced: AdvancedProfile = SettingsStore.globalAdvancedProfile();
      options.enableAudio = advanced.enableAudio;
      options.enableGfx = advanced.enableGfx;
      options.enableH264 = false;
      options.ignoreCertificate = advanced.ignoreCertificate;
      options.maxFps = advanced.maxFps;
    }
    if (!this.audioSupported) {
      options.enableAudio = false;
    }
    return options;
  }

  private submit(connect: boolean): void {
    const options: RdpConnectOptions | undefined = this.buildOptions(false);
    if (options === undefined) {
      return;
    }
    const id: string = this.connectionId.length > 0
      ? this.connectionId : util.generateRandomUUID(true);
    const conn: SavedConnection = SavedConnection.fromOptions(id, options);
    conn.useGlobalDisplay = this.useGlobalDisplay;
    conn.useGlobalAdvanced = this.useGlobalAdvanced;
    conn.useGlobalSr = this.useGlobalSr;
    conn.updatedAt = Date.now();
    if (!connect) {
      if (!this.persist(conn)) {
        return;
      }
      this.returnToList();
      return;
    }
    // Connect first; the configuration is persisted only on success so a failed
    // attempt never creates or modifies a saved connection.
    const attempt: RdpConnectOptions | undefined = this.buildOptions(true);
    if (attempt === undefined) {
      return;
    }
    attempt.password = this.password;
    const request: PendingConnection = new PendingConnection();
    request.options = attempt;
    request.connectionId = id;
    request.origin = RdpConnectOrigin.Editor;
    request.onConnected = () => {
      this.persist(conn);
      this.returnToList();
    };
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      this.errorMessage = '无法发起连接';
      return;
    }
    this.activeConnectionId = id;
    this.errorMessage = '';
    this.connecting = true;
    SessionManager.connect(request, context as common.UIAbilityContext);
    // Stay on the editor while connecting: on failure the typed password is
    // preserved so the user can correct it; on success onConnected returns.
  }

  private persist(conn: SavedConnection): boolean {
    try {
      ConnectionStore.save(conn);
      return true;
    } catch (error) {
      this.errorMessage = '保存失败，请重试';
      hilog.error(DOMAIN, TAG, 'save failed: %{public}s', JSON.stringify(error));
      return false;
    }
  }

  private cancelConnect(): void {
    if (!this.connecting || this.activeConnectionId.length === 0) {
      return;
    }
    SessionManager.disconnect(this.activeConnectionId);
  }

  private handleSave(): void {
    if (this.password.length > 0 && this.password !== this.storedPassword) {
      this.getUIContext().showAlertDialog({
      title: '密码不会被保存',
      message: '连接验证正确性后会自动保存密码。',
      systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY),
      primaryButton: {
          value: '取消',
          action: () => {
          }
        },
        secondaryButton: {
          value: '继续保存',
          fontColor: $r('app.color.accent'),
          action: () => {
            this.submit(false);
          }
        }
      });
      return;
    }
    this.submit(false);
  }

  private returnToList(): void {
    if (this.returned) {
      return;
    }
    this.returned = true;
    // The page is done: it is no longer a form anything can be filled into, and
    // an unverified password is left nothing to be offered from.
    this.autoFill = false;
    this.getUIContext().getRouter().back();
  }

  private deleteCurrent(): void {
    if (this.connectionId.length > 0) {
      ConnectionStore.remove(this.connectionId);
      CredentialStore.removePassword(this.connectionId).catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'remove password failed: %{public}s', JSON.stringify(error));
      });
    }
    this.returnToList();
  }

  @Builder
  fieldLabel(text: string) {
    Text(text)
      .fontSize(12)
      .fontColor($r('app.color.text_secondary'))
      .margin({ bottom: 6 })
  }

  @Builder
  fpsChip(preset: number) {
    Text(fpsLabel(preset))
      .fontSize(13)
      .fontColor(this.maxFps === preset ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.maxFps === preset
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.maxFps = preset;
      })
  }

  @Builder
  scaleChip(preset: number) {
    Text(preset.toString() + '%')
      .fontSize(13)
      .fontColor(this.parsedScale() === preset ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.parsedScale() === preset
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.scaleText = preset.toString();
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
      })
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
      })
  }

  @Builder
  optionRow(title: string, subtitle: string, value: boolean,
    onToggle: (v: boolean) => void, enabled: boolean, disabledReason: string) {
    Row() {
      Column() {
        Text(title)
          .fontSize(14)
          .fontColor(enabled ? $r('app.color.text_primary') : $r('app.color.text_tertiary'))
        if (subtitle.length > 0) {
          Text(subtitle).fontSize(11).fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
        if (!enabled && disabledReason.length > 0) {
          Text(disabledReason)
            .fontSize(11)
            .fontColor($r('app.color.danger'))
            .margin({ top: 2 })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Toggle({ type: ToggleType.Switch, isOn: value })
        .enabled(enabled)
        .onChange((isOn: boolean) => {
          if (!enabled) {
            return;
          }
          onToggle(isOn);
        })
    }
    .width('100%')
    .height((subtitle.length > 0 ? 58 : 48) +
      (!enabled && disabledReason.length > 0 ? 20 : 0))
  }

  build() {
    Column() {
      Scroll() {
        Column() {
          Column() {
            Row() {
              TextAction({
                label: '取消',
                onTap: () => {
                  this.cancelConnect();
                  this.returnToList();
                }
              })
              Text(this.editing ? '编辑连接' : '新增连接')
                .fontSize(17)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
                .textAlign(TextAlign.Center)
              TextAction({
                label: '保存',
                onTap: () => {
                  this.handleSave();
                }
              })
            }
            .width('100%')

            Divider()
              .color($r('app.color.divider'))
              .margin({ top: 12, bottom: 20 })

            this.fieldLabel('主机名或 IP 地址')
            TextInput({ placeholder: '例如 192.168.1.100', text: this.host })
              .width('100%')
              .height(46)
              .borderRadius(10)
              .backgroundColor($r('app.color.input_bg'))
              .margin({ bottom: 16 })
              .onChange((value: string) => {
                this.host = value;
              })

            Row() {
              Column() {
                this.fieldLabel('端口')
                TextInput({ placeholder: '3389', text: this.port })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.port = value;
                  })
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column() {
                this.fieldLabel('域（可选）')
                TextInput({ placeholder: 'WORKGROUP', text: this.domain })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .onChange((value: string) => {
                    this.domain = value;
                  })
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .margin({ bottom: 16 })

            Row() {
              Column() {
                this.fieldLabel('用户名')
                TextInput({ placeholder: '用户名', text: this.username })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.USER_NAME)
                  .enableAutoFill(this.autoFill)
                  .onChange((value: string) => {
                    this.username = value;
                  })
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column() {
                this.fieldLabel('密码')
                TextInput({ placeholder: '密码', text: this.password })
                  .width('100%')
                  .height(46)
                  .borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Password)
                  .enableAutoFill(this.autoFill)
                  .onChange((value: string) => {
                    this.password = value;
                  })
                Text('连接成功后自动保存')
                  .fontSize(11)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ top: 4 })
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .margin({ bottom: 16 })

            if (this.errorMessage.length > 0) {
              Text(this.errorMessage)
                .fontSize(13)
                .fontColor($r('app.color.danger'))
                .width('100%')
                .margin({ top: 12 })
            }

            Row() {
              if (this.editing) {
                Button('删除')
                  .layoutWeight(1)
                  .height(48)
                  .fontSize(16)
                  .fontColor($r('app.color.danger'))
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(12)
                  .enabled(!this.connecting)
                  .onClick(() => {
                    this.deleteCurrent();
                  })
                Button() {
                  Row() {
                    if (this.connecting) {
                      LoadingProgress()
                        .width(18)
                        .height(18)
                        .color('#FFFFFF')
                    }
                    Text(this.connecting ? '取消' : '连接')
                      .fontSize(16)
                      .fontWeight(FontWeight.Medium)
                      .fontColor('#FFFFFF')
                      .margin({ left: this.connecting ? 8 : 0 })
                  }
                  .justifyContent(FlexAlign.Center)
                  .alignItems(VerticalAlign.Center)
                }
                .layoutWeight(2)
                .height(48)
                .backgroundColor($r('app.color.accent'))
                .borderRadius(12)
                .margin({ left: 12 })
                .onClick(() => {
                  if (this.connecting) {
                    this.cancelConnect();
                  } else {
                    this.submit(true);
                  }
                })
              } else {
                Button() {
                  Row() {
                    if (this.connecting) {
                      LoadingProgress()
                        .width(18)
                        .height(18)
                        .color('#FFFFFF')
                    }
                    Text(this.connecting ? '取消' : '连接')
                      .fontSize(16)
                      .fontWeight(FontWeight.Medium)
                      .fontColor('#FFFFFF')
                      .margin({ left: this.connecting ? 8 : 0 })
                  }
                  .justifyContent(FlexAlign.Center)
                  .alignItems(VerticalAlign.Center)
                }
                .width('100%')
                .height(48)
                .backgroundColor($r('app.color.accent'))
                .borderRadius(12)
                .onClick(() => {
                  if (this.connecting) {
                    this.cancelConnect();
                  } else {
                    this.submit(true);
                  }
                })
              }
            }
            .width('100%')
            .margin({ top: 20 })

            Divider()
              .color($r('app.color.divider'))
              .margin({ top: 20, bottom: 8 })

            Row() {
              Column() {
                Text('高级设置').fontSize(14).fontColor($r('app.color.text_primary'))
                Text('默认跟随全局设置，展开后可单独覆盖')
                  .fontSize(11)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Row() {
                if (!this.useGlobalDisplay || !this.useGlobalAdvanced || !this.useGlobalSr) {
                  Text('已自定义')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ right: 8 })
                }
                Text(this.advancedExpanded ? '收起' : '展开')
                  .fontSize(13)
                  .fontColor($r('app.color.accent'))
              }
              .alignItems(VerticalAlign.Center)
            }
            .width('100%')
            .padding({ top: 8, bottom: 8 })
            .borderRadius(8)
            .hoverEffect(HoverEffect.Highlight)
            .onClick(() => {
              this.advancedExpanded = !this.advancedExpanded;
            })

            if (this.advancedExpanded) {
              Column() {
                Row() {
                  Column() {
                    Text('使用全局显示设置').fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('分辨率与缩放跟随全局设置')
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .margin({ top: 2 })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: this.useGlobalDisplay })
                    .selectedColor($r('app.color.accent'))
                    .onChange((isOn: boolean) => {
                      this.useGlobalDisplay = isOn;
                    })
                }
                .width('100%')
                .height(58)

                if (this.useGlobalDisplay) {
                  Text('自动检测：' + this.detectedText + ' · 推荐缩放 '
                    + this.suggestedScale() + '%')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .width('100%')
                } else {
                  Row() {
                    Column() {
                      this.fieldLabel('分辨率宽度')
                      TextInput({ placeholder: '1920', text: this.desktopWidthText })
                        .width('100%')
                        .height(46)
                        .borderRadius(10)
                        .backgroundColor($r('app.color.input_bg'))
                        .type(InputType.Number)
                        .onChange((value: string) => {
                          this.desktopWidthText = value;
                        })
                    }
                    .layoutWeight(1)
                    .alignItems(HorizontalAlign.Start)

                    Column() {
                      this.fieldLabel('分辨率高度')
                      TextInput({ placeholder: '1080', text: this.desktopHeightText })
                        .width('100%')
                        .height(46)
                        .borderRadius(10)
                        .backgroundColor($r('app.color.input_bg'))
                        .type(InputType.Number)
                        .onChange((value: string) => {
                          this.desktopHeightText = value;
                        })
                    }
                    .layoutWeight(1)
                    .margin({ left: 16 })
                    .alignItems(HorizontalAlign.Start)
                  }
                  .width('100%')
                  .margin({ top: 12 })

                  Text('自动检测：' + this.detectedText)
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .width('100%')
                    .margin({ top: 6 })

                  Column() {
                    this.fieldLabel('缩放比例')
                    Flex({ wrap: FlexWrap.Wrap }) {
                      ForEach(SCALE_PRESETS, (preset: number) => {
                        this.scaleChip(preset)
                      }, (preset: number) => preset.toString())
                    }
                    .width('100%')
                    .margin({ top: 6 })

                    TextInput({ placeholder: '100', text: this.scaleText })
                      .width('100%')
                      .height(46)
                      .borderRadius(10)
                      .backgroundColor($r('app.color.input_bg'))
                      .type(InputType.Number)
                      .onChange((value: string) => {
                        this.scaleText = value;
                      })

                    if (this.parsedScale() > 0 && !this.isPresetScale(this.parsedScale())) {
                      Text('自定义缩放比例可能导致部分应用显示模糊，建议使用上方固定档位')
                        .fontSize(11)
                        .fontColor($r('app.color.danger'))
                        .margin({ top: 6 })
                    } else {
                      Text('推荐 ' + this.suggestedScale() + '%')
                        .fontSize(11)
                        .fontColor($r('app.color.text_tertiary'))
                        .margin({ top: 4 })
                    }
                  }
                  .width('100%')
                  .margin({ top: 16 })
                  .alignItems(HorizontalAlign.Start)
                }

                Divider()
                  .color($r('app.color.divider'))
                  .margin({ top: 8, bottom: 4 })

                Row() {
                  Column() {
                    Text('使用全局超分辨率设置').fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('超分辨率：远端按输出分辨率÷倍率出画，本机 GPU 上采样回输出分辨率')
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .margin({ top: 2 })
                    if (!this.srUsable) {
                      Text('本设备或当前设置不满足超分辨率条件（需开启硬件加速且设备支持）')
                        .fontSize(11)
                        .fontColor($r('app.color.danger'))
                        .margin({ top: 2 })
                    }
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: this.useGlobalSr })
                    .enabled(this.srUsable)
                    .selectedColor($r('app.color.accent'))
                    .onChange((isOn: boolean) => {
                      if (!this.srUsable) {
                        return;
                      }
                      this.useGlobalSr = isOn;
                    })
                }
                .width('100%')
                .height(58)

                if (this.useGlobalSr) {
                  Text('全局：' + this.srSummary(SettingsStore.globalSrProfile()))
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .width('100%')
                } else {
                  Text('超分辨率').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('会话按输出分辨率÷倍率取流，本机上采样回输出分辨率')
                    .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })

                  // 超分 is a three-way choice: off, XEngine, or the presenter's own
                  // FSR; a backend this build/device cannot run is dimmed.
                  Flex({ wrap: FlexWrap.Wrap }) {
                    this.srChoiceChip(0)
                    this.srChoiceChip(1)
                    this.srChoiceChip(2)
                  }
                  .width('100%').margin({ top: 8, bottom: 8 })

                  if (this.srEnabled) {
                    Flex({ wrap: FlexWrap.Wrap }) {
                      ForEach(SR_RATIO_PRESETS, (preset: number) => {
                        this.srRatioChip(preset)
                      }, (preset: number) => preset.toString())
                    }
                    .width('100%').margin({ bottom: 8 })
                  }
                }

                Divider()
                  .color($r('app.color.divider'))
                  .margin({ top: 8, bottom: 4 })

                Row() {
                  Column() {
                    Text('使用全局高级设置').fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('音频、图形管道等跟随全局设置')
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .margin({ top: 2 })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: this.useGlobalAdvanced })
                    .selectedColor($r('app.color.accent'))
                    .onChange((isOn: boolean) => {
                      this.useGlobalAdvanced = isOn;
                    })
                }
                .width('100%')
                .height(58)

                if (this.useGlobalAdvanced) {
                  Text('全局：' + this.advancedSummary(SettingsStore.globalAdvancedProfile()))
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .width('100%')
                } else {
                  this.optionRow('音频重定向', '', this.enableAudio, (v: boolean) => {
                    this.enableAudio = v;
                  }, this.audioSupported, this.audioDisabledReason)
                  this.optionRow('忽略证书校验', '仅用于自签名证书的测试环境', this.ignoreCertificate,
                    (v: boolean) => {
                      this.ignoreCertificate = v;
                    }, true, '')

                  Column() {
                    Text('帧率上限').fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('超出上限的帧不会被解码（下次连接生效）')
                      .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
                    Flex({ wrap: FlexWrap.Wrap }) {
                      ForEach(FPS_PRESETS, (preset: number) => {
                        this.fpsChip(preset)
                      }, (preset: number) => preset.toString())
                    }
                    .width('100%').margin({ top: 10 })
                  }
                  .width('100%').margin({ top: 16 }).alignItems(HorizontalAlign.Start)
                }
              }
              .width('100%')
              .alignItems(HorizontalAlign.Start)
            }
          }
          .width('100%')
          .constraintSize({ maxWidth: 720 })
          .padding(28)
          .backgroundColor(SystemMaterial.supported ? Color.Transparent : $r('app.color.card_bg'))
          .borderRadius(20)
          .alignItems(HorizontalAlign.Start)
          .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
          .attributeModifier(this.cardMaterial)
        }
        .width('100%')
        .padding({ top: 32, bottom: 32, left: 24, right: 24 })
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
