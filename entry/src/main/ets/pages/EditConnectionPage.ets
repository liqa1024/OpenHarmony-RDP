import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { RdpConnectOptions } from '../model/RdpModels';
import { AdvancedProfile, DisplayProfile,
  SR_BACKEND_FSR, SR_BACKEND_XENGINE, SrProfile, SrSetting } from '../model/Profiles';
import { ConnectOrigin, EditConnectionParams,
  MoonlightConnection, PendingConnection, PROTOCOL_MOONLIGHT, PROTOCOL_RDP,
  RdpConnection, SavedConnection } from '../model/Connections';
import {
  ML_BITRATE_DEFAULT_KBPS, ML_BITRATE_MAX_KBPS,
  ML_BITRATE_MIN_KBPS, ML_CODEC_AUTO, ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC,
  ML_COLORSPACE_REC601, ML_COLORSPACE_REC709,
  ML_DEFAULT_PORT, ML_FPS_PRESETS, ML_RES_FIXED, ML_RES_FOLLOW_SCREEN,
  ML_SCALE_AUTO, ML_SCALE_MANUAL, MoonlightProfile
} from '../moonlight/MoonlightModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { PairResult } from '../moonlight/MoonlightModels';
import { Capability, CODEC_SUPPORT_NONE, DeviceCapabilities } from '../services/DeviceCapabilities';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { MoonlightConnector, MoonlightHostPolicy, MoonlightKnownHost, MoonlightPairResult }
  from '../services/MoonlightConnector';
import { AppSettings, SR_RATIO_PRESETS, SettingsStore,
  srRatioLabel } from '../services/SettingsStore';
import { parseScaleValue, parseSizeValue } from '../utils/ScaleParse';
import { moonlightCodecSoftware, moonlightCodecSupport, moonlightCodecUsable } from '../utils/MoonlightCodec';
import { clampFps, kbpsToMbps, mbpsToKbps, recommendedBitrateKbps } from '../utils/MoonlightQuality';
import { showConfirm, showToast } from '../utils/UiUtil';
import { releaseFeedback } from '../utils/FeedbackHost';
import { promptPairingPin } from '../utils/PairingPrompt';
import { showHostPicker } from '../utils/HostSearchPrompt';
import { MdnsDiscovery, MdnsHost } from '../services/MdnsDiscovery';
import { ChoiceChip } from '../components/ChoiceChip';
import { FormField } from '../components/FormField';
import { PageHeader } from '../components/PageHeader';
import { ContentCard } from '../components/ContentCard';
import { ProtocolTabs } from '../components/ProtocolTabs';
import { ScaleSetting } from '../components/ScaleSetting';
import { SettingToggleRow } from '../components/SettingToggleRow';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpEdit';
const RDP_DEFAULT_PORT: number = 3389;
const MOONLIGHT_SERVICE_TYPE: string = '_nvstream._tcp';
const HOST_SEARCH_MS: number = 3000;

@Entry
@Component
struct EditConnectionPage {
  @State name: string = '';
  @State host: string = '';
  @State port: string = '3389';
  @State username: string = '';
  @State password: string = '';
  @State autoFill: boolean = true;
  @State domain: string = '';
  @State desktopWidthText: string = '1920';
  @State desktopHeightText: string = '1080';
  @State scaleText: string = '100';
  @State useGlobalDisplay: boolean = true;
  @State displayResolutionAuto: boolean = true;
  @State displayScaleAuto: boolean = true;
  @State advancedExpanded: boolean = false;
  @State useGlobalAdvanced: boolean = true;
  @State audioSupported: boolean = true;
  private audioDisabledReason: string = '';
  @State enableAudio: boolean = true;
  @State ignoreCertificate: boolean = true;
  @State useGlobalSr: boolean = true;
  @State srEnabled: boolean = false;
  @State srRatioPercent: number = SR_RATIO_PRESETS[0];
  @State srBackend: number = SR_BACKEND_XENGINE;
  @State protocol: number = PROTOCOL_RDP;
  @State mlDefaultAppId: string = '';
  @State mlDefaultAppName: string = '';
  @State mlResMode: number = ML_RES_FOLLOW_SCREEN;
  @State mlWidthText: string = '1920';
  @State mlHeightText: string = '1080';
  @State mlFps: number = 60;
  @State mlFpsText: string = '60';
  @State mlFpsCustom: boolean = false;
  @State mlBitrateAuto: boolean = true;
  @State mlBitrateText: string = kbpsToMbps(ML_BITRATE_DEFAULT_KBPS).toString();
  @State mlColorSpace: number = ML_COLORSPACE_REC709;
  @State mlCodec: number = ML_CODEC_AUTO;
  @State useGlobalStream: boolean = true;
  @State mlScaleMode: number = ML_SCALE_AUTO;
  @State mlScaleText: string = '100';
  private mlH264Support: number = CODEC_SUPPORT_NONE;
  private mlHevcSupport: number = CODEC_SUPPORT_NONE;
  private mlAv1Support: number = CODEC_SUPPORT_NONE;
  @State errorMessage: string = '';
  @State hostError: string = '';
  @State editing: boolean = false;
  @State detectedText: string = '';
  @State connecting: boolean = false;
  @State pairing: boolean = false;
  private connectionId: string = '';
  private activeConnectionId: string = '';
  private tipsOptions: TipsOptions = {};
  @State searching: boolean = false;
  private discovery: MdnsDiscovery = new MdnsDiscovery();
  private searchTimer: number = -1;
  private discovered: MdnsHost[] = [];
  private originalMoonlightHost: string = '';
  private originalMoonlightHostId: string = '';
  private originalMoonlightMac: string = '';
  private storedPassword: string = '';
  private returned: boolean = false;
  private detectedWidth: number = 1920;
  private detectedHeight: number = 1080;

