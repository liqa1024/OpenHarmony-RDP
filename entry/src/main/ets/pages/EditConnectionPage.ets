import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { AdvancedProfile, DisplayProfile, RdpConnectOptions,
  SR_BACKEND_FSR, SR_BACKEND_XENGINE, SrProfile } from '../model/RdpModels';
import { ConnectOrigin, EditConnectionParams,
  ML_BITRATE_DEFAULT_KBPS, ML_BITRATE_MAX_KBPS,
  ML_BITRATE_MIN_KBPS, ML_CODEC_AUTO, ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC,
  ML_COLORSPACE_REC601, ML_COLORSPACE_REC709,
  ML_DEFAULT_PORT, ML_FPS_PRESETS, ML_RES_FIXED, ML_RES_FOLLOW_SCREEN,
  MoonlightConnection, MoonlightProfile, PendingConnection, PROTOCOL_MOONLIGHT, PROTOCOL_RDP,
  RdpConnection, SavedConnection } from '../model/Connections';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { PairResult } from '../moonlight/MoonlightModels';
import { Capability, CODEC_SUPPORT_NONE, DeviceCapabilities } from '../services/DeviceCapabilities';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { MoonlightConnector, MoonlightHostPolicy, MoonlightKnownHost, MoonlightPairResult }
  from '../services/MoonlightConnector';
import { AppSettings, SCALE_PRESETS, SR_RATIO_PRESETS, SettingsStore,
  srRatioLabel } from '../services/SettingsStore';
