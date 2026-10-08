/*
 * HmRdp - add / edit a saved connection.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { AdvancedProfile, DisplayProfile, EditConnectionParams,
  ML_AUDIO_51, ML_AUDIO_71, ML_AUDIO_STEREO, ML_BITRATE_DEFAULT_KBPS, ML_BITRATE_MAX_KBPS,
  ML_BITRATE_MIN_KBPS, ML_CODEC_AUTO, ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC,
  ML_DEFAULT_PORT, ML_FPS_PRESETS, ML_RES_FIXED, ML_RES_FOLLOW_SCREEN, ML_RES_FOLLOW_WINDOW,
  MoonlightProfile, PendingConnection, PROTOCOL_MOONLIGHT, PROTOCOL_RDP, RdpConnectOptions,
  RdpConnectOrigin, SavedConnection, SR_BACKEND_FSR, SR_BACKEND_XENGINE,
  SrProfile } from '../model/RdpModels';
import { ConnectionStore } from '../services/ConnectionStore';
import { CredentialStore } from '../services/CredentialStore';
import { PairResult } from '../moonlight/MoonlightModels';
import { Capability, CODEC_SUPPORT_NONE, DeviceCapabilities } from '../services/DeviceCapabilities';
import { ConnectionState, SessionManager, SessionStateListener } from '../services/SessionManager';
import { MoonlightConnector, MoonlightHostPolicy, MoonlightPairResult } from '../services/MoonlightConnector';
import { AppSettings, SCALE_PRESETS, SR_RATIO_PRESETS, SettingsStore,
  srRatioLabel } from '../services/SettingsStore';
import { isScalePreset, parsedTextValue, parseScaleValue, parseSizeValue } from '../utils/ScaleParse';
import { moonlightCodecSoftware, moonlightCodecSupport, moonlightCodecUsable } from '../utils/MoonlightCodec';
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
// The DNS-SD service type Sunshine / GameStream hosts advertise (mDNS search).
const MOONLIGHT_SERVICE_TYPE: string = '_nvstream._tcp';
// How long the mDNS search runs before it reports "nothing found".
const HOST_SEARCH_MS: number = 3000;

@Entry
@Component
struct EditConnectionPage {
  @State name: string = '';
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
  @State ignoreCertificate: boolean = true;
  // 超分: kept out of the display group on purpose - a LAN connection can follow
  // the (off) global default while the same host reached through a gateway wants
  // the bandwidth saving, and vice versa.
  @State useGlobalSr: boolean = true;
  @State srEnabled: boolean = false;
  @State srRatioPercent: number = SR_RATIO_PRESETS[0];
  @State srBackend: number = SR_BACKEND_XENGINE;
  @State srUsable: boolean = false;
  // Protocol this editor is for. An existing connection keeps its own protocol
  // (the segment is shown locked); a new one starts on what the caller passed.
  @State protocol: number = PROTOCOL_RDP;
  // --- Moonlight (Sunshine) stream settings. ---
  @State mlPaired: boolean = false;
  @State mlDefaultAppId: string = '';
  @State mlDefaultAppName: string = '';
  @State mlResMode: number = ML_RES_FOLLOW_WINDOW;
  @State mlWidthText: string = '1920';
  @State mlHeightText: string = '1080';
  @State mlFps: number = 60;
  @State mlBitrateText: string = ML_BITRATE_DEFAULT_KBPS.toString();
  @State mlHdr: boolean = false;
  @State mlCodec: number = ML_CODEC_AUTO;
  @State mlYuv444: boolean = false;
  @State mlAudioConfig: number = ML_AUDIO_STEREO;
  @State mlGamepad: boolean = true;
  // Moonlight: follow the global stream defaults unless overridden here (same
  // pattern as the RDP display / 超分 / advanced groups).
  @State useGlobalStream: boolean = true;
  // Per-codec decoder support (CODEC_SUPPORT_*) of this device, read once so
  // the chips can grey what cannot run and warn about software-only codecs.
  private mlH264Support: number = CODEC_SUPPORT_NONE;
  private mlHevcSupport: number = CODEC_SUPPORT_NONE;
  private mlAv1Support: number = CODEC_SUPPORT_NONE;
  @State errorMessage: string = '';
  @State editing: boolean = false;
  @State detectedText: string = '';
  @State connecting: boolean = false;
  // Moonlight's 配对 runs in the editor and may show a PIN popup; a session is
  // never opened here (the list page owns connect).
  @State pairing: boolean = false;
  private connectionId: string = '';
  // Id of the in-flight attempt (may be a brand-new id for a new connection).
  private activeConnectionId: string = '';
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  // mDNS host search: the search button shows the spinner itself while running
  // (no loading dialog); results open a picker, an empty result is a toast.
  @State searching: boolean = false;
  private discovery: MdnsDiscovery = new MdnsDiscovery();
  private searchTimer: number = -1;
  private discovered: MdnsHost[] = [];
  // The host of the Moonlight connection being edited, so a changed host can be
  // unpaired (the pairing is per host, not per connection).
  private originalMoonlightHost: string = '';
  // Its stored host identity (`mlHostId`): carried over on save, dropped when the
  // host changes (the id, pairing and default app all belonged to the old host).
  private originalMoonlightHostId: string = '';
  private storedPassword: string = '';
  private returned: boolean = false;

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
    this.mlH264Support = DeviceCapabilities.codecSupport(ML_CODEC_H264);
    this.mlHevcSupport = DeviceCapabilities.codecSupport(ML_CODEC_HEVC);
    this.mlAv1Support = DeviceCapabilities.codecSupport(ML_CODEC_AV1);
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
    // Moonlight stream defaults for a new connection, straight from the global
    // settings; an existing connection overrides them below.
    const mlDefaults: AppSettings = SettingsStore.load();
    this.mlFps = mlDefaults.mlFps;
    this.mlBitrateText = mlDefaults.mlBitrateKbps.toString();
    this.mlHdr = mlDefaults.mlHdr;
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
      this.originalMoonlightHostId = conn.mlHostId;
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
      this.ignoreCertificate = conn.ignoreCertificate;
      this.useGlobalAdvanced = conn.useGlobalAdvanced;
      this.useGlobalSr = conn.useGlobalSr;
      this.srEnabled = conn.srEnabled;
      this.srRatioPercent = SettingsStore.snapSrRatioPreset(conn.srRatioPercent);
      this.srBackend = conn.srBackend;
      this.mlPaired = conn.mlPaired;
      this.mlDefaultAppId = conn.mlDefaultAppId;
      this.mlDefaultAppName = conn.mlDefaultAppName;
      this.mlResMode = conn.mlResMode;
      if (conn.mlWidth > 0 && conn.mlHeight > 0) {
        this.mlWidthText = conn.mlWidth.toString();
        this.mlHeightText = conn.mlHeight.toString();
      }
      this.mlFps = conn.mlFps;
      this.mlBitrateText = conn.mlBitrateKbps.toString();
      this.mlHdr = conn.mlHdr;
      this.mlCodec = conn.mlCodec;
      this.clampMlCodec();
      this.mlYuv444 = conn.mlYuv444;
      this.mlAudioConfig = conn.mlAudioConfig;
      this.mlGamepad = conn.mlGamepad;
      this.useGlobalStream = conn.useGlobalStream;
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

  private parsedBitrate(): number {
    const value: number = Number.parseInt(this.mlBitrateText, 10);
    return Number.isNaN(value) ? ML_BITRATE_DEFAULT_KBPS : value;
  }

  private effectiveBitrate(): number {
    return Math.min(Math.max(this.parsedBitrate(), ML_BITRATE_MIN_KBPS), ML_BITRATE_MAX_KBPS);
  }

  private srSummary(profile: SrProfile): string {
    return profile.enabled ? `${this.onOff(true)} ${srRatioLabel(profile.ratioPercent)}` : '关';
  }

  private advancedSummary(profile: AdvancedProfile): string {
    return `音频${this.onOff(profile.enableAudio)}`
      + ` · 忽略证书${this.onOff(profile.ignoreCertificate)}`;
  }

  private resModeLabel(mode: number): string {
    if (mode === ML_RES_FIXED) {
      return '固定';
    }
    return mode === ML_RES_FOLLOW_SCREEN ? '跟随屏幕' : '跟随窗口';
  }

  private streamSummary(profile: MoonlightProfile): string {
    const res: string = profile.resMode === ML_RES_FIXED
      ? `${profile.width}×${profile.height}` : this.resModeLabel(profile.resMode);
    return `${res} · ${profile.fps}fps · ${profile.bitrateKbps}kbps`
      + (profile.hdr ? ' · HDR' : '');
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
    options.name = this.name.trim();
    options.width = this.useGlobalDisplay
      ? global.width : this.parseSize(this.desktopWidthText, global.width);
    options.height = this.useGlobalDisplay
      ? global.height : this.parseSize(this.desktopHeightText, global.height);
    options.scalePercent = this.useGlobalDisplay
      ? global.scalePercent : this.parseScale(this.scaleText, this.suggestedScale());
    options.enableAudio = this.enableAudio;
    options.ignoreCertificate = this.ignoreCertificate;
    const sr: SrProfile = this.resolvedSr();
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
    // Stay on the editor while connecting: on failure the typed password is
    // preserved so the user can correct it; on success onConnected returns.
  }

  /**
   * Persists a Moonlight connection. The editor's primary action pairs (see
   * pairMoonlight); 保存 only stores the form. Connecting a stream is done from
   * the list page.
   */
  private submitMoonlight(connect: boolean): void {
    const target: string = this.host.trim();
    if (target.length === 0) {
      this.errorMessage = '请输入主机名或 IP 地址';
      return;
    }
    // Address pre-screen (cheap, and consistent with the picker's "already
    // added" marking): never start a pair/connect for an address already in use.
    // The authoritative check is the host's own id, learned in the pre-flight
    // (MoonlightHostPolicy) - it catches the same host under a different address.
    if (this.conflictingMoonlightHost(target) !== undefined) {
      this.errorMessage = '该主机地址已添加，同一主机只能有一个 Moonlight 连接';
      return;
    }
    this.errorMessage = '';
    // The host's identity (`mlHostId`) is only learned from serverinfo, so a
    // save-only edit carries over the id it already had; a changed host drops it
    // (the id, pairing and default app all belonged to the old host). Duplicate
    // detection happens where the id is learned - at pair / connect time.
    const hostChanged: boolean = this.editing
      && this.normalizeHost(target) !== this.normalizeHost(this.originalMoonlightHost);
    const id: string = this.connectionId.length > 0
      ? this.connectionId : util.generateRandomUUID(true);
    const conn: SavedConnection = new SavedConnection();
    conn.id = id;
    conn.protocol = PROTOCOL_MOONLIGHT;
    conn.host = target;
    conn.port = this.parseMoonlightPort();
    conn.name = this.name.trim();
    conn.mlHostId = hostChanged ? '' : this.originalMoonlightHostId;
    conn.mlPaired = hostChanged ? false : this.mlPaired;
    conn.mlDefaultAppId = hostChanged ? '' : this.mlDefaultAppId;
    conn.mlDefaultAppName = hostChanged ? '' : this.mlDefaultAppName;
    conn.mlResMode = this.mlResMode;
    conn.mlWidth = this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlWidthText, 1920) : 0;
    conn.mlHeight = this.mlResMode === ML_RES_FIXED
      ? this.parseSize(this.mlHeightText, 1080) : 0;
    conn.mlFps = this.mlFps;
    conn.mlBitrateKbps = this.effectiveBitrate();
    conn.mlHdr = this.mlHdr;
    conn.mlCodec = this.mlCodec;
    conn.mlYuv444 = this.mlYuv444;
    conn.mlAudioConfig = this.mlAudioConfig;
    conn.mlGamepad = this.mlGamepad;
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

  /**
   * Moonlight: the editor pairs only. It runs the control layer's pairing step
   * (serverinfo -> pair, with the PIN popup when the host is not paired yet) and
   * persists the connection once paired. Opening a stream session is the list
   * page's connect button, never this page.
   */
  private pairMoonlight(conn: SavedConnection): void {
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
        conn.mlPaired = true;
        conn.mlHostId = result.hostId;
        this.mlPaired = true;
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

  /**
   * Search the LAN (mDNS) for hosts. The search button carries the loading
   * state itself; when the window closes the results open a picker, or an empty
   * result is reported with a toast. Filling is host always, name only when the
   * field is still empty so a typed name is never overwritten.
   */
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
      showToast(this.getUIContext(), '未发现主机，请确认主机已开启且与本机在同一局域网');
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

  /** Host identity for string comparison: case- and trailing-dot-insensitive. */
  private normalizeHost(host: string): string {
    let value: string = host.trim().toLowerCase();
    while (value.endsWith('.')) {
      value = value.substring(0, value.length - 1);
    }
    return value;
  }

  /** This connection's stored id plus the other connections' ids, for pairing. */
  private moonlightHostPolicy(): MoonlightHostPolicy {
    const policy: MoonlightHostPolicy = new MoonlightHostPolicy();
    policy.expectedId = this.originalMoonlightHostId;
    const all: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < all.length; i++) {
      const conn: SavedConnection = all[i];
      if (conn.isMoonlight() && conn.id !== this.connectionId && conn.mlHostId.length > 0) {
        policy.knownIds.push(conn.mlHostId);
      }
    }
    return policy;
  }

  /**
   * Address pre-screen: a saved Moonlight connection whose normalized address
   * matches (excluding self), or undefined. Cheap, and consistent with the
   * picker's "already added" marking; the authoritative check is the host's own
   * id (`mlHostId`), applied once serverinfo names it.
   */
  private conflictingMoonlightHost(host: string): string | undefined {
    const target: string = this.normalizeHost(host);
    const all: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < all.length; i++) {
      const conn: SavedConnection = all[i];
      if (!conn.isMoonlight() || conn.id === this.connectionId) {
        continue;
      }
      if (this.normalizeHost(conn.host) === target) {
        return conn.label();
      }
    }
    return undefined;
  }

  /** Normalized hosts already saved as Moonlight connections (picker greys them). */
  private addedMoonlightAddresses(): string[] {
    const result: string[] = [];
    const all: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < all.length; i++) {
      const conn: SavedConnection = all[i];
      if (conn.isMoonlight()) {
        result.push(this.normalizeHost(conn.host));
      }
    }
    return result;
  }

  /** Drops the pairing for `host` (local pin now, host best-effort). */
  private forgetPairing(host: string): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined || host.trim().length === 0) {
      return;
    }
    MoonlightConnector.forgetHost((context as common.UIAbilityContext).filesDir,
      host, this.parseMoonlightPort());
  }

  /** When the edited connection's host changed, unpair the old host. */
  private forgetReplacedHost(newHost: string): void {
    const old: string = this.originalMoonlightHost.trim();
    if (old.length === 0 || this.normalizeHost(old) === this.normalizeHost(newHost)) {
      return;
    }
    this.forgetPairing(old);
    this.originalMoonlightHost = newHost.trim();
    this.originalMoonlightHostId = '';
  }

  private persist(conn: SavedConnection): boolean {
    try {
      ConnectionStore.save(conn);
      // Remember the protocol so the next new connection starts on this kind.
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

  /** True while the editor is doing something the form must not disturb. */
  private busy(): boolean {
    return this.connecting || this.pairing;
  }

  /** The primary button's label: Moonlight pairs, RDP connects (or cancels). */
  private primaryLabel(): string {
    if (this.isMoonlight()) {
      return '配对';
    }
    return this.connecting ? '取消' : '连接';
  }

  /** Whether the primary button shows a spinner (RDP connect / Moonlight pair). */
  private primaryBusy(): boolean {
    return this.connecting || this.pairing;
  }

  private handleSave(): void {
    if (this.password.length > 0 && this.password !== this.storedPassword) {
      showConfirm(this.getUIContext(), {
        title: '密码不会被保存',
        message: '连接验证正确性后会自动保存密码。',
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
      if (this.isMoonlight()) {
        // Drop the host pairing with the connection: the pairing is per host, so
        // leaving it behind would let a re-add skip the PIN round.
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
  mlResModeChip(mode: number, label: string) {
    ChoiceChip({
      label: label,
      selected: this.mlResMode === mode,
      onTap: () => {
        this.mlResMode = mode;
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
      }
    })
  }

  @Builder
  mlAudioChip(config: number, label: string) {
    ChoiceChip({
      label: label,
      selected: this.mlAudioConfig === config,
      onTap: () => {
        this.mlAudioConfig = config;
      }
    })
  }

  @Builder
  moonlightForm() {
    Column() {
      // Name and the host-search entry share one row: the search button sits by
      // the (narrower) name box, aligned with the input rather than the label.
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
            // The control carries its own progress: no loading dialog.
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

  /** Moonlight's advanced overrides, shown below the action buttons like RDP's. */
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
            Text('分辨率').fontSize(14).fontColor($r('app.color.text_primary')).margin({ top: 4 })
            Flex({ wrap: FlexWrap.Wrap }) {
              this.mlResModeChip(ML_RES_FOLLOW_WINDOW, '跟随窗口')
              this.mlResModeChip(ML_RES_FOLLOW_SCREEN, '跟随屏幕')
              this.mlResModeChip(ML_RES_FIXED, '固定')
            }
            .width('100%').margin({ top: 8 })

            if (this.mlResMode === ML_RES_FIXED) {
              Row() {
                Column() {
                  this.fieldLabel('宽度')
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
                  this.fieldLabel('高度')
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
              .width('100%').margin({ bottom: 8 })
            }

            Text('帧率').fontSize(14).fontColor($r('app.color.text_primary')).margin({ top: 8 })
            Flex({ wrap: FlexWrap.Wrap }) {
              ForEach(ML_FPS_PRESETS, (preset: number) => {
                this.mlFpsChip(preset)
              }, (preset: number) => preset.toString())
            }
            .width('100%').margin({ top: 8 })

            Column() {
              this.fieldLabel('码率 (kbps)')
              TextInput({ placeholder: '20000', text: this.mlBitrateText })
                .width('100%').height(46).borderRadius(10)
                .backgroundColor($r('app.color.input_bg'))
                .type(InputType.Number)
                .onChange((value: string) => {
                  this.mlBitrateText = value;
                })
              Text('建议 ' + ML_BITRATE_MIN_KBPS + ' – ' + ML_BITRATE_MAX_KBPS + ' kbps')
                .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 4 })
            }
            .width('100%').alignItems(HorizontalAlign.Start).margin({ top: 4 })

            this.optionRow('HDR', '主机与显示器都支持时输出 HDR', this.mlHdr,
              (v: boolean) => {
                this.mlHdr = v;
              }, true, '')

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

            this.optionRow('YUV 4:4:4', '色度不抽样，文字与彩色边缘更清晰，但更占带宽',
              this.mlYuv444, (v: boolean) => {
                this.mlYuv444 = v;
              }, true, '')

            Text('音频通道').fontSize(14).fontColor($r('app.color.text_primary'))
              .margin({ top: 8 })
            Flex({ wrap: FlexWrap.Wrap }) {
              this.mlAudioChip(ML_AUDIO_STEREO, '立体声')
              this.mlAudioChip(ML_AUDIO_51, '5.1')
              this.mlAudioChip(ML_AUDIO_71, '7.1')
            }
            .width('100%').margin({ top: 8 })

            this.optionRow('手柄输入', '把连接的手柄转发给主机', this.mlGamepad,
              (v: boolean) => {
                this.mlGamepad = v;
              }, true, '')
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
            // Move the port with the protocol, unless the user already typed one.
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
              // Pairing has no session to cancel, so it locks the button; an RDP
              // connect stays tappable to cancel.
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
