/*
 * HmRdp - Moonlight connect pre-flight (ArkTS control layer).
 *
 * Runs the GameStream control sequence that has to happen before a stream
 * exists: serverinfo -> (pair) -> launch, and assembles the native stream
 * config. Pairing is interactive (the client shows a PIN the user types on the
 * host), so the caller supplies a `pinProvider` that presents the lightweight
 * PIN popup over the main window - no session window is opened during pairing.
 * See MOONLIGHT_PLAN.md / doc_agent/moonlight-native.md §4.
 */
import { moonlightVideoFormats } from 'libhmrdp.so';
import { hilog } from '@kit.PerformanceAnalysisKit';
import {
  ML_AUDIO_51, ML_AUDIO_71, ML_AUDIO_STEREO, MoonlightProfile, MoonlightStreamConfig
} from '../model/RdpModels';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { MoonlightCrypto } from '../moonlight/MoonlightCrypto';
import { MoonlightIdentity, MoonlightCredential } from '../moonlight/MoonlightIdentity';
import {
  AppListResult, LaunchResult, MoonlightLaunchOptions, NvApp, NvServerInfo, PairResult,
  ServerInfoResult
} from '../moonlight/MoonlightModels';

/**
 * Presents the 4-digit PIN and drives the wait for the host to enter it.
 * `pairing` settles when the handshake finishes (success or failure), which is
 * what closes the prompt; the returned promise is `false` only when the user
 * cancels, `true` otherwise.
 */
export type MoonlightPinProvider = (pin: string, pairing: Promise<PairResult>) => Promise<boolean>;

export class MoonlightConnectResult {
  ok: boolean = false;
  // User-facing reason when !ok.
  error: string = '';
  config: MoonlightStreamConfig = new MoonlightStreamConfig();
  // The host's HTTPS control port, kept for the session's /cancel.
  httpsPort: number = 47984;
}

/** Result of probing a host for the app-management UI. */
export class MoonlightAppsResult {
  ok: boolean = false;
  error: string = '';
  apps: NvApp[] = [];
  // App currently running on the host (0 = none), from /serverinfo.
  currentGame: number = 0;
  httpsPort: number = 47984;
}

/** Result of a pairing-only attempt (the editor's 配对 action). */
export class MoonlightPairResult {
  ok: boolean = false;
  error: string = '';
}

export class MoonlightConnector {
  /**
   * Read-only probe for the app-management UI: ensure paired (PIN popup when
   * needed), then read the host's app list and the running app. Does not launch.
   */
  static async probeApps(filesDir: string, host: string, port: number,
    pinProvider: MoonlightPinProvider): Promise<MoonlightAppsResult> {
    const result: MoonlightAppsResult = new MoonlightAppsResult();
    const credential: MoonlightCredential = await MoonlightIdentity.ensureCredential(filesDir);
    if (!credential.ok) {
      result.error = MoonlightConnector.describe(credential.error);
      return result;
    }
    const serverInfo: ServerInfoResult = await MoonlightClient.fetchServerInfo(filesDir, host, port);
    if (!serverInfo.ok) {
      result.error = MoonlightConnector.describe(serverInfo.error);
      return result;
    }
    const info: NvServerInfo = serverInfo.info;
    result.httpsPort = info.httpsPort;
    result.currentGame = info.currentGame;
    const paired: boolean = info.paired
      || MoonlightIdentity.loadPinnedServerCert(filesDir, host) !== undefined;
    if (!paired) {
      const pin: string = MoonlightConnector.generatePin();
      const pairing: Promise<PairResult> =
        MoonlightClient.pair(filesDir, host, port, info.httpsPort, info.majorVersion, pin);
      const proceed: boolean = await pinProvider(pin, pairing);
      if (!proceed) {
        result.error = '已取消配对';
        return result;
      }
      const pairResult: PairResult = await pairing;
      if (!pairResult.ok) {
        result.error = MoonlightConnector.describe(pairResult.error);
        return result;
      }
    }
    const apps: AppListResult = await MoonlightClient.fetchAppList(filesDir, host, info.httpsPort);
    if (!apps.ok) {
      result.error = MoonlightConnector.describe(apps.error);
      return result;
    }
    result.apps = apps.apps;
    result.ok = true;
    return result;
  }

