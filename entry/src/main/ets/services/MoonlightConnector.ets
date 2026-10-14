import { moonlightVideoFormats } from 'libhmrdp.so';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ML_COLOR_RANGE_LIMITED, ML_SCALE_MANUAL, ML_SCALE_NONE, ML_STREAM_CFG_AUTO,
  MoonlightProfile, MoonlightStreamConfig } from '../model/Connections';
import { SettingsStore } from './SettingsStore';
import { recommendedBitrateKbps } from '../utils/MoonlightQuality';
import { moonlightHostCodecNames } from '../utils/MoonlightCodec';
import { MoonlightClient, MoonlightDisplay } from '../moonlight/MoonlightClient';
import { MoonlightCrypto } from '../moonlight/MoonlightCrypto';
import { MoonlightIdentity, MoonlightCredential } from '../moonlight/MoonlightIdentity';
import {
  AppListResult, LaunchResult, MoonlightLaunchOptions, NvApp, NvServerInfo, PairResult,
  ServerInfoResult
} from '../moonlight/MoonlightModels';

export type MoonlightPinProvider = (pin: string, pairing: Promise<PairResult>) => Promise<boolean>;

export class MoonlightConnectResult {
  ok: boolean = false;
  error: string = '';
  config: MoonlightStreamConfig = new MoonlightStreamConfig();
  httpsPort: number = 47984;
  hostId: string = '';
  effectiveAppId: number = 0;
}

export class MoonlightAppsResult {
  ok: boolean = false;
  error: string = '';
  apps: NvApp[] = [];
  currentGame: number = 0;
  httpsPort: number = 47984;
  hostId: string = '';
}

export class MoonlightPairResult {
  ok: boolean = false;
  error: string = '';
  hostId: string = '';
}

export class MoonlightKnownHost {
  hostId: string = '';
  host: string = '';
}

export class MoonlightHostPolicy {
  known: MoonlightKnownHost[] = [];
  expectedId: string = '';
}