import { isScalePreset, parsedTextValue, parseScaleValue, parseSizeValue } from '../utils/ScaleParse';
import { moonlightCodecSoftware, moonlightCodecSupport, moonlightCodecUsable } from '../utils/MoonlightCodec';
import { clampFps, kbpsToMbps, mbpsToKbps, recommendedBitrateKbps } from '../utils/MoonlightQuality';
import { showConfirm, showToast } from '../utils/UiUtil';
import { promptPairingPin } from '../utils/PairingPrompt';
import { showHostPicker } from '../utils/HostSearchPrompt';
import { MdnsDiscovery, MdnsHost } from '../services/MdnsDiscovery';
import { ChoiceChip } from '../components/ChoiceChip';
import { HeaderBar } from '../components/HeaderBar';
import { MaterialCard } from '../components/MaterialCard';
import { ProtocolTabs } from '../components/ProtocolTabs';
import { SettingToggleRow } from '../components/SettingToggleRow';
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

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
  // Password Vault (doc_agent/settings-and-storage.md §1.1): the field types make
  // the pair a login form the system can fill, so autoFill must be cleared on leave.
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
  // 超分 stays a separate group from display on purpose: following the off global
  // default and wanting per-connection bandwidth saving can both be true.
  @State useGlobalSr: boolean = true;
  @State srEnabled: boolean = false;
  @State srRatioPercent: number = SR_RATIO_PRESETS[0];
  @State srBackend: number = SR_BACKEND_XENGINE;
  @State srUsable: boolean = false;
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
  private mlH264Support: number = CODEC_SUPPORT_NONE;
  private mlHevcSupport: number = CODEC_SUPPORT_NONE;
  private mlAv1Support: number = CODEC_SUPPORT_NONE;
  @State errorMessage: string = '';
  @State editing: boolean = false;
  @State detectedText: string = '';
  @State connecting: boolean = false;
  @State pairing: boolean = false;
  private connectionId: string = '';
  private activeConnectionId: string = '';
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
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
    this.srUsable = SettingsStore.superResolutionUsable();
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

  private parsedScale(): number {
    return parsedTextValue(this.scaleText);
  }

  private isPresetScale(value: number): boolean {
    return isScalePreset(value);
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

  // 码率输入以 Mbps 计，存储仍是 kbps。
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
    if (!this.srUsable) {
      return false;
    }
    const token: string = choice === 2 ? 'fsr' : 'xengine';
    return DeviceCapabilities.srBackends().indexOf(token) >= 0;
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

  private buildOptions(applyGlobalAdvanced: boolean): RdpConnectOptions | undefined {
    const target: string = this.host.trim();
    if (target.length === 0) {
      this.errorMessage = '请输入主机名或 IP 地址';
      return undefined;
    }
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
      this.errorMessage = '请输入主机名或 IP 地址';
      return;
    }
    const conflict: string | undefined = this.conflictingMoonlightHost(target);
    if (conflict !== undefined) {
      this.errorMessage = `主机 ${conflict} 已添加，请先删除该连接`;
      return;
    }
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
      (pin: string, pairing: Promise<PairResult>): Promise<boolean> =>
        promptPairingPin(this.getUIContext(), pin, pairing),
      this.moonlightHostPolicy())
      .then((result: MoonlightPairResult) => {
        this.pairing = false;
        if (!result.ok) {
          this.errorMessage = result.error;
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
  fieldLabel(text: string) {
    Text(text)
      .fontSize(12)
      .fontColor($r('app.color.text_secondary'))
      .margin({ bottom: 6 })
  }

  @Builder
  scaleChip(preset: number) {
    ChoiceChip({
      label: preset.toString() + '%',
      selected: this.parsedScale() === preset,
      onTap: () => {
        this.scaleText = preset.toString();
      }
    })
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
  displayScaleModeChip(label: string, auto: boolean) {
    ChoiceChip({
      label: label,
      selected: this.displayScaleAuto === auto,
      onTap: () => {
        this.displayScaleAuto = auto;
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
      })
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
      }
    })
  }

  @Builder
  moonlightForm() {
    Column() {
      Row() {
        Column() {
          this.fieldLabel('名称（可选）')
          TextInput({ placeholder: '自定义名称，留空则显示主机地址', text: this.name })
            .width('100%')
            .height(46)
            .borderRadius(10)
            .backgroundColor($r('app.color.input_bg'))
            .onChange((value: string) => {
              this.name = value;
            })
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        Row() {
          if (this.searching) {
            LoadingProgress()
              .width(20)
              .height(20)
              .color($r('app.color.accent'))
          } else {
            Image($r('app.media.ic_search'))
              .width(20)
              .height(20)
              .fillColor($r('app.color.accent'))
              .draggable(false)
          }
        }
        .width(46)
        .height(46)
        .borderRadius(10)
        .backgroundColor($r('app.color.input_bg'))
        .justifyContent(FlexAlign.Center)
        .alignItems(VerticalAlign.Center)
        .margin({ left: 12 })
        .hoverEffect(HoverEffect.Highlight)
        .opacity(this.searching ? 0.7 : 1)
        .bindTips(this.searching ? '正在搜索…' : '自动搜索局域网主机', this.tipsOptions)
        .onClick(() => {
          this.openHostSearch();
        })
      }
      .width('100%')
      .alignItems(VerticalAlign.Bottom)
      .margin({ bottom: 16 })

      Row() {
        Column() {
          this.fieldLabel('主机名或 IP 地址')
          TextInput({ placeholder: '例如 192.168.1.100', text: this.host })
            .width('100%')
            .height(46)
            .borderRadius(10)
            .backgroundColor($r('app.color.input_bg'))
            .onChange((value: string) => {
              this.host = value;
            })
        }
        .layoutWeight(3)
        .alignItems(HorizontalAlign.Start)

        Column() {
          this.fieldLabel('端口')
          TextInput({ placeholder: '47989', text: this.port })
            .width('100%')
            .height(46)
            .borderRadius(10)
            .backgroundColor($r('app.color.input_bg'))
            .type(InputType.Number)
            .onChange((value: string) => {
              this.port = value;
            })
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
          if (!this.useGlobalStream) {
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
              Text('使用会话独立串流设置').fontSize(14).fontColor($r('app.color.text_primary'))
              Text('分辨率、帧率、码率等使用本连接的独立配置，关闭则跟随全局设置')
                .fontSize(11)
                .fontColor($r('app.color.text_tertiary'))
                .margin({ top: 2 })
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)

            Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalStream })
              .selectedColor($r('app.color.accent'))
              .onChange((isOn: boolean) => {
                this.useGlobalStream = !isOn;
              })
          }
          .width('100%')
          .height(58)

          if (this.useGlobalStream) {
            Text('全局：' + this.streamSummary(SettingsStore.globalStreamProfile()))
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
          } else {
            Row() {
              Column() {
                Text('分辨率').fontSize(14).fontColor($r('app.color.text_primary'))
                Text('自动跟随当前屏幕，或固定一个分辨率')
                  .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
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
                .fontSize(11).fontColor($r('app.color.text_tertiary'))
                .width('100%').margin({ top: 6 })
            } else {
              Row() {
                Column() {
                  this.fieldLabel('分辨率宽度')
                  TextInput({ placeholder: '1920', text: this.mlWidthText })
                    .width('100%').height(46).borderRadius(10)
                    .backgroundColor($r('app.color.input_bg'))
                    .type(InputType.Number)
                    .onChange((value: string) => {
                      this.mlWidthText = value;
                    })
                }
                .layoutWeight(1).alignItems(HorizontalAlign.Start)

                Column() {
                  this.fieldLabel('分辨率高度')
                  TextInput({ placeholder: '1080', text: this.mlHeightText })
                    .width('100%').height(46).borderRadius(10)
                    .backgroundColor($r('app.color.input_bg'))
                    .type(InputType.Number)
                    .onChange((value: string) => {
                      this.mlHeightText = value;
                    })
                }
                .layoutWeight(1).margin({ left: 16 }).alignItems(HorizontalAlign.Start)
              }
              .width('100%').margin({ top: 12 })

              Text('自动检测：' + this.detectedText)
                .fontSize(11).fontColor($r('app.color.text_tertiary'))
                .width('100%').margin({ top: 6 })
            }

            Text('帧率').fontSize(14).fontColor($r('app.color.text_primary')).margin({ top: 12 })
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
                  .width(140).height(46).borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.mlFpsText = value;
                    this.syncFpsText();
                  })
                Text('fps')
                  .fontSize(14).fontColor($r('app.color.text_tertiary')).margin({ left: 10 })
              }
              .width('100%').margin({ top: 4 })
            }

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
            .width('100%').margin({ top: 12 })

            if (this.mlBitrateAuto) {
              Text('自动：约 ' + kbpsToMbps(this.mlAutoBitrateKbps()) + ' Mbps')
                .fontSize(11).fontColor($r('app.color.text_tertiary'))
                .width('100%').margin({ top: 6 })
            } else {
              Row() {
                TextInput({ placeholder: '20', text: this.mlBitrateText })
                  .width(140).height(46).borderRadius(10)
                  .backgroundColor($r('app.color.input_bg'))
                  .type(InputType.Number)
                  .onChange((value: string) => {
                    this.mlBitrateText = value;
                  })
                Text('Mbps')
                  .fontSize(14).fontColor($r('app.color.text_tertiary')).margin({ left: 10 })
              }
              .width('100%').margin({ top: 6 })
            }

            Text('编码器').fontSize(14).fontColor($r('app.color.text_primary')).margin({ top: 8 })
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

            Text('色彩空间').fontSize(14).fontColor($r('app.color.text_primary')).margin({ top: 8 })
            Flex({ wrap: FlexWrap.Wrap }) {
              this.mlColorSpaceChip(ML_COLORSPACE_REC709, 'Rec709')
              this.mlColorSpaceChip(ML_COLORSPACE_REC601, 'Rec601')
            }
            .width('100%').margin({ top: 8 })
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
      HeaderBar({
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
            if (untouched) {
              this.port = (protocol === PROTOCOL_MOONLIGHT
                ? ML_DEFAULT_PORT : RDP_DEFAULT_PORT).toString();
            }
          }
        })
      }

      Scroll() {
        Column() {
          MaterialCard({ pad: 28 }) {
            if (this.isMoonlight()) {
              this.moonlightForm()
            } else {
            this.fieldLabel('名称（可选）')
            TextInput({ placeholder: '自定义名称，留空则显示主机地址', text: this.name })
              .width('100%')
              .height(46)
              .borderRadius(10)
              .backgroundColor($r('app.color.input_bg'))
              .margin({ bottom: 16 })
              .onChange((value: string) => {
                this.name = value;
              })

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
            }

            if (this.errorMessage.length > 0) {
              Text(this.errorMessage)
                .fontSize(13)
                .fontColor($r('app.color.danger'))
                .width('100%')
                .margin({ top: 12 })
            }

            Row() {
              Button('保存')
                .layoutWeight(1)
                .height(48)
                .fontSize(16)
                .fontColor($r('app.color.accent'))
                .backgroundColor($r('app.color.input_bg'))
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
                      .color('#FFFFFF')
                  }
                  Text(this.primaryLabel())
                    .fontSize(16)
                    .fontWeight(FontWeight.Medium)
                    .fontColor('#FFFFFF')
                    .margin({ left: this.primaryBusy() ? 8 : 0 })
                }
                .justifyContent(FlexAlign.Center)
                .alignItems(VerticalAlign.Center)
              }
              .layoutWeight(1)
              .height(48)
              .backgroundColor($r('app.color.accent'))
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
                .fontColor($r('app.color.danger'))
                .backgroundColor($r('app.color.input_bg'))
                .borderRadius(12)
                .margin({ top: 12 })
                .enabled(!this.busy())
                .onClick(() => {
                  this.deleteCurrent();
                })
            }

            if (this.isMoonlight()) {
              this.moonlightAdvanced()
            } else {
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
                    Text('使用会话独立显示设置').fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('分辨率与缩放使用本连接的独立配置，关闭则跟随全局设置')
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .margin({ top: 2 })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalDisplay })
                    .selectedColor($r('app.color.accent'))
                    .onChange((isOn: boolean) => {
                      this.useGlobalDisplay = !isOn;
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
                      Text('分辨率').fontSize(14).fontColor($r('app.color.text_primary'))
                      Text('自动跟随当前屏幕，或固定一个分辨率')
                        .fontSize(11)
                        .fontColor($r('app.color.text_tertiary'))
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
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .width('100%')
                      .margin({ top: 6 })
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
                  }

                  Row() {
                    Column() {
                      Text('缩放比例').fontSize(14).fontColor($r('app.color.text_primary'))
                      Text('自动按分辨率推荐，或固定一个比例')
                        .fontSize(11)
                        .fontColor($r('app.color.text_tertiary'))
                        .margin({ top: 2 })
                    }
                    .alignItems(HorizontalAlign.Start)
                    .layoutWeight(1)

                    Row() {
                      this.displayScaleModeChip('自动', true)
                      this.displayScaleModeChip('手动', false)
                    }
                  }
                  .width('100%')
                  .margin({ top: 16 })

                  if (this.displayScaleAuto) {
                    Text('推荐 ' + this.suggestedScale() + '%')
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .width('100%')
                      .margin({ top: 6 })
                  } else {
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
                }

                Divider()
                  .color($r('app.color.divider'))
                  .margin({ top: 8, bottom: 4 })

                Row() {
                  Column() {
                    Text('使用会话独立超分辨率设置')
                      .fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('超分辨率：远端按输出分辨率÷倍率出画，本机 GPU 上采样回输出分辨率；关闭则跟随全局设置')
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

                  Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalSr })
                    .enabled(this.srUsable)
                    .selectedColor($r('app.color.accent'))
                    .onChange((isOn: boolean) => {
                      if (!this.srUsable) {
                        return;
                      }
                      this.useGlobalSr = !isOn;
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
                    Text('使用会话独立高级设置').fontSize(14).fontColor($r('app.color.text_primary'))
                    Text('音频、证书等使用本连接的独立配置，关闭则跟随全局设置')
                      .fontSize(11)
                      .fontColor($r('app.color.text_tertiary'))
                      .margin({ top: 2 })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)

                  Toggle({ type: ToggleType.Switch, isOn: !this.useGlobalAdvanced })
                    .selectedColor($r('app.color.accent'))
                    .onChange((isOn: boolean) => {
                      this.useGlobalAdvanced = !isOn;
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
    .backgroundColor($r('app.color.page_bg'))
  }
}