  /**
   * Pairs with the host only: serverinfo -> (pair), with the PIN popup when
   * needed. No session is opened and the app list is not read - the editor's
   * 配对 action; connecting a session is the list page's job.
   */
  static async pairOnly(filesDir: string, host: string, port: number,
    pinProvider: MoonlightPinProvider): Promise<MoonlightPairResult> {
    const result: MoonlightPairResult = new MoonlightPairResult();
    const credential: MoonlightCredential = await MoonlightIdentity.ensureCredential(filesDir);
    if (!credential.ok) {
      result.error = MoonlightConnector.describe(credential.error);
      return result;
    }
    const serverInfo: ServerInfoResult = await MoonlightClient.fetchServerInfo(filesDir, host, port);
    if (!serverInfo.ok) {
      result.error = MoonlightConnector.describe(serverInfo.error);
      return result;
    }
    const info: NvServerInfo = serverInfo.info;
    const paired: boolean = info.paired
      || MoonlightIdentity.loadPinnedServerCert(filesDir, host) !== undefined;
    if (!paired) {
      const pin: string = MoonlightConnector.generatePin();
      // Start the handshake first: the host only prompts for the PIN once our
      // getservercert request is pending. The prompt closes when it settles.
      const pairing: Promise<PairResult> =
        MoonlightClient.pair(filesDir, host, port, info.httpsPort, info.majorVersion, pin);
      const proceed: boolean = await pinProvider(pin, pairing);
      if (!proceed) {
        result.error = '已取消配对';
        return result;
      }
      const pairResult: PairResult = await pairing;
      if (!pairResult.ok) {
        result.error = MoonlightConnector.describe(pairResult.error);
        return result;
      }
    }
    result.ok = true;
    return result;
  }

  /**
   * The whole pre-flight for one connection. `width`/`height` are the agreed
   * stream size (the caller resolves follow-window / follow-screen to real
   * pixels). Returns the config the session backend feeds to LiStartConnection.
   */
  static async prepare(filesDir: string, host: string, port: number, stream: MoonlightProfile,
    appId: number, width: number, height: number,
    pinProvider: MoonlightPinProvider): Promise<MoonlightConnectResult> {
    const result: MoonlightConnectResult = new MoonlightConnectResult();
    const credential: MoonlightCredential = await MoonlightIdentity.ensureCredential(filesDir);
    if (!credential.ok) {
      result.error = MoonlightConnector.describe(credential.error);
      return result;
    }
    const serverInfo: ServerInfoResult = await MoonlightClient.fetchServerInfo(filesDir, host, port);
    if (!serverInfo.ok) {
      hilog.error(0xD001, 'HmRdpMoonlight', 'serverinfo failed: %{public}s', serverInfo.error);
      result.error = MoonlightConnector.describe(serverInfo.error);
      return result;
    }
    const info: NvServerInfo = serverInfo.info;

    // Sunshine's plain-HTTP serverinfo always reports PairStatus=0, so a stored
    // pin (from an earlier successful pairing) is what tells us we are paired
    // and can skip the PIN round.
    const paired: boolean = info.paired
      || MoonlightIdentity.loadPinnedServerCert(filesDir, host) !== undefined;
    if (!paired) {
      const pin: string = MoonlightConnector.generatePin();
      // The host only prompts for the PIN once our getservercert request is
      // pending, so start the handshake first and show the PIN while it waits.
      const pairing: Promise<PairResult> =
        MoonlightClient.pair(filesDir, host, port, info.httpsPort, info.majorVersion, pin);
      const proceed: boolean = await pinProvider(pin, pairing);
      if (!proceed) {
        // The handshake keeps running and times out on its own; nothing to join.
        result.error = '已取消配对';
        return result;
      }
      const pairResult: PairResult = await pairing;
      if (!pairResult.ok) {
        hilog.error(0xD001, 'HmRdpMoonlight', 'pair failed: %{public}s', pairResult.error);
        result.error = MoonlightConnector.describe(pairResult.error);
        return result;
      }
    }

    // No app chosen: pick one from the host's list (the app picker UI is a later
    // milestone; this gets a stream up). Prefer an app named "Desktop".
    let effectiveAppId: number = appId;
    if (effectiveAppId <= 0) {
      const apps: AppListResult = await MoonlightClient.fetchAppList(filesDir, host, info.httpsPort);
      if (!apps.ok || apps.apps.length === 0) {
        hilog.error(0xD001, 'HmRdpMoonlight', 'applist failed: %{public}s', apps.error);
        result.error = '主机没有可启动的应用';
        return result;
      }
      effectiveAppId = MoonlightConnector.pickApp(apps.apps);
      hilog.info(0xD001, 'HmRdpMoonlight', 'auto app id=%{public}d', effectiveAppId);
    }

    const rikeyHex: string = MoonlightCrypto.randomHex(16);
    const rikeyId: number = MoonlightConnector.randomKeyId();
    const options: MoonlightLaunchOptions = new MoonlightLaunchOptions();
    options.appId = effectiveAppId;
    options.width = width;
    options.height = height;
    options.fps = stream.fps;
    options.bitrateKbps = stream.bitrateKbps;
    options.audioConfig = stream.audioConfig;
    options.hdr = stream.hdr;
    options.yuv444 = stream.yuv444;
    options.gamepad = stream.gamepad;
    options.rikeyHex = rikeyHex;
    options.rikeyId = rikeyId;
    let launch: LaunchResult =
      await MoonlightClient.launch(filesDir, host, info.httpsPort, options);
    if (!launch.ok && launch.error === 'busy') {
      // The host is already streaming something: stop it and try once more, so a
      // stale session never blocks a new connect (no manual host action).
      hilog.warn(0xD001, 'HmRdpMoonlight', 'host busy; cancelling and retrying launch');
      await MoonlightClient.cancel(filesDir, host, info.httpsPort);
      launch = await MoonlightClient.launch(filesDir, host, info.httpsPort, options);
    }
    if (!launch.ok) {
      hilog.error(0xD001, 'HmRdpMoonlight', 'launch failed: %{public}s', launch.error);
      result.error = MoonlightConnector.describe(launch.error);
      return result;
    }

    const config: MoonlightStreamConfig = new MoonlightStreamConfig();
    config.host = host;
    config.serverInfoAppVersion = info.appVersion;
    config.serverInfoGfeVersion = info.gfeVersion;
    config.sessionUrl = launch.sessionUrl;
    config.serverCodecModeSupport = info.serverCodecModeSupport;
    config.width = width;
    config.height = height;
    config.fps = stream.fps;
    config.bitrateKbps = stream.bitrateKbps;
    config.packetSize = 1024;
    config.streamingRemotely = false;
    config.audioConfiguration = MoonlightConnector.audioConfiguration(stream.audioConfig);
    config.supportedVideoFormats = moonlightVideoFormats(stream.codec);
    config.clientRefreshRateX100 = 0;
    config.colorSpace = 0;
    config.colorRange = 0;
    config.encryptionFlags = 0;
    config.remoteInputAesKeyHex = rikeyHex;
    config.remoteInputAesIvHex = MoonlightConnector.ivHex(rikeyId);
    result.config = config;
    result.httpsPort = info.httpsPort;
    result.ok = true;
    return result;
  }

