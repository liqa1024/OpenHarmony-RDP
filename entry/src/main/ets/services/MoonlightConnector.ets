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
import {
  ML_AUDIO_51, ML_AUDIO_71, ML_AUDIO_STEREO, MoonlightProfile, MoonlightStreamConfig
} from '../model/RdpModels';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { MoonlightCrypto } from '../moonlight/MoonlightCrypto';
import { MoonlightIdentity, MoonlightCredential } from '../moonlight/MoonlightIdentity';
import {
  LaunchResult, MoonlightLaunchOptions, NvServerInfo, PairResult, ServerInfoResult
} from '../moonlight/MoonlightModels';

/** `pin` is the 4-digit code to display; resolve true once the user entered it. */
export type MoonlightPinProvider = (pin: string) => Promise<boolean>;

export class MoonlightConnectResult {
  ok: boolean = false;
  // User-facing reason when !ok.
  error: string = '';
  config: MoonlightStreamConfig = new MoonlightStreamConfig();
}

export class MoonlightConnector {
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
      result.error = MoonlightConnector.describe(serverInfo.error);
      return result;
    }
    const info: NvServerInfo = serverInfo.info;

    if (!info.paired) {
      const pin: string = MoonlightConnector.generatePin();
      const confirmed: boolean = await pinProvider(pin);
      if (!confirmed) {
        result.error = '已取消配对';
        return result;
      }
      const pairResult: PairResult =
        await MoonlightClient.pair(filesDir, host, port, info.httpsPort, info.majorVersion, pin);
      if (!pairResult.ok) {
        result.error = MoonlightConnector.describe(pairResult.error);
        return result;
      }
    }

    const rikeyHex: string = MoonlightCrypto.randomHex(16);
    const rikeyId: number = MoonlightConnector.randomKeyId();
    const options: MoonlightLaunchOptions = new MoonlightLaunchOptions();
    options.appId = appId;
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
    const launch: LaunchResult =
      await MoonlightClient.launch(filesDir, host, info.httpsPort, options);
    if (!launch.ok) {
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
    if (token === 'dir' || token === 'huks' || token === 'corrupt' || token === 'write'
      || token === 'missing' || token === 'no-cred') {
      return '本地客户端身份不可用，无法配对';
    }
    return token.length > 0 ? token : 'Moonlight 连接失败';
  }
}