  private sessionListener: SessionStateListener = (connectionId: string,
    state: ConnectionState, message: string, origin: ConnectOrigin): void => {
    if (origin !== ConnectOrigin.Editor || connectionId !== this.activeConnectionId) {
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
    this.mlH264Support = DeviceCapabilities.codecSupport(ML_CODEC_H264);
    this.mlHevcSupport = DeviceCapabilities.codecSupport(ML_CODEC_HEVC);
    this.mlAv1Support = DeviceCapabilities.codecSupport(ML_CODEC_AV1);
    const audioCapability: Capability = DeviceCapabilities.audio();
    this.audioSupported = audioCapability.supported;
    this.audioDisabledReason = audioCapability.reason;
    const detected: DisplayProfile = SettingsStore.detectedDisplay();
    this.detectedText = detected.valid()
      ? `${detected.width} × ${detected.height}` : '无法获取显示器分辨率';
    this.detectedWidth = detected.valid() ? detected.width : 1920;
    this.detectedHeight = detected.valid() ? detected.height : 1080;
    this.desktopWidthText = detected.valid() ? detected.width.toString() : '1920';
    this.desktopHeightText = detected.valid() ? detected.height.toString() : '1080';
    this.scaleText = this.suggestedScale().toString();
    const mlDefaults: AppSettings = SettingsStore.load();
    this.mlFps = mlDefaults.mlFps;
    this.mlFpsText = mlDefaults.mlFps.toString();
    this.mlFpsCustom = ML_FPS_PRESETS.indexOf(mlDefaults.mlFps) < 0;
    this.mlBitrateAuto = mlDefaults.mlBitrateAuto;
    this.mlBitrateText = kbpsToMbps(mlDefaults.mlBitrateKbps).toString();
    this.mlColorSpace = mlDefaults.mlColorSpace;
    this.mlCodec = mlDefaults.mlCodec;
    this.mlScaleMode = mlDefaults.mlScaleMode;
    this.mlScaleText = mlDefaults.mlScalePercent.toString();
    this.clampMlCodec();

    const params: EditConnectionParams =
      this.getUIContext().getRouter().getParams() as EditConnectionParams;
    const id: string = params?.id ?? '';
    this.connectionId = id;
    this.editing = id.length > 0;
    if (!this.editing) {
      this.protocol = params?.protocol ?? PROTOCOL_RDP;
      if (this.protocol === PROTOCOL_MOONLIGHT) {
        this.port = ML_DEFAULT_PORT.toString();
      }
      return;
    }
    const conn: SavedConnection | undefined = ConnectionStore.get(id);
    if (conn !== undefined) {
      this.protocol = conn.protocol;
      this.name = conn.name;
      this.host = conn.host;
      this.originalMoonlightHost = conn.host;
      this.port = conn.port.toString();
      if (conn instanceof RdpConnection) {
        this.username = conn.username;
        this.domain = conn.domain;
        this.useGlobalDisplay = conn.useGlobalDisplay;
        if (!conn.useGlobalDisplay) {
          this.displayResolutionAuto = conn.resolutionAuto;
          this.displayScaleAuto = conn.scaleAuto;
        }
        if (conn.width > 0 && conn.height > 0) {
          this.desktopWidthText = conn.width.toString();
          this.desktopHeightText = conn.height.toString();
        }
        if (conn.scalePercent > 0) {
          this.scaleText = conn.scalePercent.toString();
        }
        this.enableAudio = this.audioSupported && conn.enableAudio;
        this.ignoreCertificate = conn.ignoreCertificate;
        this.useGlobalAdvanced = conn.useGlobalAdvanced;
        this.useGlobalSr = conn.useGlobalSr;
        this.srEnabled = conn.srEnabled;
        this.srRatioPercent = SettingsStore.snapSrRatioPreset(conn.srRatioPercent);
        this.srBackend = conn.srBackend;
      } else if (conn instanceof MoonlightConnection) {
        this.originalMoonlightHostId = conn.mlHostId;
        this.originalMoonlightMac = conn.mlMac;
        this.mlDefaultAppId = conn.mlDefaultAppId;
        this.mlDefaultAppName = conn.mlDefaultAppName;
        this.mlResMode = conn.mlResMode;
        if (conn.mlWidth > 0 && conn.mlHeight > 0) {
          this.mlWidthText = conn.mlWidth.toString();
          this.mlHeightText = conn.mlHeight.toString();
        }
        this.mlFps = conn.mlFps;
        this.mlFpsText = conn.mlFps.toString();
        this.mlFpsCustom = ML_FPS_PRESETS.indexOf(conn.mlFps) < 0;
        this.mlBitrateAuto = conn.mlBitrateAuto;
        this.mlBitrateText = kbpsToMbps(conn.mlBitrateKbps).toString();
        this.mlColorSpace = conn.mlColorSpace;
        this.mlCodec = conn.mlCodec;
        this.useGlobalStream = conn.useGlobalStream;
        this.mlScaleMode = conn.mlScaleMode;
        this.mlScaleText = conn.mlScalePercent.toString();
        this.useGlobalSr = conn.useGlobalSr;
        this.srEnabled = conn.srEnabled;
        this.srRatioPercent = SettingsStore.snapSrRatioPreset(conn.srRatioPercent);
        this.srBackend = conn.srBackend;
      }
      this.clampMlCodec();
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
    if (this.searchTimer >= 0) {
      clearTimeout(this.searchTimer);
      this.searchTimer = -1;
    }
    this.discovery.stop();
    releaseFeedback(this.getUIContext());
  }

  onBackPress(): boolean {
    this.cancelConnect();
    this.autoFill = false;
    return false;
  }

  private parsePort(): number {
    const value: number = Number.parseInt(this.port, 10);
    return Number.isNaN(value) || value <= 0 ? 3389 : value;
  }

  private parseSize(text: string, fallback: number): number {
    return parseSizeValue(text, fallback);
  }

  private parseScale(text: string, fallback: number): number {
    return parseScaleValue(text, fallback);
  }

  private onOff(value: boolean): string {
    return value ? '开' : '关';
  }

  private isMoonlight(): boolean {
    return this.protocol === PROTOCOL_MOONLIGHT;
  }

  private parseMoonlightPort(): number {
    const value: number = Number.parseInt(this.port, 10);
    return Number.isNaN(value) || value <= 0 ? ML_DEFAULT_PORT : value;
  }

  private effectiveBitrate(): number {
    const mbps: number = Number.parseFloat(this.mlBitrateText);
    const kbps: number = Number.isNaN(mbps) ? ML_BITRATE_DEFAULT_KBPS : mbpsToKbps(mbps);
    return Math.min(Math.max(kbps, ML_BITRATE_MIN_KBPS), ML_BITRATE_MAX_KBPS);
  }

  private mlPreviewWidth(): number {
    return this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlWidthText, this.detectedWidth) : this.detectedWidth;
  }

  private mlPreviewHeight(): number {
    return this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlHeightText, this.detectedHeight) : this.detectedHeight;
  }

  private mlAutoScalePercent(): number {
    const sr: SrProfile = this.resolvedSr();
    return SettingsStore.autoScalePercent(this.mlPreviewWidth(), this.mlPreviewHeight(),
      sr.enabled, sr.ratioPercent);
  }

  private mlAutoBitrateKbps(): number {
    return recommendedBitrateKbps(this.mlPreviewWidth(), this.mlPreviewHeight(), this.mlFps,
      this.mlCodec);
  }

  private srSummary(profile: SrProfile): string {
    return profile.enabled ? `${this.onOff(true)} ${srRatioLabel(profile.ratioPercent)}` : '关';
  }

  private advancedSummary(profile: AdvancedProfile): string {
    return `音频${this.onOff(profile.enableAudio)}`
      + ` · 忽略证书${this.onOff(profile.ignoreCertificate)}`;
  }

