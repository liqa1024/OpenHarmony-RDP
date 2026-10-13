import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { util } from '@kit.ArkTS';
import { SuppressWarnings, SuppressWarningsType } from '@kit.BasicServicesKit';

class DerTlv {
  tag: number = 0;
  valueStart: number = 0;
  valueEnd: number = 0;
  next: number = 0;
}

// Minimal DER reader: only enough to walk the X.509 outer structure.
function readTlv(data: Uint8Array, pos: number): DerTlv | undefined {
  if (pos + 2 > data.length) {
    return undefined;
  }
  const tlv: DerTlv = new DerTlv();
  tlv.tag = data[pos];
  const first: number = data[pos + 1];
  let length: number = 0;
  let cursor: number = pos + 2;
  if ((first & 0x80) === 0) {
    length = first;
  } else {
    const count: number = first & 0x7f;
    if (count === 0 || count > 4 || cursor + count > data.length) {
      return undefined;
    }
    for (let i = 0; i < count; i++) {
      length = (length << 8) | data[cursor + i];
    }
    cursor += count;
  }
  if (cursor + length > data.length) {
    return undefined;
  }
  tlv.valueStart = cursor;
  tlv.valueEnd = cursor + length;
  tlv.next = cursor + length;
  return tlv;
}

@SuppressWarnings({ rules: [SuppressWarningsType.SYSCAP] })
export class MoonlightCrypto {
  static toHex(bytes: Uint8Array): string {
    let out: string = '';
    for (let i = 0; i < bytes.length; i++) {
      const value: number = bytes[i];
      out += value < 16 ? '0' + value.toString(16) : value.toString(16);
    }
    return out;
  }

