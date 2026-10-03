/*
 * HmRdp - Moonlight (GameStream) control layer (ArkTS side).
 *
 * Drives serverinfo -> pair -> applist -> launch/resume against the host, using
 * the native HTTP/TLS primitive and the pairing crypto primitives; all XML is
 * parsed here (MoonlightXml). This mirrors Moonlight's Android PairingManager /
 * NvHTTP (see doc_agent/moonlight-native.md §4). Native owns transport, the
 * client identity and the pinned certificate only.
 *
 * NOTE: the native HTTP call is synchronous, so these methods block the calling
 * thread for the duration of one request; callers must keep that in mind (a
 * short request is sub-second, getservercert waits for the PIN on the host).
 */
import { util } from '@kit.ArkTS';
import {
  MoonlightHttpOptions, MoonlightHttpResult, moonlightAesEcb, moonlightCertSignature,
  moonlightClearPinnedServerCert, moonlightEnsureCredential, moonlightHttpRequest,
  moonlightPairChallengeResponse, moonlightPairDeriveKey, moonlightPairExpectedResponse,
  moonlightRandomHex, moonlightSignClientSecret, moonlightStorePinnedServerCert,
  moonlightVerifyServerSecret
} from 'libhmrdp.so';
import { AppListResult, NvServerInfo, PairResult, ServerInfoResult } from './MoonlightModels';
import { MoonlightXml } from './MoonlightXml';

const DEFAULT_HTTPS_PORT: number = 47984;
// Long timeout for getservercert: the host user must type the PIN first.
const PAIR_CERT_TIMEOUT_MS: number = 300000;

/** An HTTP body plus a stable error token when it could not be used. */
class HttpOutcome {
  body: string = '';
  error: string = '';
}

export class MoonlightClient {
  // Shared with other Moonlight clients so games started elsewhere can be quit.
  static readonly UNIQUE_ID: string = '0123456789ABCDEF';

  /** One `/serverinfo` probe over plain HTTP. */
  static fetchServerInfo(dir: string, host: string, port: number): ServerInfoResult {
    const result: ServerInfoResult = new ServerInfoResult();
    const path: string =
      `/serverinfo?uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const outcome: HttpOutcome =
      MoonlightClient.bodyOrError(MoonlightClient.httpGet(dir, host, port, path, false));
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
    info.majorVersion = MoonlightClient.majorVersion(
      MoonlightXml.readOptional(body, 'appversion') ?? '');
    info.serverCodecModeSupport =
      MoonlightXml.readNumber(body, 'ServerCodecModeSupport', 0);
    if (info.state.endsWith('_SERVER_BUSY')) {
      info.currentGame = MoonlightXml.readNumber(body, 'currentgame', 0);
    }
    result.info = info;
    result.ok = true;
    return result;
  }

  /** The host's application list over TLS (requires a paired credential + pin). */
  static fetchAppList(dir: string, host: string, httpsPort: number): AppListResult {
    const result: AppListResult = new AppListResult();
    const path: string =
      `/applist?uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const outcome: HttpOutcome =
      MoonlightClient.bodyOrError(MoonlightClient.httpGet(dir, host, httpsPort, path, true));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    result.apps = MoonlightXml.parseAppList(outcome.body);
    result.ok = true;
    return result;
  }

