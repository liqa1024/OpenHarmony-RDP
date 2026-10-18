import { deviceInfo } from '@kit.BasicServicesKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { moonlightLaunchQueryParameters } from 'libhmrdp.so';
import { MoonlightCrypto } from './MoonlightCrypto';
import { MoonlightCredential, MoonlightIdentity } from './MoonlightIdentity';
import { MoonlightHttp, MoonlightHttpCancel, MoonlightHttpOptions, MoonlightHttpResult }
  from './MoonlightHttp';
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

// POST /display-scale body (foundation-sunshine display scaling).
class DisplayScaleRequest {
  scale_percent: number = 100;
  device_id: string = '';
  display_name: string = '';
}

// Foundation-sunshine virtual display as reported by GET /displays.
export class MoonlightDisplay {
  deviceId: string = '';
  displayName: string = '';
  currentScalePercent: number = 0;
  supportedScalePercents: number[] = [];
  scaleSupported: boolean = false;

  usable(): boolean {
    return (this.deviceId.length > 0 || this.displayName.length > 0) && this.scaleSupported;
  }
}

// A scale request split into what to send and what it really selects on the host.
export class MoonlightScaleTarget {
  // Value to put in the request (the host's own label for the step).
  label: number = 0;
  // True Windows percent that step applies.
  percent: number = 0;
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
      // Sunshine nvhttp names: localAudioPlayMode (0 = client plays, 1 = host
      // local), surroundAudioInfo packed; the gamepad mask is the set of controllers
      // attached here (hot-plug sends arrival events on the control stream).
      + `&localAudioPlayMode=${options.audioEnabled ? 0 : 1}`
      + '&surroundAudioInfo=196610'
      + `&remoteControllersBitmap=${options.gamepadMask}`
      + `&gcmap=${options.gamepadMask}`
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
    majorVersion: number, pin: string, cancel?: MoonlightHttpCancel): Promise<PairResult> {
    const result: PairResult = new PairResult();
    if (cancel !== undefined && cancel.isCanceled()) {
      result.error = 'canceled';
      return result;
    }
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
    const certResponse: MoonlightHttpResult =
      await MoonlightClient.httpGet(dir, host, httpPort, getCertPath, false, undefined,
        PAIR_CERT_TIMEOUT_MS, undefined, cancel);
    if (!certResponse.ok) {
      result.error = cancel !== undefined && cancel.isCanceled() ? 'canceled' : certResponse.error;
      return result;
    }
    if (certResponse.status === 409) {
      result.error = 'rejected';
      return result;
    }
    let rootCode: number = 0;
    if (certResponse.body.length > 0) {
      rootCode = MoonlightXml.statusCode(certResponse.body);
      if (rootCode === 409) {
        result.error = 'rejected';
        return result;
      }
    }
    if (rootCode !== 200) {
      result.error = 'failed';
      return result;
    }
    let outcome: HttpOutcome = MoonlightClient.bodyOrError(certResponse);
    if (outcome.body.length === 0) {
      result.error = outcome.error === 'busy' ? 'rejected' : 'failed';
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }
    const serverCertHex: string = MoonlightXml.read(outcome.body, 'plaincert');
    const serverCertBytes: Uint8Array | undefined = MoonlightCrypto.fromHex(serverCertHex);
    if (serverCertBytes === undefined || serverCertBytes.length === 0) {
      result.error = 'rejected';
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
      MoonlightClient.pairPath(`clientchallenge=${encryptedChallenge}`, uniqueId, uuid),
      false, undefined, undefined, undefined, cancel));
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
      MoonlightClient.pairPath(`serverchallengeresp=${challengeResponse}`, uniqueId, uuid),
      false, undefined, undefined, undefined, cancel));
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
      MoonlightClient.pairPath(`clientpairingsecret=${clientPairingSecret}`, uniqueId, uuid),
      false, undefined, undefined, undefined, cancel));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }

    outcome = MoonlightClient.bodyOrError(await MoonlightClient.httpGet(dir, host, httpsPort,
      MoonlightClient.pairPath('phrase=pairchallenge', uniqueId, uuid),
      true, serverCertDer, undefined, undefined, cancel));
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

  // Windows scale descriptor of the foundation-sunshine virtual display.
  static async fetchVirtualDisplay(dir: string, host: string,
    httpsPort: number): Promise<MoonlightDisplay> {
    const display: MoonlightDisplay = new MoonlightDisplay();
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined) {
      hilog.warn(0xD001, 'HmRdpMoonlight', 'displays: no pinned cert');
      return display;
    }
    const response: MoonlightHttpResult =
      await MoonlightClient.httpGet(dir, host, httpsPort, '/displays', true, pin, 8000);
    if (response.ok && response.status === 200) {
      MoonlightClient.fillVirtualDisplay(response.body, display);
    }
    if (display.usable()) {
      hilog.info(0xD001, 'HmRdpMoonlight',
        'displays id=%{public}s name=%{public}s supported=[%{public}s]',
        display.deviceId.length > 0 ? display.deviceId : '-',
        display.displayName.length > 0 ? display.displayName : '-',
        MoonlightClient.describeNumbers(display.supportedScalePercents));
    } else if (response.ok && response.status === 200) {
      // Endpoint answered but no virtual display in it: worth the body (a 404 host is just "unsupported").
      hilog.info(0xD001, 'HmRdpMoonlight', 'displays: no virtual display body=%{public}s',
        MoonlightClient.snippet(response.body, 400));
    }
    return display;
  }

  // Windows' real display-scale steps (Settings dropdown).
  private static readonly WINDOWS_SCALE_STEPS: number[] =
    [100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500];

  // Some foundation-sunshine builds label Windows step indices with their own table, which
  // carries values Windows does not have (120/140/160/180); a percent then has to be turned
  // into a step index and sent as the host's label for that index. Once the host reports only
  // real Windows values its labels are the real percents, so this falls back to the nearest
  // one — i.e. it stays correct if the host is ever fixed, and needs no client change.
  static resolveScaleTarget(percent: number, hostLabels: number[]): MoonlightScaleTarget {
    const target: MoonlightScaleTarget = new MoonlightScaleTarget();
    if (hostLabels.length === 0) {
      target.label = percent;
      target.percent = percent;
      return target;
    }
    if (MoonlightClient.labelsAreWindowsSteps(hostLabels)) {
      const value: number = MoonlightClient.nearestValue(percent, hostLabels);
      target.label = value;
      target.percent = value;
      return target;
    }
    const steps: number[] = MoonlightClient.WINDOWS_SCALE_STEPS;
    let index: number = steps.indexOf(percent);
    if (index < 0) {
      index = MoonlightClient.nearestIndex(percent, steps);
    }
    if (index > hostLabels.length - 1) {
      index = hostLabels.length - 1;
    }
    target.label = hostLabels[index];
    target.percent = MoonlightClient.stepPercent(index);
    return target;
  }

  // True Windows percent behind a value the host reported (0 when it cannot be mapped).
  static trueScalePercent(hostLabel: number, hostLabels: number[]): number {
    if (hostLabel <= 0) {
      return 0;
    }
    if (MoonlightClient.labelsAreWindowsSteps(hostLabels)) {
      return hostLabel;
    }
    const index: number = hostLabels.indexOf(hostLabel);
    return index >= 0 ? MoonlightClient.stepPercent(index) : 0;
  }

  private static stepPercent(index: number): number {
    const steps: number[] = MoonlightClient.WINDOWS_SCALE_STEPS;
    return index >= 0 && index < steps.length ? steps[index] : 0;
  }

  private static labelsAreWindowsSteps(hostLabels: number[]): boolean {
    for (let i = 0; i < hostLabels.length; i++) {
      if (MoonlightClient.WINDOWS_SCALE_STEPS.indexOf(hostLabels[i]) < 0) {
        return false;
      }
    }
    return true;
  }

  private static nearestValue(percent: number, values: number[]): number {
    return values[MoonlightClient.nearestIndex(percent, values)];
  }

  private static nearestIndex(percent: number, values: number[]): number {
    let best: number = 0;
    let bestDiff: number = Math.abs(percent - values[0]);
    for (let i = 1; i < values.length; i++) {
      const diff: number = Math.abs(percent - values[i]);
      if (diff < bestDiff) {
        best = i;
        bestDiff = diff;
      }
    }
    return best;
  }

  // Applies a Windows display scale (percent). false when the host refused it.
  static async setDisplayScale(dir: string, host: string, httpsPort: number, deviceId: string,
    displayName: string, scalePercent: number): Promise<boolean> {
    const pin: Uint8Array | undefined = MoonlightIdentity.loadPinnedServerCert(dir, host);
    if (pin === undefined) {
      return false;
    }
    const request: DisplayScaleRequest = new DisplayScaleRequest();
    request.scale_percent = scalePercent;
    request.device_id = deviceId;
    request.display_name = displayName;
    const options: MoonlightHttpOptions = new MoonlightHttpOptions();
    options.useTls = true;
    options.host = host;
    options.port = httpsPort;
    options.path = '/display-scale';
    options.method = 'POST';
    options.contentType = 'application/json';
    options.body = JSON.stringify(request);
    options.pinnedServerCertDer = pin;
    options.timeoutMs = 8000;
    const response: MoonlightHttpResult =
      await MoonlightClient.performWithCredential(dir, options);
    const ok: boolean = response.ok && response.status === 200;
    if (ok) {
      hilog.info(0xD001, 'HmRdpMoonlight', 'display-scale label %{public}d ok', scalePercent);
    } else {
      hilog.info(0xD001, 'HmRdpMoonlight', 'display-scale label %{public}d status=%{public}d body=%{public}s',
        scalePercent, response.status, MoonlightClient.snippet(response.body, 900));
    }
    return ok;
  }

  // The virtual display is announced with the friendly name "Zako HDR".
  private static fillVirtualDisplay(body: string, display: MoonlightDisplay): void {
    const marker: string = '"friendly_name":"Zako HDR"';
    const at: number = body.indexOf(marker);
    if (at < 0) {
      return;
    }
    // Keys are dumped alphabetically: device_id/display_name precede friendly_name,
    // scale_set_supported/supported_scale_percents follow it. A GUID device_id contains
    // braces itself, so the object must never be sliced by brace positions.
    display.deviceId = MoonlightClient.jsonStringBack(body, '"device_id":"', at);
    display.displayName = MoonlightClient.jsonStringBack(body, '"display_name":"', at);
    display.currentScalePercent = MoonlightClient.jsonNumberBack(body, '"current_scale_percent":', at);
    display.scaleSupported = body.indexOf('"scale_set_supported":true', at) >= 0;
    display.supportedScalePercents =
      MoonlightClient.jsonNumbersForward(body, '"supported_scale_percents":', at);
  }

  private static jsonNumberBack(body: string, key: string, before: number): number {
    const keyAt: number = body.lastIndexOf(key, before);
    if (keyAt < 0) {
      return 0;
    }
    const valueStart: number = keyAt + key.length;
    const valueEnd: number = body.indexOf(',', valueStart);
    const text: string = valueEnd < 0 ? body.substring(valueStart) : body.substring(valueStart, valueEnd);
    const value: number = Number.parseInt(text.trim(), 10);
    return Number.isNaN(value) ? 0 : value;
  }

  private static jsonStringBack(body: string, key: string, before: number): string {
    const keyAt: number = body.lastIndexOf(key, before);
    if (keyAt < 0) {
      return '';
    }
    const valueStart: number = keyAt + key.length;
    const valueEnd: number = body.indexOf('"', valueStart);
    if (valueEnd < 0) {
      return '';
    }
    return MoonlightClient.unescapeJson(body.substring(valueStart, valueEnd));
  }

  private static jsonNumbersForward(body: string, key: string, from: number): number[] {
    const values: number[] = [];
    const keyAt: number = body.indexOf(key, from);
    const open: number = keyAt < 0 ? -1 : body.indexOf('[', keyAt + key.length);
    // Compact JSON puts the array right after the key; a distant '[' is a different field.
    const close: number = open < 0 ? -1 : body.indexOf(']', open);
    if (open < 0 || close < 0) {
      return values;
    }
    const parts: string[] = body.substring(open + 1, close).split(',');
    for (let i = 0; i < parts.length; i++) {
      const value: number = Number.parseInt(parts[i].trim(), 10);
      if (!Number.isNaN(value)) {
        values.push(value);
      }
    }
    return values;
  }

  private static snippet(text: string, limit: number = 320): string {
    return text.length > limit ? text.substring(0, limit) : text;
  }

  private static describeNumbers(values: number[]): string {
    let text: string = '';
    for (let i = 0; i < values.length; i++) {
      text += i === 0 ? values[i].toString() : ',' + values[i].toString();
    }
    return text;
  }

  private static unescapeJson(text: string): string {
    let out: string = '';
    let i: number = 0;
    while (i < text.length) {
      if (text.charAt(i) === '\\' && i + 1 < text.length) {
        const next: string = text.charAt(i + 1);
        if (next === '\\' || next === '/') {
          out += next;
          i += 2;
          continue;
        }
      }
      out += text.charAt(i);
      i++;
    }
    return out;
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
    binary?: boolean, cancel?: MoonlightHttpCancel): Promise<MoonlightHttpResult> {
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
      return MoonlightHttp.perform(options, cancel);
    }
    return MoonlightClient.performWithCredential(dir, options, cancel);
  }

  // HTTPS with the paired identity (mTLS) + pinned cert; the key PEM lives only for the request.
  private static async performWithCredential(dir: string,
    options: MoonlightHttpOptions, cancel?: MoonlightHttpCancel): Promise<MoonlightHttpResult> {
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
      return await MoonlightHttp.perform(options, cancel);
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
