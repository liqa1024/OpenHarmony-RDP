/*
 * HmRdp - Moonlight (GameStream) control transport (ArkTS side).
 *
 * One HTTP/1.1 request is the primitive the whole GameStream control surface is
 * built from (/serverinfo, /pair, /applist, /launch, /resume). The control port
 * is configurable (plain 47989, TLS 47984), the host is self-signed, and paired
 * requests need a client certificate (mutual TLS): all three are @ohos.net.http
 * options here. The response body is returned raw and parsed by the caller
 * (MoonlightXml). See MOONLIGHT_PLAN.md "HTTP 层的落地口径".
 */
import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { MoonlightIdentity } from './MoonlightIdentity';

export class MoonlightHttpOptions {
  method: string = 'GET';
  host: string = '';
  port: number = 47989;
  path: string = '/';
  body: string = '';
  contentType: string = 'application/xml';
  useTls: boolean = false;
  // Pinned server leaf certificate (DER). Empty means first contact: any
  // certificate is accepted; non-empty locks the peer's public key.
  pinnedServerCertDer: Uint8Array = new Uint8Array(0);
  // Mutual TLS: both are required together, and only on paired requests.
  certPath: string = '';
  keyPath: string = '';
  timeoutMs: number = 10000;
}

export class MoonlightHttpResult {
  // True when transport and HTTP framing succeeded; a 4xx/5xx is still ok (the
  // caller reads the status from the body's <root status_code>).
  ok: boolean = false;
  status: number = 0;
  body: string = '';
  // Stable error token when !ok: "request" / "resolve" / "connect" / "tls" /
  // "io" / "timeout" / "http" / "pin".
  error: string = '';
}

export class MoonlightHttp {
  /** Performs one request; blocking on the caller's async continuation. */
  static async perform(options: MoonlightHttpOptions): Promise<MoonlightHttpResult> {
    const result: MoonlightHttpResult = new MoonlightHttpResult();
    if (options.host.length === 0) {
      result.error = 'request';
      return result;
    }
    const scheme: string = options.useTls ? 'https' : 'http';
    const url: string = `${scheme}://${options.host}:${options.port}${options.path}`;
    const request: http.HttpRequest = http.createHttp();
    try {
      const headers: Record<string, string> = { 'Content-Type': options.contentType };
      const method: http.RequestMethod = options.method === 'POST'
        ? http.RequestMethod.POST : http.RequestMethod.GET;
      const requestOptions: http.HttpRequestOptions = {
        method: method,
        connectTimeout: options.timeoutMs,
        readTimeout: options.timeoutMs,
        expectDataType: http.HttpDataType.STRING,
        header: headers
      };
      if (options.body.length > 0) {
        requestOptions.extraData = options.body;
      }
      if (options.useTls) {
        // The host is self-signed: never validate against the system CA.
        requestOptions.remoteValidation = 'skip';
        if (options.pinnedServerCertDer.length > 0) {
          const hash: string | undefined =
            await MoonlightIdentity.serverPublicKeyHash(options.pinnedServerCertDer);
          if (hash === undefined) {
            result.error = 'pin';
            return result;
          }
          const pinning: http.CertificatePinning = {
            publicKeyHash: hash,
            hashAlgorithm: 'SHA-256'
          };
          requestOptions.certificatePinning = pinning;
        }
        if (options.certPath.length > 0 && options.keyPath.length > 0) {
          const clientCert: http.ClientCert = {
            certPath: options.certPath,
            keyPath: options.keyPath
          };
          requestOptions.clientCert = clientCert;
        }
      }
      const response: http.HttpResponse = await request.request(url, requestOptions);
      result.ok = true;
      result.status = response.responseCode as number;
      const raw: string | Object | ArrayBuffer = response.result;
      result.body = typeof raw === 'string' ? raw : '';
      return result;
    } catch (error) {
      result.error = MoonlightHttp.errorToken(error as BusinessError);
      return result;
    } finally {
      request.destroy();
    }
  }

  private static errorToken(error: BusinessError): string {
    const code: number = error !== undefined ? error.code : 0;
    if (code === 2300006) {
      return 'resolve';
    }
    if (code === 2300007) {
      return 'connect';
    }
    if (code === 2300008) {
      return 'tls';
    }
    if (code === 2300005) {
      return 'io';
    }
    if (code === 2300028) {
      return 'timeout';
    }
    return 'request';
  }
}