  /**
   * Pairs with the host using `pin` (shown to the user, typed on the host).
   * Ports: getservercert / clientchallenge / serverchallengeresp /
   * clientpairingsecret go over plain HTTP, pairchallenge over HTTPS; the
   * server certificate from getservercert is pinned on success.
   */
  static pair(dir: string, host: string, httpPort: number, httpsPort: number,
    majorVersion: number, pin: string): PairResult {
    const result: PairResult = new PairResult();
    const credential: string = moonlightEnsureCredential(dir);
    if (!credential.startsWith('1|')) {
      result.error = credential.length > 0 ? credential : 'no-cred';
      return result;
    }
    const clientCertPem: string = credential.substring(2);
    const clientCertHex: string = MoonlightClient.hexOfString(clientCertPem);

    const salt: string = moonlightRandomHex(16);
    const aesKey: string = moonlightPairDeriveKey(majorVersion, salt, pin);
    if (aesKey.length === 0) {
      result.error = 'crypto';
      return result;
    }

    // 1. getservercert (plain HTTP, long timeout: the host user types the PIN).
    const getCertPath: string = MoonlightClient.pairPath(
      `phrase=getservercert&salt=${salt}&clientcert=${clientCertHex}`);
    let outcome: HttpOutcome = MoonlightClient.bodyOrError(
      MoonlightClient.httpGet(dir, host, httpPort, getCertPath, false, undefined,
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
    if (serverCertHex.length === 0) {
      // Another device is pairing with the host.
      result.error = 'already';
      return result;
    }
    const serverCertSig: string = moonlightCertSignature(serverCertHex);
    const clientCertSig: string = moonlightCertSignature(clientCertHex);

    // 2. clientchallenge (plain HTTP).
    const randomChallenge: string = moonlightRandomHex(16);
    const encryptedChallenge: string = moonlightAesEcb(true, aesKey, randomChallenge);
    outcome = MoonlightClient.bodyOrError(MoonlightClient.httpGet(dir, host, httpPort,
      MoonlightClient.pairPath(`clientchallenge=${encryptedChallenge}`), false));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }
    const encryptedResponse: string = MoonlightXml.read(outcome.body, 'challengeresponse');
    const decryptedResponse: string = moonlightAesEcb(false, aesKey, encryptedResponse);
    const hashLenHex: number = (majorVersion >= 7 ? 32 : 20) * 2;
    if (decryptedResponse.length < hashLenHex + 32) {
      result.error = 'crypto';
      return result;
    }
    const serverResponse: string = decryptedResponse.substring(0, hashLenHex);
    const serverChallenge: string = decryptedResponse.substring(hashLenHex, hashLenHex + 32);

    // 3. serverchallengeresp (plain HTTP).
    const clientSecret: string = moonlightRandomHex(16);
    const challengeResponse: string = moonlightPairChallengeResponse(majorVersion, aesKey,
      serverChallenge, clientCertSig, clientSecret);
    outcome = MoonlightClient.bodyOrError(MoonlightClient.httpGet(dir, host, httpPort,
      MoonlightClient.pairPath(`serverchallengeresp=${challengeResponse}`), false));
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
    if (!moonlightVerifyServerSecret(serverCertHex, serverSecret, serverSignature)) {
      // The server secret did not verify against its certificate: MITM or a bad
      // host certificate.
      result.error = 'mitm';
      return result;
    }
    const expectedResponse: string = moonlightPairExpectedResponse(majorVersion, randomChallenge,
      serverCertSig, serverSecret);
    if (expectedResponse !== serverResponse) {
      // The challenge response did not match: the PIN was wrong.
      result.error = 'pin';
      return result;
    }

    // 4. clientpairingsecret (plain HTTP).
    const clientSignature: string = moonlightSignClientSecret(dir, clientSecret);
    const clientPairingSecret: string = clientSecret + clientSignature;
    outcome = MoonlightClient.bodyOrError(MoonlightClient.httpGet(dir, host, httpPort,
      MoonlightClient.pairPath(`clientpairingsecret=${clientPairingSecret}`), false));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }

    // 5. pairchallenge (HTTPS; pin the certificate we just received).
    outcome = MoonlightClient.bodyOrError(MoonlightClient.httpGet(dir, host, httpsPort,
      MoonlightClient.pairPath('phrase=pairchallenge'), true, serverCertHex));
    if (outcome.body.length === 0) {
      result.error = outcome.error;
      return result;
    }
    if (MoonlightXml.read(outcome.body, 'paired') !== '1') {
      result.error = 'failed';
      return result;
    }

    moonlightStorePinnedServerCert(dir, host, serverCertHex);
    result.ok = true;
    return result;
  }

  /** Removes the host pairing (`/unpair`) and drops the stored pin. */
  static unpair(dir: string, host: string, port: number): boolean {
    const path: string =
      `/unpair?uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
    const response: MoonlightHttpResult = MoonlightClient.httpGet(dir, host, port, path, false);
    moonlightClearPinnedServerCert(dir, host);
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

  private static pairPath(arguments_: string): string {
    return `/pair?devicename=roth&updateState=1&${arguments_}`
      + `&uniqueid=${MoonlightClient.UNIQUE_ID}&uuid=${MoonlightClient.uuid()}`;
  }

  private static uuid(): string {
    return moonlightRandomHex(16);
  }

  private static httpGet(dir: string, host: string, port: number, path: string, tls: boolean,
    pinHex?: string, timeoutMs?: number): MoonlightHttpResult {
    const options: MoonlightHttpOptions = {
      useTls: tls,
      host: host,
      port: port,
      path: path,
      method: 'GET',
      pinServerCertHex: pinHex,
      timeoutMs: timeoutMs
    };
    return moonlightHttpRequest(dir, options);
  }

  /**
   * Returns the response body, or an empty body with a stable error token. A
   * 4xx/5xx is reported by the `<root status_code>` inside the body, so only
   * transport failures and non-200 root statuses fail here.
   */
  private static bodyOrError(response: MoonlightHttpResult): HttpOutcome {
    const outcome: HttpOutcome = new HttpOutcome();
    if (!response.ok) {
      const error: string = response.error ?? '';
      outcome.error = error.length > 0 ? error : 'request';
      return outcome;
    }
    const body: string = response.body ?? '';
    if (body.length === 0) {
      outcome.error = 'http';
      return outcome;
    }
    if (MoonlightXml.statusCode(body) !== 200) {
      outcome.error = 'status';
      return outcome;
    }
    outcome.body = body;
    return outcome;
  }

  private static hexOfString(text: string): string {
    const bytes: Uint8Array = new util.TextEncoder().encodeInto(text);
    let out: string = '';
    for (let i = 0; i < bytes.length; i++) {
      const value: number = bytes[i];
      out += value < 16 ? '0' + value.toString(16) : value.toString(16);
    }
    return out;
  }
}
