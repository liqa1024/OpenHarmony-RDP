import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { MoonlightIdentity } from './MoonlightIdentity';

export class MoonlightHttpOptions {
  method: string = 'GET';
  host: string = '';
  port: number = 47989;
  path: string = '/';
  body: string = '';
  contentType: string = 'application/xml';
  useTls: boolean = false;
  // Empty = first contact (accept any cert); non-empty pins the peer's public key.
  pinnedServerCertDer: Uint8Array = new Uint8Array(0);
  certPath: string = '';
  keyPath: string = '';
  timeoutMs: number = 10000;
  binary: boolean = false;
}

export class MoonlightHttpResult {
  // A 4xx/5xx still counts as ok; the caller reads the body's <root status_code>.
  ok: boolean = false;
  status: number = 0;
  body: string = '';
  bodyBytes: ArrayBuffer | undefined = undefined;
  error: string = '';
}

export class MoonlightHttp {
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
        expectDataType: options.binary ? http.HttpDataType.ARRAY_BUFFER
                                       : http.HttpDataType.STRING,
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
      if (options.binary) {
        if (raw instanceof ArrayBuffer) {
          result.bodyBytes = raw;
        }
      } else {
        result.body = typeof raw === 'string' ? raw : '';
      }
      if (result.status !== 200) {
        hilog.warn(0xD001, 'HmRdpMoonlight', 'http %{public}s:%{public}d%{public}s status=%{public}d',
          options.host, options.port, MoonlightHttp.endpoint(options.path), result.status);
      }
      return result;
    } catch (error) {
      result.error = MoonlightHttp.errorToken(error as BusinessError);
      hilog.error(0xD001, 'HmRdpMoonlight', 'http %{public}s:%{public}d%{public}s error=%{public}s',
        options.host, options.port, MoonlightHttp.endpoint(options.path), result.error);
      return result;
    } finally {
      request.destroy();
    }
  }

  private static endpoint(path: string): string {
    const index: number = path.indexOf('?');
    return index >= 0 ? path.substring(0, index) : path;
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