  static fromHex(hex: string): Uint8Array | undefined {
    if (hex.length % 2 !== 0) {
      return undefined;
    }
    const out: Uint8Array = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; i++) {
      const hi: number = MoonlightCrypto.nibble(hex.charCodeAt(i * 2));
      const lo: number = MoonlightCrypto.nibble(hex.charCodeAt(i * 2 + 1));
      if (hi < 0 || lo < 0) {
        return undefined;
      }
      out[i] = (hi << 4) | lo;
    }
    return out;
  }

  static utf8(text: string): Uint8Array {
    return new util.TextEncoder().encodeInto(text);
  }

  static text(bytes: Uint8Array): string {
    return new util.TextDecoder().decodeToString(bytes);
  }

  // SHA-256 or SHA-1, matching the server generation.
  static hash(sha256: boolean, data: Uint8Array): Uint8Array | undefined {
    try {
      const md: cryptoFramework.Md = cryptoFramework.createMd(sha256 ? 'SHA256' : 'SHA1');
      md.updateSync(MoonlightCrypto.blob(data));
      return md.digestSync().data;
    } catch (error) {
      return undefined;
    }
  }

  // AES-128-ECB, no padding; `data` is zero-padded up to a whole block, exactly
  // as the Android and Qt clients use the raw block cipher.
  static aesEcb(encrypt: boolean, key: Uint8Array, data: Uint8Array): Uint8Array | undefined {
    if (key.length !== 16 || data.length === 0) {
      return undefined;
    }
    try {
      const generator: cryptoFramework.SymKeyGenerator = cryptoFramework.createSymKeyGenerator('AES128');
      const symKey: cryptoFramework.SymKey = generator.convertKeySync(MoonlightCrypto.blob(key));
      const cipher: cryptoFramework.Cipher = cryptoFramework.createCipher('AES128|ECB|NoPadding');
      const mode: cryptoFramework.CryptoMode =
        encrypt ? cryptoFramework.CryptoMode.ENCRYPT_MODE : cryptoFramework.CryptoMode.DECRYPT_MODE;
      cipher.initSync(mode, symKey, null);
      const padded: Uint8Array = new Uint8Array(((data.length + 15) >> 4) << 4);
      padded.set(data, 0);
      return cipher.doFinalSync(MoonlightCrypto.blob(padded)).data;
    } catch (error) {
      return undefined;
    }
  }

  static randomHex(length: number): string {
    if (length < 1 || length > 256) {
      return '';
    }
    try {
      const bytes: Uint8Array = cryptoFramework.createRandom().generateRandomSync(length).data;
      return MoonlightCrypto.toHex(bytes);
    } catch (error) {
      return '';
    }
  }

  static signSha256(priKeyPkcs8Der: Uint8Array, data: Uint8Array): Uint8Array | undefined {
    try {
      const generator: cryptoFramework.AsyKeyGenerator =
        cryptoFramework.createAsyKeyGenerator('RSA2048|PRIMES_2');
      const keyPair: cryptoFramework.KeyPair =
        generator.convertKeySync(null, MoonlightCrypto.blob(priKeyPkcs8Der));
      const signer: cryptoFramework.Sign = cryptoFramework.createSign('RSA2048|PKCS1|SHA256');
      signer.initSync(keyPair.priKey);
      return signer.signSync(MoonlightCrypto.blob(data)).data;
    } catch (error) {
      return undefined;
    }
  }

  static verifySha256(pubKey: cryptoFramework.PubKey, data: Uint8Array,
    signature: Uint8Array): boolean {
    try {
      const verifier: cryptoFramework.Verify = cryptoFramework.createVerify('RSA2048|PKCS1|SHA256');
      verifier.initSync(pubKey);
      return verifier.verifySync(MoonlightCrypto.blob(data), MoonlightCrypto.blob(signature));
    } catch (error) {
      return false;
    }
  }

  static certSignatureToHex(certDer: Uint8Array): string | undefined {
    const signature: Uint8Array | undefined = MoonlightCrypto.certSignature(certDer);
    return signature === undefined ? undefined : MoonlightCrypto.toHex(signature);
  }

  // Raw signature bytes (ASN.1 BIT STRING) = third child of the outer SEQUENCE:
  // `Certificate ::= SEQUENCE { tbs, signatureAlgorithm, signatureValue }`.
  static certSignature(certDer: Uint8Array): Uint8Array | undefined {
    const outer: DerTlv | undefined = readTlv(certDer, 0);
    if (outer === undefined || outer.tag !== 0x30) {
      return undefined;
    }
    const tbs: DerTlv | undefined = readTlv(certDer, outer.valueStart);
    if (tbs === undefined) {
      return undefined;
    }
    const algorithm: DerTlv | undefined = readTlv(certDer, tbs.next);
    if (algorithm === undefined) {
      return undefined;
    }
    const signature: DerTlv | undefined = readTlv(certDer, algorithm.next);
    if (signature === undefined || signature.tag !== 0x03
      || signature.valueStart + 1 > signature.valueEnd) {
      return undefined;
    }
    return certDer.slice(signature.valueStart + 1, signature.valueEnd);
  }

  // ArkTS: DataBlob must be built as a typed literal.
  static blob(data: Uint8Array): cryptoFramework.DataBlob {
    const value: cryptoFramework.DataBlob = { data: data };
    return value;
  }

  static pemToDer(pem: string): Uint8Array | undefined {
    let body: string = '';
    const lines: string[] = pem.split('\n');
    for (let i = 0; i < lines.length; i++) {
      const line: string = lines[i].trim();
      if (line.length === 0 || line.startsWith('-----')) {
        continue;
      }
      body += line;
    }
    if (body.length === 0) {
      return undefined;
    }
    try {
      return new util.Base64Helper().decodeSync(body);
    } catch (error) {
      return undefined;
    }
  }

  private static nibble(code: number): number {
    if (code >= 0x30 && code <= 0x39) {
      return code - 0x30;
    }
    if (code >= 0x61 && code <= 0x66) {
      return code - 0x61 + 10;
    }
    if (code >= 0x41 && code <= 0x46) {
      return code - 0x41 + 10;
    }
    return -1;
  }
}