  /** 4-digit pairing PIN, shown to the user to type on the host. */
  static generatePin(): string {
    const hex: string = MoonlightCrypto.randomHex(2);
    const value: number = Number.parseInt(hex, 16);
    const base: number = Number.isNaN(value) ? 0 : value;
    return (1000 + (base % 9000)).toString();
  }

  /** First app, preferring one named "Desktop" (Sunshine lists the desktop as an app).
   *  Also the effective default when a connection has no explicit default set. */
  static pickApp(apps: NvApp[]): number {
    for (let i = 0; i < apps.length; i++) {
      if (apps[i].name.toLowerCase() === 'desktop') {
        return apps[i].id;
      }
    }
    return apps[0].id;
  }

  private static randomKeyId(): number {
    const hex: string = MoonlightCrypto.randomHex(4);
    const value: number = Number.parseInt(hex, 16);
    const base: number = Number.isNaN(value) ? 0 : value;
    return base & 0x7FFFFFFF;
  }

  /** remoteInputAesIv: the 32-bit rikeyid little-endian, rest zero. */
  private static ivHex(rikeyId: number): string {
    const iv: Uint8Array = new Uint8Array(16);
    iv[0] = rikeyId & 0xff;
    iv[1] = (rikeyId >> 8) & 0xff;
    iv[2] = (rikeyId >> 16) & 0xff;
    iv[3] = (rikeyId >> 24) & 0xff;
    return MoonlightCrypto.toHex(iv);
  }

  /** ML_AUDIO_* -> Moonlight AUDIO_CONFIGURATION_* (MAKE_AUDIO_CONFIGURATION). */
  private static audioConfiguration(audioConfig: number): number {
    if (audioConfig === ML_AUDIO_51) {
      return 0x3F06CA;
    }
    if (audioConfig === ML_AUDIO_71) {
      return 0x63F08CA;
    }
    if (audioConfig === ML_AUDIO_STEREO) {
      return 0x302CA;
    }
    return 0x302CA;
  }

  /** Turns a stable control-layer token into a user-facing reason. */
  private static describe(token: string): string {
    if (token === 'resolve' || token === 'connect' || token === 'timeout' || token === 'tls'
      || token === 'io' || token === 'request' || token === 'http' || token === 'status') {
      return '无法连接到主机，请检查地址与网络';
    }
    if (token === 'pin') {
      return 'PIN 不正确，请在主机上重新输入后重试';
    }
    if (token === 'already') {
      return '主机正在与另一台设备配对，请稍后重试';
    }
    if (token === 'mitm') {
      return '主机证书校验失败，已中止（可能存在中间人）';
    }
    if (token === 'failed' || token === 'crypto') {
      return '配对失败，请重试';
    }
    if (token === 'unpaired') {
      return '主机尚未配对';
    }
    if (token === 'busy') {
      return '主机正在串流其他应用，请稍后重试';
    }
    if (token === 'dir' || token === 'huks' || token === 'corrupt' || token === 'write'
      || token === 'missing' || token === 'no-cred') {
      return '本地客户端身份不可用，无法配对';
    }
    return token.length > 0 ? token : 'Moonlight 连接失败';
  }
}