export class MoonlightConnector {
  static async probeApps(filesDir: string, host: string, port: number,
    pinProvider: MoonlightPinProvider, expectedHostId: string): Promise<MoonlightAppsResult> {
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
    result.hostId = info.uniqueId;
    MoonlightConnector.invalidateStalePin(filesDir, host, expectedHostId, info.uniqueId);
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

  static async pairOnly(filesDir: string, host: string, port: number,
    pinProvider: MoonlightPinProvider, policy: MoonlightHostPolicy): Promise<MoonlightPairResult> {
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
    result.hostId = info.uniqueId;
    const policyError: string =
      MoonlightConnector.applyHostPolicy(filesDir, host, info.uniqueId, policy);
    if (policyError.length > 0) {
      result.error = policyError;
      return result;
    }
    const paired: boolean = info.paired
      || MoonlightIdentity.loadPinnedServerCert(filesDir, host) !== undefined;
    if (!paired) {
      const pin: string = MoonlightConnector.generatePin();
      // Start the handshake first: the host only prompts for the PIN once our
      // getservercert request is pending.
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

  static forgetHost(filesDir: string, host: string, port: number): void {
    const target: string = host.trim();
    if (target.length === 0) {
      return;
    }
    MoonlightIdentity.clearPinnedServerCert(filesDir, target);
    MoonlightClient.unpair(filesDir, target, port)
      .catch((error: BusinessError): void => {
        hilog.warn(0xD001, 'HmRdpMoonlight', 'unpair %{public}s failed: %{public}s',
          target, JSON.stringify(error));
      });
  }

  private static applyHostPolicy(filesDir: string, host: string, hostId: string,
    policy: MoonlightHostPolicy): string {
    if (hostId.length > 0) {
      for (let i = 0; i < policy.known.length; i++) {
        if (policy.known[i].hostId === hostId) {
          return `主机 ${policy.known[i].host} 已存在另一条 Moonlight 连接，请先删除该连接`;
        }
      }
    }
    MoonlightConnector.invalidateStalePin(filesDir, host, policy.expectedId, hostId);
    return '';
  }

  private static invalidateStalePin(filesDir: string, host: string, expectedId: string,
    hostId: string): void {
    // A stale pin would make the next mTLS request fail instead of re-pairing.
    if (expectedId.length > 0 && hostId.length > 0 && expectedId !== hostId) {
      hilog.info(0xD001, 'HmRdpMoonlight', 'host %{public}s identity changed; dropping stale pin',
        host);
      MoonlightIdentity.clearPinnedServerCert(filesDir, host);
    }
  }

  static async prepare(filesDir: string, host: string, port: number, stream: MoonlightProfile,
    appId: number, width: number, height: number,
    pinProvider: MoonlightPinProvider, policy: MoonlightHostPolicy,
    audioEnabled: boolean): Promise<MoonlightConnectResult> {
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
    result.hostId = info.uniqueId;
    const policyError: string =
      MoonlightConnector.applyHostPolicy(filesDir, host, info.uniqueId, policy);
    if (policyError.length > 0) {
      hilog.warn(0xD001, 'HmRdpMoonlight', 'host policy rejected %{public}s', host);
      result.error = policyError;
      return result;
    }

    // No shared codec: fail here, not via the library's silent H.264 fallback.
    const videoFormats: number = moonlightVideoFormats(stream.codec, info.serverCodecModeSupport);
    if (videoFormats === 0) {
      hilog.error(0xD001, 'HmRdpMoonlight', 'no shared video codec with %{public}s', host);
      result.error = '本机与主机没有共同支持的视频编码（主机支持 '
        + moonlightHostCodecNames(info.serverCodecModeSupport) + '）';
      return result;
    }

    // Sunshine's plain-HTTP serverinfo always reports PairStatus=0; a stored pin
    // from an earlier pairing is what proves we are paired.
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
    // The stream resolution is final here (follow-screen already resolved), so the
    // auto bitrate is computed against the size that will actually be sent.
    const effectiveBitrateKbps: number = stream.bitrateAuto
      ? recommendedBitrateKbps(width, height, stream.fps, stream.codec)
      : stream.bitrateKbps;
    options.appId = effectiveAppId;
    options.width = width;
    options.height = height;
    options.fps = stream.fps;
    options.bitrateKbps = effectiveBitrateKbps;
    options.rikeyHex = rikeyHex;
    options.rikeyId = rikeyId;
    // Host-side local playback mirrors client playback: with client audio on the
    // host stays silent and streams; off it plays locally.
    options.audioEnabled = audioEnabled;
    let launch: LaunchResult;
    if (info.currentGame === effectiveAppId) {
      launch = await MoonlightClient.resume(filesDir, host, info.httpsPort, options);
    } else {
      if (info.currentGame !== 0) {
        await MoonlightClient.cancel(filesDir, host, info.httpsPort);
      }
      launch = await MoonlightClient.launch(filesDir, host, info.httpsPort, options);
      if (!launch.ok && launch.error === 'busy') {
        // The host started something between the check and the launch; cancel and retry once.
        hilog.warn(0xD001, 'HmRdpMoonlight', 'host busy; cancelling and retrying launch');
        await MoonlightClient.cancel(filesDir, host, info.httpsPort);
        launch = await MoonlightClient.launch(filesDir, host, info.httpsPort, options);
      }
    }
    if (!launch.ok) {
      hilog.error(0xD001, 'HmRdpMoonlight', 'launch failed: %{public}s', launch.error);
      result.error = MoonlightConnector.describe(launch.error);
      return result;
    }

    await MoonlightConnector.applyVirtualDisplayScale(filesDir, host, info.httpsPort, stream,
      width, height);

    const config: MoonlightStreamConfig = new MoonlightStreamConfig();
    config.host = host;
    config.serverInfoAppVersion = info.appVersion;
    config.serverInfoGfeVersion = info.gfeVersion;
    config.sessionUrl = launch.sessionUrl;
    config.serverCodecModeSupport = info.serverCodecModeSupport;
    config.width = width;
    config.height = height;
    config.fps = stream.fps;
    config.bitrateKbps = effectiveBitrateKbps;
    config.packetSize = 1024;
    // Let the client library decide local vs remote (RFC1918 heuristic) instead of
    // forcing LOCAL; it drives bitrate/audio/QoS advertisement and packet size.
    config.streamingRemotely = ML_STREAM_CFG_AUTO;
    config.audioConfiguration = MoonlightConnector.AUDIO_CONFIGURATION_STEREO;
    config.supportedVideoFormats = videoFormats;
    config.clientRefreshRateX100 = 0;
    config.colorSpace = stream.colorSpace;
    config.colorRange = ML_COLOR_RANGE_LIMITED;
    config.encryptionFlags = 0;
    config.remoteInputAesKeyHex = rikeyHex;
    config.remoteInputAesIvHex = MoonlightConnector.ivHex(rikeyId);
    config.enableAudio = audioEnabled;
    result.config = config;
    result.httpsPort = info.httpsPort;
    result.effectiveAppId = effectiveAppId;
    result.ok = true;
    return result;
  }

  // foundation-sunshine only: write the requested scale onto the host virtual display, which
  // is created/released per session (so it needs no restore). Any other host is left alone.
  private static async applyVirtualDisplayScale(filesDir: string, host: string, httpsPort: number,
    stream: MoonlightProfile, width: number, height: number): Promise<void> {
    if (stream.scaleMode === ML_SCALE_NONE) {
      return;
    }
    const wanted: number = stream.scaleMode === ML_SCALE_MANUAL
      ? stream.scalePercent : SettingsStore.scalePresetForResolution(width, height);
    if (wanted <= 0) {
      return;
    }
    try {
      const display: MoonlightDisplay =
        await MoonlightClient.fetchVirtualDisplay(filesDir, host, httpsPort);
      if (!display.usable()) {
        hilog.info(0xD001, 'HmRdpMoonlight', 'display scale skipped: no usable virtual display');
        return;
      }
      const label: number =
        MoonlightClient.hostScaleLabel(wanted, display.supportedScalePercents);
      const applied: boolean = await MoonlightClient.setDisplayScale(filesDir, host, httpsPort,
        display.deviceId, display.displayName, label);
      hilog.info(0xD001, 'HmRdpMoonlight',
        'display scale wanted=%{public}d label=%{public}d applied=%{public}d',
        wanted, label, applied ? 1 : 0);
    } catch (error) {
      hilog.warn(0xD001, 'HmRdpMoonlight', 'display scale failed: %{public}s',
        JSON.stringify(error));
    }
  }

  static generatePin(): string {
    const hex: string = MoonlightCrypto.randomHex(2);
    const value: number = Number.parseInt(hex, 16);
    const base: number = Number.isNaN(value) ? 0 : value;
    return (1000 + (base % 9000)).toString();
  }

  // Sunshine lists the host desktop itself as an app named "Desktop".
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

  private static ivHex(rikeyId: number): string {
    const iv: Uint8Array = new Uint8Array(16);
    iv[0] = rikeyId & 0xff;
    iv[1] = (rikeyId >> 8) & 0xff;
    iv[2] = (rikeyId >> 16) & 0xff;
    iv[3] = (rikeyId >> 24) & 0xff;
    return MoonlightCrypto.toHex(iv);
  }

  // Moonlight stereo AUDIO_CONFIGURATION (MAKE_AUDIO_CONFIGURATION).
  private static readonly AUDIO_CONFIGURATION_STEREO: number = 0x302CA;

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