  private resModeLabel(mode: number): string {
    return mode === ML_RES_FIXED ? '手动' : '自动';
  }

  private streamSummary(profile: MoonlightProfile): string {
    const res: string = profile.resMode === ML_RES_FIXED
      ? `${profile.width}×${profile.height}` : this.resModeLabel(profile.resMode);
    const bitrate: string = profile.bitrateAuto
      ? '自动' : `${kbpsToMbps(profile.bitrateKbps)}Mbps`;
    return `${res} · ${profile.fps}fps · ${bitrate}`;
  }

  private srUsable(): boolean {
    return SettingsStore.superResolutionUsable(this.protocol);
  }

  private resolvedSr(): SrProfile {
    const setting: SrSetting = {
      useGlobalSr: this.useGlobalSr,
      srEnabled: this.srEnabled,
      srRatioPercent: this.srRatioPercent,
      srBackend: this.srBackend
    };
    return SettingsStore.resolveSr(this.protocol, setting);
  }

  private srChoice(): number {
    if (!this.srEnabled) {
      return 0;
    }
    return this.srBackend === SR_BACKEND_FSR ? 2 : 1;
  }

  private srChoiceUsable(choice: number): boolean {
    if (choice === 0) {
      return true;
    }
    const backend: number = choice === 2 ? SR_BACKEND_FSR : SR_BACKEND_XENGINE;
    return SettingsStore.srBackendUsable(backend, this.protocol);
  }

  private suggestedScale(): number {
    const sr: SrProfile = this.resolvedSr();
    const fallback: DisplayProfile = SettingsStore.globalDisplayProfile();
    let width: number = fallback.width;
    let height: number = fallback.height;
    if (!this.useGlobalDisplay) {
      if (this.displayResolutionAuto) {
        const screen: DisplayProfile = SettingsStore.detectedDisplay();
        if (screen.valid()) {
          width = screen.width;
          height = screen.height;
        }
      } else {
        width = this.parseSize(this.desktopWidthText, fallback.width);
        height = this.parseSize(this.desktopHeightText, fallback.height);
      }
    }
    return SettingsStore.autoScalePercent(width, height, sr.enabled, sr.ratioPercent);
  }

  private focusHostField(): void {
    const target: string = this.isMoonlight() ? 'editHostMoonlight' : 'editHostRdp';
    try {
      this.getUIContext().getFocusController().requestFocus(target);
    } catch (error) {
    }
  }

  private buildOptions(applyGlobalAdvanced: boolean): RdpConnectOptions | undefined {
    const target: string = this.host.trim();
    if (target.length === 0) {
      this.errorMessage = '';
      this.hostError = '请输入主机名或 IP 地址';
      this.focusHostField();
      return undefined;
    }
    this.hostError = '';
    this.errorMessage = '';
    const global: DisplayProfile = SettingsStore.globalDisplayProfile();
    const sr: SrProfile = this.resolvedSr();
    const options: RdpConnectOptions =
      RdpConnectOptions.of(target, this.parsePort(), this.username.trim(), this.password,
        this.domain.trim());
    options.name = this.name.trim();
    if (this.useGlobalDisplay) {
      options.width = global.width;
      options.height = global.height;
      options.scalePercent = global.scalePercent;
    } else {
      const screen: DisplayProfile = SettingsStore.detectedDisplay();
      if (this.displayResolutionAuto && screen.valid()) {
        options.width = screen.width;
        options.height = screen.height;
      } else {
        options.width = this.parseSize(this.desktopWidthText, global.width);
        options.height = this.parseSize(this.desktopHeightText, global.height);
      }
      options.scalePercent = this.displayScaleAuto
        ? SettingsStore.autoScalePercent(options.width, options.height,
          sr.enabled && sr.ratioPercent > 100, sr.ratioPercent)
        : this.parseScale(this.scaleText, this.suggestedScale());
    }
    options.enableAudio = this.enableAudio;
    options.ignoreCertificate = this.ignoreCertificate;
    options.srEnabled = sr.enabled;
    options.srRatioPercent = sr.ratioPercent;
    options.srBackend = sr.backend;
    options.streamWidth = SettingsStore.srSessionSize(options.width,
      sr.enabled ? sr.ratioPercent : 0);
    options.streamHeight = SettingsStore.srSessionSize(options.height,
      sr.enabled ? sr.ratioPercent : 0);
    if (applyGlobalAdvanced && this.useGlobalAdvanced) {
      const advanced: AdvancedProfile = SettingsStore.globalAdvancedProfile();
      options.enableAudio = advanced.enableAudio;
      options.ignoreCertificate = advanced.ignoreCertificate;
    }
    if (!this.audioSupported) {
      options.enableAudio = false;
    }
    return options;
  }

  private submit(connect: boolean): void {
    if (this.isMoonlight()) {
      this.submitMoonlight(connect);
      return;
    }
    const options: RdpConnectOptions | undefined = this.buildOptions(false);
    if (options === undefined) {
      return;
    }
    const id: string = this.connectionId.length > 0
      ? this.connectionId : util.generateRandomUUID(true);
    const conn: RdpConnection = RdpConnection.fromOptions(id, options);
    conn.useGlobalDisplay = this.useGlobalDisplay;
    conn.resolutionAuto = !this.useGlobalDisplay && this.displayResolutionAuto;
    conn.scaleAuto = !this.useGlobalDisplay && this.displayScaleAuto;
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
    const attempt: RdpConnectOptions | undefined = this.buildOptions(true);
    if (attempt === undefined) {
      return;
    }
    attempt.password = this.password;
    const request: PendingConnection = new PendingConnection();
    request.options = attempt;
    request.connectionId = id;
    request.origin = ConnectOrigin.Editor;
    request.protocol = PROTOCOL_RDP;
    request.savedConnection = conn;
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
  }

