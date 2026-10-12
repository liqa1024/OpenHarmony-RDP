import { deviceInfo } from '@kit.BasicServicesKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { moonlightLaunchQueryParameters } from 'libhmrdp.so';
import { MoonlightCrypto } from './MoonlightCrypto';
import { MoonlightCredential, MoonlightIdentity } from './MoonlightIdentity';
import { MoonlightHttp, MoonlightHttpOptions, MoonlightHttpResult } from './MoonlightHttp';
import { MoonlightXml } from './MoonlightXml';
import {
  AppListResult, LaunchResult, MoonlightLaunchOptions, NvServerInfo, PairResult,
  ServerInfoResult
} from './MoonlightModels';

const DEFAULT_HTTPS_PORT: number = 47984;
// Long timeout for getservercert: the host user must type the PIN first.
const PAIR_CERT_TIMEOUT_MS: number = 300000;

class HttpOutcome {
  body: string = '';
  error: string = '';
}

export class MoonlightClient {
  // Shared with other Moonlight clients so games started elsewhere can be quit.
  static readonly UNIQUE_ID: string = '0123456789ABCDEF';

  static async fetchServerInfo(dir: string, host: string, port: number,
    timeoutMs?: number): Promise<ServerInfoResult> {
    const result: ServerInfoResult = new ServerInfoResult();
    const path: string =
      `/serverinfo?uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const outcome: HttpOutcome =
      MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, port, path, false,
        undefined, timeoutMs));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    const body: string = outcome.body;
    const info: NvServerInfo = new NvServerInfo();
    info.hostname = MoonlightXml.readOptional(body, 'hostname') ?? '';
    info.uniqueId = MoonlightXml.readOptional(body, 'uniqueid') ?? '';
    info.httpsPort = MoonlightXml.readNumber(body, 'HttpsPort', DEFAULT_HTTPS_PORT);
    info.externalPort = MoonlightXml.readNumber(body, 'ExternalPort', port);
    info.mac = MoonlightXml.readOptional(body, 'mac') ?? '';
    info.state = MoonlightXml.readOptional(body, 'state') ?? '';
    info.gpuType = MoonlightXml.readOptional(body, 'gputype') ?? '';
    info.gfeVersion = MoonlightXml.readOptional(body, 'GfeVersion') ?? '';
    info.paired = (MoonlightXml.readOptional(body, 'PairStatus') ?? '0') === '1';
    info.appVersion = MoonlightXml.readOptional(body, 'appversion') ?? '';
    info.majorVersion = MoonlightClient.majorVersion(info.appVersion);
    info.serverCodecModeSupport = MoonlightXml.readNumber(body, 'ServerCodecModeSupport', 0);
    if (info.state.endsWith('_SERVER_BUSY')) {
      info.currentGame = MoonlightXml.readNumber(body, 'currentgame', 0);
    }
    result.info = info;
    result.ok = true;
    return result;
  }

  static async fetchAppList(dir: string, host: string, httpsPort: number): Promise<AppListResult> {
    const result: AppListResult = new AppListResult();
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined) {
      result.error = 'unpaired';
      return result;
    }
    const path: string =
      `/applist?uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const outcome: HttpOutcome =
      MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, httpsPort, path, true, pin));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    result.apps = MoonlightXml.parseAppList(outcome.body);
    result.ok = true;
    return result;
  }

  static async fetchAppAsset(dir: string, host: string, httpsPort: number,
    appId: number): Promise<ArrayBuffer | undefined> {
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined) {
      return undefined;
    }
    // AssetType=2 / AssetIdx=0 select the box art; the host ignores an unknown
    // clientid, so it is not sent.
    const path: string = `/appasset?appid=${appId}&AssetType=2&AssetIdx=0`
      + `&uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const response: MoonlightHttpResult =
      await MoonlightClient.httpGet(dir, host, httpsPort, path, true, pin, 10000, true);
    return response.ok ? response.bodyBytes : undefined;
  }

  static async launch(dir: string, host: string, httpsPort: number,
    options: MoonlightLaunchOptions): Promise<LaunchResult> {
    return MoonlightClient.startSession(dir, host, httpsPort, options, false);
  }

  static async resume(dir: string, host: string, httpsPort: number,
    options: MoonlightLaunchOptions): Promise<LaunchResult> {
    return MoonlightClient.startSession(dir, host, httpsPort, options, true);
  }

  private static async startSession(dir: string, host: string, httpsPort: number,
    options: MoonlightLaunchOptions, resume: boolean): Promise<LaunchResult> {
    const result: LaunchResult = new LaunchResult();
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined) {
      result.error = 'unpaired';
      return result;
    }
    const verb: string = resume ? 'resume' : 'launch';
    const path: string = `/${verb}?uniqueid=${MoonlightClient.UNIQUE_ID}`
      + `&uuid=${MoonlightClient.uuid()}`
      + `&clientname=${MoonlightClient.clientName()}`
      + `&appid=${options.appId}`
      + `&mode=${options.width}x${options.height}x${options.fps}`
      + `&rikey=${options.rikeyHex}`
      + `&rikeyid=${options.rikeyId}`
      // Sunshine nvhttp parameter names: localAudioPlayMode (not localaudio),
      // surroundAudioInfo (packed int). Audio goes to the client (0) when client
      // playback is on, else the host plays locally (1). Gamepad not wired yet:
      // controller mask 0.
      + `&localAudioPlayMode=${options.audioEnabled ? 0 : 1}`
      + '&surroundAudioInfo=196610'
      + '&remoteControllersBitmap=0'
      + '&gcmap=0'
      + moonlightLaunchQueryParameters();
    const outcome: HttpOutcome =
      MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, httpsPort, path, true, pin));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    const sessionUrl: string = MoonlightXml.readOptional(outcome.body, 'sessionUrl0')
      ?? MoonlightXml.readOptional(outcome.body, 'sessionUrl') ?? '';
    if (sessionUrl.length === 0) {
      result.error = 'status';
      return result;
    }
    result.sessionUrl = sessionUrl;
    result.rikeyHex = options.rikeyHex;
    result.rikeyId = options.rikeyId;
    result.ok = true;
    return result;
  }

  static async pair(dir: string, host: string, httpPort: number, httpsPort: number,
    majorVersion: number, pin: string): Promise<PairResult> {
    const result: PairResult = new PairResult();
    const credential: MoonlightCredential = await MoonlightIdentity.ensureCredential(dir);
    if (!credential.ok) {
      result.error = credential.error.length > 0 ? credential.error : 'no-cred';
      return result;
    }
    const clientCertDer: Uint8Array | undefined = MoonlightCrypto.pemToDer(credential.certPem);
    if (clientCertDer === undefined) {
      result.error = 'crypto';
      return result;
    }
    const clientCertSig: string | undefined =
      MoonlightCrypto.certSignatureToHex(clientCertDer);
    if (clientCertSig === undefined) {
      result.error = 'crypto';
      return result;
    }
    // The host parses `clientcert` as PEM (Sunshine crypto::x509), so it is the
    // hex of the PEM text, not of the DER.
    const clientCertHex: string = MoonlightCrypto.toHex(MoonlightCrypto.utf8(credential.certPem));
    // One uniqueid per pairing: Sunshine keys its pending session by it and
    // answers a second getservercert with the same id 409 (no prompt).
    const uniqueId: string = MoonlightCrypto.randomHex(16);
    const uuid: string = MoonlightCrypto.randomHex(16);

    const salt: string = MoonlightCrypto.randomHex(16);
    // The AES key is SHA-256(salt || pin) (Sunshine / GFE 7+), never SHA-1.
    const aesKey: string | undefined = MoonlightClient.deriveKey(salt, pin);
    if (aesKey === undefined) {
      result.error = 'crypto';
      return result;
    }

    const getCertPath: string = MoonlightClient.pairPath(
      `phrase=getservercert&salt=${salt}&clientcert=${clientCertHex}`, uniqueId, uuid);
    let outcome: HttpOutcome = MoonlightClient.bodyOrError(
      await MoonlightClient.httpGet(dir, host, httpPort, getCertPath, false, undefined,
        PAIR_CERT_TIMEOUT_MS));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }
    const serverCertHex: string = MoonlightXml.read(outcome.body, 'plaincert');
    const serverCertBytes: Uint8Array | undefined = MoonlightCrypto.fromHex(serverCertHex);
    if (serverCertBytes === undefined || serverCertBytes.length === 0) {
      result.error = 'already';
      return result;
    }
    // plaincert is hex of the server certificate PEM; the crypto works on DER.
    const serverCertDer: Uint8Array | undefined =
      MoonlightCrypto.pemToDer(MoonlightCrypto.text(serverCertBytes));
    if (serverCertDer === undefined) {
      result.error = 'crypto';
      return result;
    }
    const serverCertSig: string | undefined =
      MoonlightCrypto.certSignatureToHex(serverCertDer);
    if (serverCertSig === undefined) {
      result.error = 'crypto';
      return result;
    }

    const randomChallenge: string = MoonlightCrypto.randomHex(16);
    const encryptedChallenge: string | undefined = MoonlightClient.aesEncryptHex(aesKey, randomChallenge);
    if (encryptedChallenge === undefined) {
      result.error = 'crypto';
      return result;
    }
    outcome = MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, httpPort,
      MoonlightClient.pairPath(`clientchallenge=${encryptedChallenge}`, uniqueId, uuid), false));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }
    const encryptedResponse: string = MoonlightXml.read(outcome.body, 'challengeresponse');
    const decryptedResponse: string | undefined = MoonlightClient.aesDecryptHex(aesKey, encryptedResponse);
    // SHA-256 digest (32 bytes) then the 16-byte server challenge.
    const hashLenHex: number = 64;
    if (decryptedResponse === undefined || decryptedResponse.length < hashLenHex + 32) {
      result.error = 'crypto';
      return result;
    }
    const serverResponse: string = decryptedResponse.substring(0, hashLenHex);
    const serverChallenge: string = decryptedResponse.substring(hashLenHex, hashLenHex + 32);

    const clientSecret: string = MoonlightCrypto.randomHex(16);
    const challengeResponse: string | undefined = MoonlightClient.challengeResponse(
      aesKey, serverChallenge, clientCertSig, clientSecret);
    if (challengeResponse === undefined) {
      result.error = 'crypto';
      return result;
    }
    outcome = MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, httpPort,
      MoonlightClient.pairPath(`serverchallengeresp=${challengeResponse}`, uniqueId, uuid), false));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }
    const pairingSecret: string = MoonlightXml.read(outcome.body, 'pairingsecret');
    if (pairingSecret.length < 34) {
      result.error = 'failed';
      return result;
    }
    const serverSecret: string = pairingSecret.substring(0, 32);
    const serverSignature: string = pairingSecret.substring(32);
    const verified: boolean = await MoonlightClient.verifyServerSecret(serverCertDer,
      serverSecret, serverSignature);
    if (!verified) {
      result.error = 'mitm';
      return result;
    }
    const expectedResponse: string | undefined = MoonlightClient.expectedResponse(
      randomChallenge, serverCertSig, serverSecret);
    if (expectedResponse !== serverResponse) {
      result.error = 'pin';
      return result;
    }

    const clientSignature: string | undefined =
      MoonlightClient.signClientSecret(credential, clientSecret);
    if (clientSignature === undefined) {
      result.error = 'crypto';
      return result;
    }
    const clientPairingSecret: string = clientSecret + clientSignature;
    outcome = MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, httpPort,
      MoonlightClient.pairPath(`clientpairingsecret=${clientPairingSecret}`, uniqueId, uuid), false));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }

    outcome = MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, httpsPort,
      MoonlightClient.pairPath('phrase=pairchallenge', uniqueId, uuid), true, serverCertDer));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }

    MoonlightIdentity.storePinnedServerCert(dir, host, serverCertDer);
    result.ok = true;
    return result;
  }

  static async unpair(dir: string, host: string, port: number): Promise<boolean> {
    const path: string =
      `/unpair?uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const response: MoonlightHttpResult = await MoonlightClient.httpGet(dir, host, port, path, false);
    MoonlightIdentity.clearPinnedServerCert(dir, host);
    return response.ok;
  }

  static async cancel(dir: string, host: string, httpsPort: number): Promise<boolean> {
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined) {
      return false;
    }
    const path: string =
      `/cancel?uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const response: MoonlightHttpResult =
      await MoonlightClient.httpGet(dir, host, httpsPort, path, true, pin);
    return response.ok;
  }

  private static majorVersion(appVersion: string): number {
    const parts: string[] = appVersion.split('.');
    if (parts.length === 0) {
      return 0;
    }
    const value: number = Number.parseInt(parts[0], 10);
    return Number.isNaN(value) ? 0 : value;
  }

  private static pairPath(arguments_: string, uniqueId: string, uuid: string): string {
    return `/pair?devicename=roth&updateState=1&clientname=${MoonlightClient.clientName()}`
      + `&${arguments_}&uniqueid=${uniqueId}&uuid=${uuid}`;
  }

  private static clientName(): string {
    let name: string = deviceInfo.marketName;
    if (name.length === 0) {
      name = deviceInfo.productModel;
    }
    if (name.length === 0) {
      // Fallback: the value other clients send as `devicename`.
      name = 'roth';
    }
    return encodeURIComponent(name);
  }

  private static uuid(): string {
    return MoonlightCrypto.randomHex(16);
  }

  private static async httpGet(dir: string, host: string, port: number, path: string,
    tls: boolean, pinDer?: Uint8Array, timeoutMs?: number,
    binary?: boolean): Promise<MoonlightHttpResult> {
    const options: MoonlightHttpOptions = new MoonlightHttpOptions();
    options.useTls = tls;
    options.host = host;
    options.port = port;
    options.path = path;
    options.method = 'GET';
    options.pinnedServerCertDer = pinDer ?? new Uint8Array(0);
    options.timeoutMs = timeoutMs ?? 10000;
    options.binary = binary ?? false;
    if (!tls) {
      return MoonlightHttp.perform(options);
    }
    return MoonlightClient.performWithCredential(dir, options);
  }

  // HTTPS with the paired identity (mTLS) + pinned cert; the key PEM lives only for the request.
  private static async performWithCredential(dir: string,
    options: MoonlightHttpOptions): Promise<MoonlightHttpResult> {
    const credential: MoonlightCredential = await MoonlightIdentity.ensureCredential(dir);
    if (!credential.ok) {
      const failed: MoonlightHttpResult = new MoonlightHttpResult();
      failed.error = credential.error.length > 0 ? credential.error : 'no-cred';
      return failed;
    }
    const keyPath: string | undefined = MoonlightIdentity.writeClientKeyPem(dir, credential);
    if (keyPath === undefined) {
      const failed: MoonlightHttpResult = new MoonlightHttpResult();
      failed.error = 'tls';
      return failed;
    }
    options.certPath = MoonlightIdentity.clientCertPath(dir);
    options.keyPath = keyPath;
    try {
      return await MoonlightHttp.perform(options);
    } finally {
      MoonlightIdentity.removeClientKeyPem(dir);
    }
  }

  static async getClipboardBlob(dir: string, host: string, httpsPort: number,
    id: string): Promise<ArrayBuffer | undefined> {
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined || id.length === 0) {
      return undefined;
    }
    const path: string = `/api/v1/clipboard/blob/${id}`;
    const response: MoonlightHttpResult =
      await MoonlightClient.httpGet(dir, host, httpsPort, path, true, pin, 30000, true);
    if (!response.ok || response.status !== 200 || response.bodyBytes === undefined) {
      return undefined;
    }
    return response.bodyBytes;
  }

  static async postClipboardBlob(dir: string, host: string, httpsPort: number,
    bytes: ArrayBuffer, mime: string): Promise<string | undefined> {
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined) {
      return undefined;
    }
    const options: MoonlightHttpOptions = new MoonlightHttpOptions();
    options.useTls = true;
    options.host = host;
    options.port = httpsPort;
    options.path = '/api/v1/clipboard/blob';
    options.method = 'POST';
    options.contentType = 'application/octet-stream';
    options.xClipboardMime = mime;
    options.bodyBytes = bytes;
    options.pinnedServerCertDer = pin;
    options.timeoutMs = 30000;
    const response: MoonlightHttpResult =
      await MoonlightClient.performWithCredential(dir, options);
    if (!response.ok || response.status !== 200) {
      return undefined;
    }
    return MoonlightClient.readJsonId(response.body);
  }

  // Avoids a generic JSON parse (ArkTS has no untyped object access).
  private static readJsonId(body: string): string | undefined {
    const key: string = '"id"';
    const at: number = body.indexOf(key);
    if (at < 0) {
      return undefined;
    }
    const colon: number = body.indexOf(':', at + key.length);
    if (colon < 0) {
      return undefined;
    }
    const open: number = body.indexOf('"', colon + 1);
    if (open < 0) {
      return undefined;
    }
    const close: number = body.indexOf('"', open + 1);
    if (close < 0) {
      return undefined;
    }
    const id: string = body.substring(open + 1, close);
    return id.length > 0 ? id : undefined;
  }

  // A host 4xx/5xx is reported by the body's <root status_code>, so only
  // transport failures and non-200 root statuses fail here.
  private static bodyOrError(response: MoonlightHttpResult): HttpOutcome {
    const outcome: HttpOutcome = new HttpOutcome();
    if (!response.ok) {
      outcome.error = response.error.length > 0 ? response.error : 'request';
      return outcome;
    }
    const body: string = response.body;
    if (body.length === 0) {
      outcome.error = 'http';
      return outcome;
    }
    const code: number = MoonlightXml.statusCode(body);
    if (code !== 200) {
      hilog.warn(0xD001, 'HmRdpMoonlight', 'root status=%{public}d body=%{public}s',
        code, body.substring(0, 240));
      outcome.error = body.indexOf('already running') >= 0 ? 'busy' : 'status';
      return outcome;
    }
    outcome.body = body;
    return outcome;
  }

  private static deriveKey(saltHex: string, pin: string): string | undefined {
    const salt: Uint8Array | undefined = MoonlightCrypto.fromHex(saltHex);
    if (salt === undefined) {
      return undefined;
    }
    const pinBytes: Uint8Array = MoonlightCrypto.utf8(pin);
    const material: Uint8Array = new Uint8Array(salt.length + pinBytes.length);
    material.set(salt, 0);
    material.set(pinBytes, salt.length);
    const digest: Uint8Array | undefined = MoonlightCrypto.hash(true, material);
    if (digest === undefined || digest.length < 16) {
      return undefined;
    }
    return MoonlightCrypto.toHex(digest.slice(0, 16));
  }

  private static aesEncryptHex(keyHex: string, dataHex: string): string | undefined {
    const key: Uint8Array | undefined = MoonlightCrypto.fromHex(keyHex);
    const data: Uint8Array | undefined = MoonlightCrypto.fromHex(dataHex);
    if (key === undefined || data === undefined) {
      return undefined;
    }
    const out: Uint8Array | undefined = MoonlightCrypto.aesEcb(true, key, data);
    return out === undefined ? undefined : MoonlightCrypto.toHex(out);
  }

  private static aesDecryptHex(keyHex: string, dataHex: string): string | undefined {
    const key: Uint8Array | undefined = MoonlightCrypto.fromHex(keyHex);
    const data: Uint8Array | undefined = MoonlightCrypto.fromHex(dataHex);
    if (key === undefined || data === undefined) {
      return undefined;
    }
    const out: Uint8Array | undefined = MoonlightCrypto.aesEcb(false, key, data);
    return out === undefined ? undefined : MoonlightCrypto.toHex(out);
  }

  private static challengeResponse(keyHex: string,
    serverChallengeHex: string, clientCertSignatureHex: string,
    clientSecretHex: string): string | undefined {
    const key: Uint8Array | undefined = MoonlightCrypto.fromHex(keyHex);
    const challenge: Uint8Array | undefined = MoonlightCrypto.fromHex(serverChallengeHex);
    const signature: Uint8Array | undefined = MoonlightCrypto.fromHex(clientCertSignatureHex);
    const secret: Uint8Array | undefined = MoonlightCrypto.fromHex(clientSecretHex);
    if (key === undefined || challenge === undefined || signature === undefined
      || secret === undefined) {
      return undefined;
    }
    const digest: Uint8Array | undefined = MoonlightCrypto.hash(true,
      MoonlightClient.concat([challenge, signature, secret]));
    if (digest === undefined) {
      return undefined;
    }
    const padded: Uint8Array = new Uint8Array(32);
    padded.set(digest.slice(0, Math.min(digest.length, 32)), 0);
    const encrypted: Uint8Array | undefined = MoonlightCrypto.aesEcb(true, key, padded);
    return encrypted === undefined ? undefined : MoonlightCrypto.toHex(encrypted);
  }

  private static expectedResponse(randomChallengeHex: string,
    serverCertSignatureHex: string, serverSecretHex: string): string | undefined {
    const challenge: Uint8Array | undefined = MoonlightCrypto.fromHex(randomChallengeHex);
    const signature: Uint8Array | undefined = MoonlightCrypto.fromHex(serverCertSignatureHex);
    const secret: Uint8Array | undefined = MoonlightCrypto.fromHex(serverSecretHex);
    if (challenge === undefined || signature === undefined || secret === undefined) {
      return undefined;
    }
    const digest: Uint8Array | undefined = MoonlightCrypto.hash(true,
      MoonlightClient.concat([challenge, signature, secret]));
    return digest === undefined ? undefined : MoonlightCrypto.toHex(digest);
  }

  private static signClientSecret(credential: MoonlightCredential,
    clientSecretHex: string): string | undefined {
    const secret: Uint8Array | undefined = MoonlightCrypto.fromHex(clientSecretHex);
    if (secret === undefined) {
      return undefined;
    }
    const signature: Uint8Array | undefined =
      MoonlightCrypto.signSha256(credential.keyPkcs8Der, secret);
    return signature === undefined ? undefined : MoonlightCrypto.toHex(signature);
  }

  private static async verifyServerSecret(serverCertDer: Uint8Array, serverSecretHex: string,
    serverSignatureHex: string): Promise<boolean> {
    const secret: Uint8Array | undefined = MoonlightCrypto.fromHex(serverSecretHex);
    const signature: Uint8Array | undefined = MoonlightCrypto.fromHex(serverSignatureHex);
    if (secret === undefined || signature === undefined) {
      return false;
    }
    const publicKey: cryptoFramework.PubKey | undefined =
      await MoonlightIdentity.serverPublicKey(serverCertDer);
    if (publicKey === undefined) {
      hilog.error(0xD001, 'HmRdpMoonlight', 'server public key unavailable (cert parse failed)');
      return false;
    }
    const ok: boolean = MoonlightCrypto.verifySha256(publicKey, secret, signature);
    hilog.info(0xD001, 'HmRdpMoonlight', 'server secret verify: %{public}s', ok ? 'ok' : 'false');
    return ok;
  }

  private static concat(parts: Uint8Array[]): Uint8Array {
    let length: number = 0;
    for (let i = 0; i < parts.length; i++) {
      length += parts[i].length;
    }
    const out: Uint8Array = new Uint8Array(length);
    let offset: number = 0;
    for (let i = 0; i < parts.length; i++) {
      out.set(parts[i], offset);
      offset += parts[i].length;
    }
    return out;
  }
}