  private submitMoonlight(connect: boolean): void {
    const target: string = this.host.trim();
    if (target.length === 0) {
      this.errorMessage = '';
      this.hostError = '请输入主机名或 IP 地址';
      this.focusHostField();
      return;
    }
    const conflict: string | undefined = this.conflictingMoonlightHost(target);
    if (conflict !== undefined) {
      this.errorMessage = '';
      this.hostError = `主机 ${conflict} 已添加，请先删除该连接`;
      this.focusHostField();
      return;
    }
    this.hostError = '';
    this.errorMessage = '';
    const hostChanged: boolean = this.editing
      && this.normalizeHost(target) !== this.normalizeHost(this.originalMoonlightHost);
    const id: string = this.connectionId.length > 0
      ? this.connectionId : util.generateRandomUUID(true);
    const conn: MoonlightConnection = new MoonlightConnection();
    conn.id = id;
    conn.protocol = PROTOCOL_MOONLIGHT;
    conn.host = target;
    conn.port = this.parseMoonlightPort();
    conn.name = this.name.trim();
    conn.mlHostId = hostChanged ? '' : this.originalMoonlightHostId;
    conn.mlMac = hostChanged ? '' : this.originalMoonlightMac;
    conn.mlDefaultAppId = hostChanged ? '' : this.mlDefaultAppId;
    conn.mlDefaultAppName = hostChanged ? '' : this.mlDefaultAppName;
    conn.mlResMode = this.mlResMode;
    conn.mlWidth = this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlWidthText, 1920) : 0;
    conn.mlHeight = this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlHeightText, 1080) : 0;
    conn.mlFps = this.mlFps;
    conn.mlBitrateAuto = this.mlBitrateAuto;
    conn.mlBitrateKbps = this.effectiveBitrate();
    conn.mlColorSpace = this.mlColorSpace;
    conn.mlCodec = this.mlCodec;
    conn.useGlobalStream = this.useGlobalStream;
    conn.mlScaleMode = this.mlScaleMode;
    conn.mlScalePercent = this.parseScale(this.mlScaleText, 100);
    conn.useGlobalSr = this.useGlobalSr;
    conn.srEnabled = this.srEnabled && this.srUsable();
    conn.srRatioPercent = SettingsStore.snapSrRatioPreset(this.srRatioPercent);
    conn.srBackend = this.srBackend;
    conn.updatedAt = Date.now();
    if (!connect) {
      if (!this.persist(conn)) {
        return;
      }
      this.forgetReplacedHost(target);
      this.returnToList();
      return;
    }
    this.pairMoonlight(conn);
  }

  private pairMoonlight(conn: MoonlightConnection): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      this.errorMessage = '无法发起配对';
      return;
    }
    this.errorMessage = '';
    this.pairing = true;
    MoonlightConnector.pairOnly((context as common.UIAbilityContext).filesDir, conn.host, conn.port,
      (pin: string, pairing: Promise<PairResult>, cancel: () => void): Promise<boolean> =>
        promptPairingPin(this.getUIContext(), pin, pairing, cancel),
      this.moonlightHostPolicy())
      .then((result: MoonlightPairResult) => {
        this.pairing = false;
        if (!result.ok) {
          this.errorMessage = MoonlightConnector.describeError(result.error);
          return;
        }
        conn.mlHostId = result.hostId;
        if (this.persist(conn)) {
          this.forgetReplacedHost(conn.host.trim());
          showToast(this.getUIContext(), '配对成功');
          this.returnToList();
        }
      })
      .catch((error: BusinessError) => {
        this.pairing = false;
        this.errorMessage = '配对失败，请重试';
        hilog.error(DOMAIN, TAG, 'pair failed: %{public}s', JSON.stringify(error));
      });
  }

  private openHostSearch(): void {
    if (this.searching) {
      return;
    }
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    this.searching = true;
    this.discovered = [];
    this.discovery.start(context as common.UIAbilityContext, MOONLIGHT_SERVICE_TYPE,
      (hosts: MdnsHost[]): void => {
        this.discovered = hosts;
      });
    this.searchTimer = setTimeout((): void => {
      this.finishHostSearch();
    }, HOST_SEARCH_MS);
  }

  private finishHostSearch(): void {
    if (this.searchTimer >= 0) {
      clearTimeout(this.searchTimer);
      this.searchTimer = -1;
    }
    this.discovery.stop();
    this.searching = false;
    const hosts: MdnsHost[] = this.discovered;
    this.discovered = [];
    if (hosts.length === 0) {
      showToast(this.getUIContext(), '未发现主机，请确认主机已通电并与本机在同一局域网');
      return;
    }
    showHostPicker(this.getUIContext(), hosts, '选择主机', this.addedMoonlightAddresses())
      .then((host: MdnsHost | undefined) => {
        if (host === undefined) {
          return;
        }
        this.host = host.address;
        this.hostError = '';
        this.errorMessage = '';
        if (host.port > 0) {
          this.port = host.port.toString();
        }
        if (this.name.length === 0 && host.name.length > 0) {
          this.name = host.name;
        }
      })
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'host picker failed: %{public}s', JSON.stringify(error));
      });
  }

  private normalizeHost(host: string): string {
    let value: string = host.trim().toLowerCase();
    while (value.endsWith('.')) {
      value = value.substring(0, value.length - 1);
    }
    return value;
  }

  private moonlightHostPolicy(): MoonlightHostPolicy {
    const policy: MoonlightHostPolicy = new MoonlightHostPolicy();
    policy.expectedId = this.originalMoonlightHostId;
    const all: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < all.length; i++) {
      const conn: SavedConnection = all[i];
      if (conn instanceof MoonlightConnection && conn.id !== this.connectionId
        && conn.mlHostId.length > 0) {
        const known: MoonlightKnownHost = new MoonlightKnownHost();
        known.hostId = conn.mlHostId;
        known.host = conn.host;
        policy.known.push(known);
      }
    }
    return policy;
  }

  private conflictingMoonlightHost(host: string): string | undefined {
    const target: string = this.normalizeHost(host);
    const all: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < all.length; i++) {
      const conn: SavedConnection = all[i];
      if (!(conn instanceof MoonlightConnection) || conn.id === this.connectionId) {
        continue;
      }
      if (this.normalizeHost(conn.host) === target) {
        return conn.host;
      }
    }
    return undefined;
  }

  private addedMoonlightAddresses(): string[] {
    const result: string[] = [];
    const all: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < all.length; i++) {
      const conn: SavedConnection = all[i];
      if (conn instanceof MoonlightConnection) {
        result.push(this.normalizeHost(conn.host));
      }
    }
    return result;
  }

  private forgetPairing(host: string): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined || host.trim().length === 0) {
      return;
    }
    MoonlightConnector.forgetHost((context as common.UIAbilityContext).filesDir,
      host, this.parseMoonlightPort());
  }

  private forgetReplacedHost(newHost: string): void {
    const old: string = this.originalMoonlightHost.trim();
    if (old.length === 0 || this.normalizeHost(old) === this.normalizeHost(newHost)) {
      return;
    }
    this.forgetPairing(old);
    this.originalMoonlightHost = newHost.trim();
    this.originalMoonlightHostId = '';
    this.originalMoonlightMac = '';
  }

  private persist(conn: SavedConnection): boolean {
    try {
      ConnectionStore.save(conn);
      SettingsStore.rememberNewProtocol(conn.protocol);
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

  private busy(): boolean {
    return this.connecting || this.pairing;
  }

  private primaryLabel(): string {
    if (this.isMoonlight()) {
      return '配对';
    }
    return this.connecting ? '取消' : '连接';
  }

  private primaryBusy(): boolean {
    return this.connecting || this.pairing;
  }

  private handleSave(): void {
    if (this.password.length > 0 && this.password !== this.storedPassword) {
      showConfirm(this.getUIContext(), {
        title: '密码不会被保存',
        message: '连接验证成功后才会保存密码。是否继续保存连接？',
        confirmLabel: '继续保存',
        onConfirm: () => {
          this.submit(false);
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
    this.autoFill = false;
    this.getUIContext().getRouter().back();
  }

  private confirmDeleteCurrent(): void {
    if (this.connectionId.length === 0) {
      return;
    }
    const conn: SavedConnection | undefined = ConnectionStore.get(this.connectionId);
    const label: string = conn !== undefined ? conn.label() : this.name;
    showConfirm(this.getUIContext(), {
      title: '删除连接',
      message: `将删除「${label}」，该操作无法撤销。是否继续？`,
      confirmLabel: '删除',
      danger: true,
      onConfirm: () => {
        this.deleteCurrent();
      }
    });
  }

  private deleteCurrent(): void {
    if (this.connectionId.length > 0) {
      ConnectionStore.remove(this.connectionId);
      CredentialStore.removePassword(this.connectionId).catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'remove password failed: %{public}s', JSON.stringify(error));
      });
      if (this.isMoonlight()) {
        this.forgetPairing(this.host);
      }
    }
    this.returnToList();
  }

  @Builder
  displayResModeChip(label: string, auto: boolean) {
    ChoiceChip({
      label: label,
      selected: this.displayResolutionAuto === auto,
      onTap: () => {
        this.displayResolutionAuto = auto;
      }
    })
  }

  @Builder
  srRatioChip(preset: number) {
    ChoiceChip({
      label: srRatioLabel(preset),
      selected: this.srRatioPercent === preset,
      onTap: () => {
        this.srRatioPercent = preset;
      }
    })
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
      }
    })
  }

  @Builder
  srAdvancedSection() {
    Column() {
      Row() {
        Column() {
          Text('使用会话独立超分辨率设置')
            .fontSize(14)
          Text('超分：远端按输出÷倍率出画；关闭则跟随全局')
            .fontSize(12).fontColor($r('app.color.text_tertiary'))
            
            .margin({ top: 2 })
          if (!this.srUsable()) {
            Text('本设备不支持超分辨率')
              .fontSize(12).fontColor($r('app.color.warning'))
              .lineHeight(16)
              .margin({ top: 2 })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalSr })
          .selectedColor($r('app.color.brand_fill'))
          .enabled(this.srUsable())
          
          .onChange((isOn: boolean) => {
            if (!this.srUsable()) {
              return;
            }
            this.useGlobalSr = !isOn;
          })
      }
      .width('100%')
      .height(58)

      if (this.useGlobalSr) {
        Text('全局：' + this.srSummary(SettingsStore.globalSrProfile(this.protocol)))
          .fontSize(12).fontColor($r('app.color.text_tertiary'))
          
          .width('100%')
      } else {
        Text('超分辨率').fontSize(14)
        Text('会话按输出÷倍率取流，本机上采样')
          .fontSize(12).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })

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
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  mlResModeChip(mode: number, label: string, leftMargin: number) {
    Text(label)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor(this.mlResMode === mode
        ? $r('app.color.brand_text') : $r('app.color.text_secondary'))
      .height(32)
      .textAlign(TextAlign.Center)
      .backgroundColor(this.mlResMode === mode
        ? $r('app.color.brand_tint') : $r('app.color.control_bg'))
      .border({
        width: 1,
        color: this.mlResMode === mode
          ? $r('app.color.brand_tint_strong') : $r('app.color.control_border')
      })
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .responseRegion({ x: 0, y: -4, width: '100%', height: 40 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.mlResMode = mode;
      })
      .focusable(true)
      .focusOnTouch(true)
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
      }
    })
  }

  @Builder
  mlBitrateModeChip(label: string, auto: boolean, leftMargin: number) {
    Text(label)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor(this.mlBitrateAuto === auto
        ? $r('app.color.brand_text') : $r('app.color.text_secondary'))
      .height(32)
      .textAlign(TextAlign.Center)
      .backgroundColor(this.mlBitrateAuto === auto
        ? $r('app.color.brand_tint') : $r('app.color.control_bg'))
      .border({
        width: 1,
        color: this.mlBitrateAuto === auto
          ? $r('app.color.brand_tint_strong') : $r('app.color.control_border')
      })
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .responseRegion({ x: 0, y: -4, width: '100%', height: 40 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.mlBitrateAuto = auto;
      })
      .focusable(true)
      .focusOnTouch(true)
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
      }
    })
  }

  @Builder
  moonlightForm() {
    Column() {
      Row() {
        Column() {
          FormField({ label: '名称（可选）' }) {
            TextInput({ placeholder: '自定义名称，留空则显示主机地址', text: this.name })
              .width('100%')
              .height(46)
              .fontColor($r('app.color.text_primary'))
              .placeholderColor($r('app.color.text_tertiary'))
              .backgroundColor($r('app.color.surface_subtle'))
              .border({ width: 1, color: $r('app.color.border') })
              .borderRadius(10)
              .onChange((value: string) => {
                this.name = value;
              })
          }
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Row() {
          if (this.searching) {
            LoadingProgress()
              .width(20)
              .height(20)
              .color($r('app.color.brand_fill'))
          } else {
            Image($r('app.media.ic_search'))
              .width(20)
              .height(20)
              .fillColor($r('app.color.icon_secondary'))
              .draggable(false)
          }
        }
        .width(46)
        .height(46)
        .backgroundColor($r('app.color.surface_subtle'))
        .border({ width: 1, color: $r('app.color.border') })
        .borderRadius(10)
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .margin({ left: 12 })
        .hoverEffect(HoverEffect.Highlight)
        .opacity(this.searching ? 0.7 : 1)
        .bindTips(this.searching ? '正在搜索…' : '自动搜索局域网主机', this.tipsOptions)
        .onClick(() => {
          this.openHostSearch();
        })
        .focusable(true)
        .focusOnTouch(true)
      }
      .width('100%')
      .alignItems(VerticalAlign.Bottom)
      .margin({ bottom: 16 })

      Row() {
        Column() {
          FormField({ label: '主机名或 IP 地址', error: this.hostError }) {
            TextInput({ placeholder: '例如 192.168.1.100', text: this.host })
              .id('editHostMoonlight')
              .width('100%')
              .height(46)
              .fontColor($r('app.color.text_primary'))
              .placeholderColor($r('app.color.text_tertiary'))
              .backgroundColor($r('app.color.surface_subtle'))
              .border({ width: 1, color: $r('app.color.border') })
              .borderRadius(10)
              .onChange((value: string) => {
                this.host = value;
                this.hostError = '';
                this.errorMessage = '';
              })
          }
        }
        .layoutWeight(3)
        .alignItems(HorizontalAlign.Start)

        Column() {
          FormField({ label: '端口' }) {
            TextInput({ placeholder: '47989', text: this.port })
              .width('100%')
              .height(46)
              .fontColor($r('app.color.text_primary'))
              .placeholderColor($r('app.color.text_tertiary'))
              .backgroundColor($r('app.color.surface_subtle'))
              .border({ width: 1, color: $r('app.color.border') })
              .borderRadius(10)
              .type(InputType.Number)
              .onChange((value: string) => {
                this.port = value;
              })
          }
        }
        .layoutWeight(2)
        .margin({ left: 16 })
        .alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .margin({ bottom: 16 })

    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  moonlightAdvanced() {
    Column() {
      Divider()
        .strokeWidth(1)
        .color($r('app.color.divider'))
        .margin({ top: 20, bottom: 8 })

      Row() {
        Column() {
          Text('高级设置').fontSize(14)
          Text('默认跟随全局；展开可覆盖')
            .fontSize(12).fontColor($r('app.color.text_tertiary'))
            
            .margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Row() {
          if (!this.useGlobalStream) {
            Text('已自定义')
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              
              .margin({ right: 8 })
          }
          Text(this.advancedExpanded ? '收起' : '展开')
            .fontSize(14)
            
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
      .focusable(true)
      .focusOnTouch(true)

      if (this.advancedExpanded) {
        Column() {
          Row() {
            Column() {
              Text('使用会话独立串流设置').fontSize(14)
              Text('本连接独立配置；关闭则跟随全局')
                .fontSize(12).fontColor($r('app.color.text_tertiary'))
                
                .margin({ top: 2 })
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)

            Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalStream })
              .selectedColor($r('app.color.brand_fill'))
              
              .onChange((isOn: boolean) => {
                this.useGlobalStream = !isOn;
              })
          }
          .width('100%')
          .height(58)

          if (this.useGlobalStream) {
            Text('全局：' + this.streamSummary(SettingsStore.globalStreamProfile()))
              .fontSize(12).fontColor($r('app.color.text_tertiary'))
              
              .width('100%')
          } else {
            Row() {
              Column() {
                Text('分辨率').fontSize(14)
                Text('跟随当前屏幕或固定分辨率')
                  .fontSize(12).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Row() {
                this.mlResModeChip(ML_RES_FOLLOW_SCREEN, '自动', 0)
                this.mlResModeChip(ML_RES_FIXED, '手动', 8)
              }
            }
            .width('100%').margin({ top: 12 })

            if (this.mlResMode === ML_RES_FOLLOW_SCREEN) {
              Text('自动检测：' + this.detectedText)
                .fontSize(12).fontColor($r('app.color.text_tertiary'))
                .width('100%').margin({ top: 6 })
            } else {
              Row() {
                Column() {
                  FormField({ label: '分辨率宽度' }) {
                    TextInput({ placeholder: '1920', text: this.mlWidthText })
                      .width('100%').height(46).fontColor($r('app.color.text_primary')).placeholderColor($r('app.color.text_tertiary')).backgroundColor($r('app.color.surface_subtle')).border({ width: 1, color: $r('app.color.border') }).borderRadius(10)
                      .type(InputType.Number)
                      .onChange((value: string) => {
                        this.mlWidthText = value;
                      })
                  }
                }
                .layoutWeight(1).alignItems(HorizontalAlign.Start)

                Column() {
                  FormField({ label: '分辨率高度' }) {
                    TextInput({ placeholder: '1080', text: this.mlHeightText })
                      .width('100%').height(46).fontColor($r('app.color.text_primary')).placeholderColor($r('app.color.text_tertiary')).backgroundColor($r('app.color.surface_subtle')).border({ width: 1, color: $r('app.color.border') }).borderRadius(10)
                      .type(InputType.Number)
                      .onChange((value: string) => {
                        this.mlHeightText = value;
                      })
                  }
                }
                .layoutWeight(1).margin({ left: 16 }).alignItems(HorizontalAlign.Start)
              }
              .width('100%').margin({ top: 12 })

              Text('自动检测：' + this.detectedText)
                .fontSize(12).fontColor($r('app.color.text_tertiary'))
                .width('100%').margin({ top: 6 })
            }

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
                },
                onPercentTextChange: (text: string) => {
                  this.mlScaleText = text;
                }
              })
            }
            .width('100%').margin({ top: 12 })

            Text('帧率').fontSize(14).margin({ top: 12 })
            Text('默认 60，可选档位或自定义')
              .fontSize(12).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
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
                  .width(140).height(46).fontColor($r('app.color.text_primary')).placeholderColor($r('app.color.text_tertiary')).backgroundColor($r('app.color.surface_subtle')).border({ width: 1, color: $r('app.color.border') }).borderRadius(10)
                  
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.mlFpsText = value;
                    this.syncFpsText();
                  })
                Text('fps')
                  .fontSize(14).margin({ left: 10 })
              }
              .width('100%').margin({ top: 4 })
            }

            Row() {
              Column() {
                Text('码率').fontSize(14)
                Text('自动按分辨率与帧率计算')
                  .fontSize(12).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Row() {
                this.mlBitrateModeChip('自动', true, 0)
                this.mlBitrateModeChip('手动', false, 8)
              }
            }
            .width('100%').margin({ top: 12 })

            if (this.mlBitrateAuto) {
              Text('自动：约 ' + kbpsToMbps(this.mlAutoBitrateKbps()) + ' Mbps')
                .fontSize(12).fontColor($r('app.color.text_tertiary'))
                .width('100%').margin({ top: 6 })
            } else {
              Row() {
                TextInput({ placeholder: '20', text: this.mlBitrateText })
                  .width(140).height(46).fontColor($r('app.color.text_primary')).placeholderColor($r('app.color.text_tertiary')).backgroundColor($r('app.color.surface_subtle')).border({ width: 1, color: $r('app.color.border') }).borderRadius(10)
                  
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.mlBitrateText = value;
                  })
                Text('Mbps')
                  .fontSize(14).margin({ left: 10 })
              }
              .width('100%').margin({ top: 6 })
            }

            Text('编码器').fontSize(14).margin({ top: 8 })
            if (!this.mlCodecUsable(ML_CODEC_AUTO)) {
              Text('无可用解码器，无法串流')
                .fontSize(12).fontColor($r('app.color.warning')).margin({ top: 2 })
            } else if (this.mlCodecSoftware(this.mlCodec)) {
              Text('无硬件解码，将使用软件解码')
                .fontSize(12).fontColor($r('app.color.warning')).margin({ top: 2 })
            }
            Flex({ wrap: FlexWrap.Wrap }) {
              this.mlCodecChip(ML_CODEC_AUTO, '自动')
              this.mlCodecChip(ML_CODEC_H264, 'H.264')
              this.mlCodecChip(ML_CODEC_HEVC, 'HEVC')
              this.mlCodecChip(ML_CODEC_AV1, 'AV1')
            }
            .width('100%').margin({ top: 8 })

            Text('色彩空间').fontSize(14).margin({ top: 8 })
            Flex({ wrap: FlexWrap.Wrap }) {
              this.mlColorSpaceChip(ML_COLORSPACE_REC709, 'Rec709')
              this.mlColorSpaceChip(ML_COLORSPACE_REC601, 'Rec601')
            }
            .width('100%').margin({ top: 8 })

            Divider()
              .strokeWidth(1)
              .color($r('app.color.divider'))
              .margin({ top: 8, bottom: 4 })

            this.srAdvancedSection()
          }
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  optionRow(title: string, subtitle: string, value: boolean,
    onToggle: (v: boolean) => void, enabled: boolean, disabledReason: string) {
    SettingToggleRow({
      title: title,
      subtitle: subtitle,
      value: value,
      switchEnabled: enabled,
      disabledReason: disabledReason,
      onToggle: (v: boolean) => {
        onToggle(v);
      }
    })
  }

  build() {
    Column() {
      PageHeader({
        title: this.editing ? '编辑连接' : '新增连接',
        leftLabel: '取消',
        onLeft: () => {
          this.cancelConnect();
          this.returnToList();
        }
      }) {
        ProtocolTabs({
          protocol: this.protocol,
          locked: this.editing,
          onChange: (protocol: number) => {
            const untouched: boolean = this.port.length === 0
              || this.port === ML_DEFAULT_PORT.toString() || this.port === RDP_DEFAULT_PORT.toString();
            this.protocol = protocol;
            this.hostError = '';
            this.errorMessage = '';
            if (untouched) {
              this.port = (protocol === PROTOCOL_MOONLIGHT
                ? ML_DEFAULT_PORT : RDP_DEFAULT_PORT).toString();
            }
          }
        })
      }

      Scroll() {
        Column() {
          ContentCard({ pad: 28 }) {
            if (this.isMoonlight()) {
              this.moonlightForm()
            } else {
            FormField({
              label: '名称（可选）',
              marginBottom: 16
            }) {
              TextInput({ placeholder: '自定义名称，留空则显示主机地址', text: this.name })
                .width('100%')
                .height(46)
                .fontColor($r('app.color.text_primary'))
                .placeholderColor($r('app.color.text_tertiary'))
                .backgroundColor($r('app.color.surface_subtle'))
                .border({ width: 1, color: $r('app.color.border') })
                .borderRadius(10)
                .onChange((value: string) => {
                  this.name = value;
                })
            }

            FormField({
              label: '主机名或 IP 地址',
              error: this.hostError,
              marginBottom: 16
            }) {
              TextInput({ placeholder: '例如 192.168.1.100', text: this.host })
                .id('editHostRdp')
                .width('100%')
                .height(46)
                .fontColor($r('app.color.text_primary'))
                .placeholderColor($r('app.color.text_tertiary'))
                .backgroundColor($r('app.color.surface_subtle'))
                .border({ width: 1, color: $r('app.color.border') })
                .borderRadius(10)
                .onChange((value: string) => {
                  this.host = value;
                  this.hostError = '';
                  this.errorMessage = '';
                })
            }

            Row() {
              Column() {
                FormField({ label: '端口' }) {
                  TextInput({ placeholder: '3389', text: this.port })
                    .width('100%')
                    .height(46)
                    .fontColor($r('app.color.text_primary'))
                    .placeholderColor($r('app.color.text_tertiary'))
                    .backgroundColor($r('app.color.surface_subtle'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .borderRadius(10)
                    .type(InputType.Number)
                    .onChange((value: string) => {
                      this.port = value;
                    })
                }
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column() {
                FormField({ label: '域（可选）' }) {
                  TextInput({ placeholder: 'WORKGROUP', text: this.domain })
                    .width('100%')
                    .height(46)
                    .fontColor($r('app.color.text_primary'))
                    .placeholderColor($r('app.color.text_tertiary'))
                    .backgroundColor($r('app.color.surface_subtle'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .borderRadius(10)
                    .onChange((value: string) => {
                      this.domain = value;
                    })
                }
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .margin({ bottom: 16 })

            Row() {
              Column() {
                FormField({ label: '用户名' }) {
                  TextInput({ placeholder: '用户名', text: this.username })
                    .width('100%')
                    .height(46)
                    .fontColor($r('app.color.text_primary'))
                    .placeholderColor($r('app.color.text_tertiary'))
                    .backgroundColor($r('app.color.surface_subtle'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .borderRadius(10)
                    .type(InputType.USER_NAME)
                    .enableAutoFill(this.autoFill)
                    .onChange((value: string) => {
                      this.username = value;
                    })
                }
              }
              .layoutWeight(1)
              .alignItems(HorizontalAlign.Start)

              Column() {
                FormField({
                  label: '密码',
                  hint: '连接成功后自动保存'
                }) {
                  TextInput({ placeholder: '密码', text: this.password })
                    .width('100%')
                    .height(46)
                    .fontColor($r('app.color.text_primary'))
                    .placeholderColor($r('app.color.text_tertiary'))
                    .backgroundColor($r('app.color.surface_subtle'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .borderRadius(10)
                    .type(InputType.Password)
                    .enableAutoFill(this.autoFill)
                    .onChange((value: string) => {
                      this.password = value;
                    })
                }
              }
              .layoutWeight(1)
              .margin({ left: 16 })
              .alignItems(HorizontalAlign.Start)
            }
            .width('100%')
            .margin({ bottom: 16 })
            }

            if (this.errorMessage.length > 0) {
              Text(this.errorMessage)
                .fontSize(14)
                .lineHeight(20)
                .fontColor($r('app.color.danger'))
                .width('100%')
                .margin({ top: 12 })
            }

            Row() {
              Button('保存')
                .layoutWeight(1)
                .height(48)
                .fontSize(16)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_primary'))
                .backgroundColor($r('app.color.control_bg'))
                .border({ width: 1, color: $r('app.color.control_border') })
                .borderRadius(12)
                .enabled(!this.busy())
                .onClick(() => {
                  this.handleSave();
                })

              Button() {
                Row() {
                  if (this.primaryBusy()) {
                    LoadingProgress()
                      .width(18)
                      .height(18)
                      .color($r('app.color.on_brand'))
                  }
                  Text(this.primaryLabel())
                    .fontSize(16)
                    .fontWeight(FontWeight.Medium)
                    .fontColor($r('app.color.on_brand'))
                    .margin({ left: this.primaryBusy() ? 8 : 0 })
                }
                .justifyContent(FlexAlign.Center)
                .alignItems(VerticalAlign.Center)
              }
              .layoutWeight(1)
              .height(48)
              .backgroundColor($r('app.color.brand_fill'))
              .borderRadius(12)
              .margin({ left: 12 })
              .enabled(!this.pairing)
              .onClick(() => {
                if (this.connecting) {
                  this.cancelConnect();
                } else {
                  this.submit(true);
                }
              })
            }
            .width('100%')
            .margin({ top: 20 })

            if (this.editing) {
              Button('删除')
                .width('100%')
                .height(48)
                .fontSize(16)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.danger'))
                .backgroundColor($r('app.color.danger_tint'))
                .borderRadius(12)
                .margin({ top: 12 })
                .enabled(!this.busy())
                .onClick(() => {
                  this.confirmDeleteCurrent();
                })
            }

            if (this.isMoonlight()) {
              this.moonlightAdvanced()
            } else {
            Divider()
              .strokeWidth(1)
              .color($r('app.color.divider'))
              .margin({ top: 20, bottom: 8 })

            Row() {
              Column() {
                Text('高级设置').fontSize(14)
                Text('默认跟随全局；展开可覆盖')
                  .fontSize(12).fontColor($r('app.color.text_tertiary'))
                  
                  .margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Row() {
                if (!this.useGlobalDisplay || !this.useGlobalAdvanced || !this.useGlobalSr) {
                  Text('已自定义')
                    .fontSize(12).fontColor($r('app.color.text_tertiary'))
                    
                    .margin({ right: 8 })
                }
                Text(this.advancedExpanded ? '收起' : '展开')
                  .fontSize(14)
                  
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
            .focusable(true)
            .focusOnTouch(true)

            if (this.advancedExpanded) {
              Column() {
                Row() {
                  Column() {
                    Text('使用会话独立显示设置').fontSize(14)
                    Text('本连接独立配置；关闭则跟随全局')
                      .fontSize(12).fontColor($r('app.color.text_tertiary'))
                      
                      .margin({ top: 2 })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalDisplay })
                    .selectedColor($r('app.color.brand_fill'))
                    
                    .onChange((isOn: boolean) => {
                      this.useGlobalDisplay = !isOn;
                    })
                }
                .width('100%')
                .height(58)

                if (this.useGlobalDisplay) {
                  Text('自动检测：' + this.detectedText + ' · 推荐缩放 '
                    + this.suggestedScale() + '%')
                    .fontSize(12).fontColor($r('app.color.text_tertiary'))
                    
                    .width('100%')
                } else {
                  Row() {
                    Column() {
                      Text('分辨率').fontSize(14)
                      Text('跟随当前屏幕或固定分辨率')
                        .fontSize(12).fontColor($r('app.color.text_tertiary'))
                        
                        .margin({ top: 2 })
                    }
                    .alignItems(HorizontalAlign.Start)
                    .layoutWeight(1)

                    Row() {
                      this.displayResModeChip('自动', true)
                      this.displayResModeChip('手动', false)
                    }
                  }
                  .width('100%')
                  .margin({ top: 12 })

                  if (this.displayResolutionAuto) {
                    Text('自动检测：' + this.detectedText)
                      .fontSize(12).fontColor($r('app.color.text_tertiary'))
                      
                      .width('100%')
                      .margin({ top: 6 })
                  } else {
                    Row() {
                      Column() {
                        FormField({ label: '分辨率宽度' }) {
                          TextInput({ placeholder: '1920', text: this.desktopWidthText })
                            .width('100%')
                            .height(46)
                            .fontColor($r('app.color.text_primary'))
                            .placeholderColor($r('app.color.text_tertiary'))
                            .backgroundColor($r('app.color.surface_subtle'))
                            .border({ width: 1, color: $r('app.color.border') })
                            .borderRadius(10)
                            .type(InputType.Number)
                            .onChange((value: string) => {
                              this.desktopWidthText = value;
                            })
                        }
                      }
                      .layoutWeight(1)
                      .alignItems(HorizontalAlign.Start)

                      Column() {
                        FormField({ label: '分辨率高度' }) {
                          TextInput({ placeholder: '1080', text: this.desktopHeightText })
                            .width('100%')
                            .height(46)
                            .fontColor($r('app.color.text_primary'))
                            .placeholderColor($r('app.color.text_tertiary'))
                            .backgroundColor($r('app.color.surface_subtle'))
                            .border({ width: 1, color: $r('app.color.border') })
                            .borderRadius(10)
                            .type(InputType.Number)
                            .onChange((value: string) => {
                              this.desktopHeightText = value;
                            })
                        }
                      }
                      .layoutWeight(1)
                      .margin({ left: 16 })
                      .alignItems(HorizontalAlign.Start)
                    }
                    .width('100%')
                    .margin({ top: 12 })

                    Text('自动检测：' + this.detectedText)
                      .fontSize(12).fontColor($r('app.color.text_tertiary'))
                      
                      .width('100%')
                      .margin({ top: 6 })
                  }

                  Column() {
                    ScaleSetting({
                      mode: this.displayScaleAuto ? ML_SCALE_AUTO : ML_SCALE_MANUAL,
                      allowNone: false,
                      allowCustomInput: true,
                      percentText: this.scaleText,
                      autoPercent: this.suggestedScale(),
                      subtitle: '按分辨率推荐或固定比例',
                      onModeChange: (mode: number) => {
                        this.displayScaleAuto = mode === ML_SCALE_AUTO;
                      },
                      onPercentTextChange: (text: string) => {
                        this.scaleText = text;
                      }
                    })
                  }
                  .width('100%')
                  .margin({ top: 16 })
                }

                Divider()
                  .strokeWidth(1)
                  .color($r('app.color.divider'))
                  .margin({ top: 8, bottom: 4 })

                this.srAdvancedSection()

                Divider()
                  .strokeWidth(1)
                  .color($r('app.color.divider'))
                  .margin({ top: 8, bottom: 4 })

                Row() {
                  Column() {
                    Text('使用会话独立高级设置').fontSize(14)
                    Text('本连接独立配置；关闭则跟随全局')
                      .fontSize(12).fontColor($r('app.color.text_tertiary'))
                      
                      .margin({ top: 2 })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalAdvanced })
                    .selectedColor($r('app.color.brand_fill'))
                    
                    .onChange((isOn: boolean) => {
                      this.useGlobalAdvanced = !isOn;
                    })
                }
                .width('100%')
                .height(58)

                if (this.useGlobalAdvanced) {
                  Text('全局：' + this.advancedSummary(SettingsStore.globalAdvancedProfile()))
                    .fontSize(12).fontColor($r('app.color.text_tertiary'))
                    
                    .width('100%')
                } else {
                  this.optionRow('音频重定向', '', this.enableAudio, (v: boolean) => {
                    this.enableAudio = v;
                  }, this.audioSupported, this.audioDisabledReason)
                  this.optionRow('忽略证书校验', '仅用于自签名证书的测试环境', this.ignoreCertificate,
                    (v: boolean) => {
                      this.ignoreCertificate = v;
                    }, true, '')
                }
              }
              .width('100%')
              .alignItems(HorizontalAlign.Start)
            }
            }
          }
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
    
  }
}


