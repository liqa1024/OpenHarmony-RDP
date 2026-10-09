import { cert } from '@kit.DeviceCertificateKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { huks } from '@kit.UniversalKeystoreKit';
import { util } from '@kit.ArkTS';
import { fileIo } from '@kit.CoreFileKit';
import { SuppressWarnings, SuppressWarningsType } from '@kit.BasicServicesKit';
import { MoonlightCrypto } from './MoonlightCrypto';

const CN: string = 'NVIDIA GameStream Client';
const CLIENT_CERT_FILE: string = 'client.crt';
const CLIENT_KEY_FILE: string = 'client.key.enc';
const PIN_PREFIX: string = 'server_';
const PIN_SUFFIX: string = '.der';
// HUKS wrapping key: non-exportable, so the AES key never leaves the secure environment.
const HUKS_ALIAS: string = 'hmrdp_moonlight_cred_v1';
const NONCE_SIZE: number = 12;
const TAG_SIZE: number = 16;
const MAGIC: Uint8Array = new Uint8Array([0x48, 0x4d, 0x52, 0x44, 0x50, 0x4d, 0x4c, 0x31]); // HMRDPML1

function aadBytes(): Uint8Array {
  return MoonlightCrypto.utf8('HmRdpMoonlightCredV1');
}

export class MoonlightClientIdentity {
  certPem: string = '';
  keyPkcs8Der: Uint8Array = new Uint8Array(0);
}

export class MoonlightCredential {
  ok: boolean = false;
  certPem: string = '';
  keyPkcs8Der: Uint8Array = new Uint8Array(0);
  error: string = '';
}

function concat(parts: Uint8Array[]): Uint8Array {
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

function derLength(length: number): Uint8Array {
  if (length < 0x80) {
    return new Uint8Array([length]);
  }
  const body: number[] = [];
  let value: number = length;
  while (value > 0) {
    body.unshift(value & 0xff);
    value = value >> 8;
  }
  const out: Uint8Array = new Uint8Array(body.length + 1);
  out[0] = 0x80 | body.length;
  for (let i = 0; i < body.length; i++) {
    out[i + 1] = body[i];
  }
  return out;
}

function derTlv(tag: number, content: Uint8Array): Uint8Array {
  return concat([new Uint8Array([tag]), derLength(content.length), content]);
}

function derSequence(parts: Uint8Array[]): Uint8Array {
  return derTlv(0x30, concat(parts));
}

function derPositiveInteger(bytes: Uint8Array): Uint8Array {
  if (bytes.length > 0 && (bytes[0] & 0x80) !== 0) {
    const padded: Uint8Array = new Uint8Array(bytes.length + 1);
    padded.set(bytes, 1);
    return derTlv(0x02, padded);
  }
  return derTlv(0x02, bytes);
}

function derUtcTime(date: Date): Uint8Array {
  const pad = (value: number): string => value < 10 ? '0' + value : '' + value;
  const text: string = pad(date.getUTCFullYear() % 100) + pad(date.getUTCMonth() + 1)
    + pad(date.getUTCDate()) + pad(date.getUTCHours()) + pad(date.getUTCMinutes())
    + pad(date.getUTCSeconds()) + 'Z';
  return derTlv(0x17, MoonlightCrypto.utf8(text));
}

function derName(): Uint8Array {
  const cnOid: Uint8Array = new Uint8Array([0x55, 0x04, 0x03]); // 2.5.4.3
  const attribute: Uint8Array = derSequence([
    derTlv(0x06, cnOid),
    derTlv(0x0c, MoonlightCrypto.utf8(CN))
  ]);
  return derSequence([derTlv(0x31, attribute)]);
}

function derSha256WithRsa(): Uint8Array {
  // 1.2.840.113549.1.1.11
  const oid: Uint8Array = new Uint8Array([0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b]);
  return derSequence([derTlv(0x06, oid), new Uint8Array([0x05, 0x00])]);
}

function derBitString(bytes: Uint8Array): Uint8Array {
  const unused: Uint8Array = new Uint8Array([0x00]);
  return derTlv(0x03, concat([unused, bytes]));
}

function pemEncode(label: string, der: Uint8Array): string {
  const base64: string = new util.Base64Helper().encodeToStringSync(der);
  let body: string = '';
  for (let i = 0; i < base64.length; i += 64) {
    body += base64.substring(i, i + 64) + '\n';
  }
  // LF endings only: the host rejects CRLF in the PEM.
  return `-----BEGIN ${label}-----\n${body}-----END ${label}-----\n`;
}

function hostFileName(hostKey: string): string {
  // Hostnames are case-insensitive and a trailing dot is the DNS root: fold both
  // so the pin key matches the connection-uniqueness key (RdpModels.mlHostId).
  let key: string = hostKey.trim().toLowerCase();
  if (key.endsWith('.')) {
    key = key.substring(0, key.length - 1);
  }
  let name: string = '';
  for (let i = 0; i < key.length; i++) {
    const code: number = key.charCodeAt(i);
    const safe: boolean = (code >= 0x61 && code <= 0x7a) || (code >= 0x41 && code <= 0x5a)
      || (code >= 0x30 && code <= 0x39) || code === 0x2e || code === 0x2d || code === 0x5f;
    name += safe ? key.charAt(i) : '_';
  }
  if (name.length === 0) {
    name = 'default';
  }
  return PIN_PREFIX + name + PIN_SUFFIX;
}

function baseProperties(keySize: number, purpose: number): huks.HuksParam[] {
  const properties: huks.HuksParam[] = [
    { tag: huks.HuksTag.HUKS_TAG_ALGORITHM, value: huks.HuksKeyAlg.HUKS_ALG_AES },
    { tag: huks.HuksTag.HUKS_TAG_KEY_SIZE, value: keySize },
    { tag: huks.HuksTag.HUKS_TAG_PURPOSE, value: purpose },
    { tag: huks.HuksTag.HUKS_TAG_PADDING, value: huks.HuksKeyPadding.HUKS_PADDING_NONE },
    { tag: huks.HuksTag.HUKS_TAG_BLOCK_MODE, value: huks.HuksCipherMode.HUKS_MODE_GCM }
  ];
  return properties;
}

async function ensureHuksKey(): Promise<boolean> {
  try {
    const exists: boolean = await huks.isKeyItemExist(HUKS_ALIAS, { properties: [] });
    if (exists) {
      return true;
    }
  } catch (error) {
    // A missing key is not an error; fall through to generate it.
  }
  try {
    const options: huks.HuksOptions = {
      properties: baseProperties(huks.HuksKeySize.HUKS_AES_KEY_SIZE_256,
        huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_ENCRYPT | huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_DECRYPT)
    };
    await huks.generateKeyItem(HUKS_ALIAS, options);
    return true;
  } catch (error) {
    return false;
  }
}

async function huksEncrypt(nonce: Uint8Array, plain: Uint8Array): Promise<Uint8Array | undefined> {
  try {
    const properties: huks.HuksParam[] = baseProperties(huks.HuksKeySize.HUKS_AES_KEY_SIZE_256,
      huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_ENCRYPT);
    properties.push({ tag: huks.HuksTag.HUKS_TAG_NONCE, value: nonce });
    properties.push({ tag: huks.HuksTag.HUKS_TAG_ASSOCIATED_DATA, value: aadBytes() });
    const options: huks.HuksOptions = { properties: properties, inData: plain };
    const session: huks.HuksSessionHandle = await huks.initSession(HUKS_ALIAS, options);
    const result: huks.HuksReturnResult = await huks.finishSession(session.handle, options);
    return result.outData;
  } catch (error) {
    return undefined;
  }
}

async function huksDecrypt(nonce: Uint8Array, wrapped: Uint8Array): Promise<Uint8Array | undefined> {
  if (wrapped.length <= TAG_SIZE) {
    return undefined;
  }
  const body: Uint8Array = wrapped.slice(0, wrapped.length - TAG_SIZE);
  const tag: Uint8Array = wrapped.slice(wrapped.length - TAG_SIZE);
  try {
    const properties: huks.HuksParam[] = baseProperties(huks.HuksKeySize.HUKS_AES_KEY_SIZE_256,
      huks.HuksKeyPurpose.HUKS_KEY_PURPOSE_DECRYPT);
    properties.push({ tag: huks.HuksTag.HUKS_TAG_NONCE, value: nonce });
    properties.push({ tag: huks.HuksTag.HUKS_TAG_ASSOCIATED_DATA, value: aadBytes() });
    properties.push({ tag: huks.HuksTag.HUKS_TAG_AE_TAG, value: tag });
    const options: huks.HuksOptions = { properties: properties, inData: body };
    const session: huks.HuksSessionHandle = await huks.initSession(HUKS_ALIAS, options);
    const result: huks.HuksReturnResult = await huks.finishSession(session.handle, options);
    return result.outData;
  } catch (error) {
    return undefined;
  }
}

function ensureDir(path: string): boolean {
  try {
    if (fileIo.accessSync(path)) {
      return true;
    }
    fileIo.mkdirSync(path, true);
    return true;
  } catch (error) {
    return false;
  }
}

function writeBytes(path: string, data: Uint8Array): boolean {
  // Write to a temporary file then rename: a torn write must not look valid.
  const tmp: string = path + '.tmp';
  let file: fileIo.File | undefined = undefined;
  try {
    file = fileIo.openSync(tmp, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY
      | fileIo.OpenMode.TRUNC);
    fileIo.writeSync(file.fd, data.buffer as ArrayBuffer);
    fileIo.closeSync(file);
    file = undefined;
    fileIo.renameSync(tmp, path);
    return true;
  } catch (error) {
    if (file !== undefined) {
      try {
        fileIo.closeSync(file);
      } catch (ignored) {
      }
    }
    try {
      fileIo.unlinkSync(tmp);
    } catch (ignored) {
    }
    return false;
  }
}

function readBytes(path: string): Uint8Array | undefined {
  let file: fileIo.File | undefined = undefined;
  try {
    file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
    const size: number = fileIo.statSync(file.fd).size;
    if (size <= 0) {
      return undefined;
    }
    const buffer: ArrayBuffer = new ArrayBuffer(size);
    const read: number = fileIo.readSync(file.fd, buffer);
    return new Uint8Array(buffer.slice(0, read));
  } catch (error) {
    return undefined;
  } finally {
    if (file !== undefined) {
      try {
        fileIo.closeSync(file);
      } catch (ignored) {
      }
    }
  }
}

function readText(path: string): string | undefined {
  let file: fileIo.File | undefined = undefined;
  try {
    file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
    const size: number = fileIo.statSync(file.fd).size;
    if (size <= 0) {
      return undefined;
    }
    const buffer: ArrayBuffer = new ArrayBuffer(size);
    fileIo.readSync(file.fd, buffer);
    return new util.TextDecoder().decodeToString(new Uint8Array(buffer));
  } catch (error) {
    return undefined;
  } finally {
    if (file !== undefined) {
      try {
        fileIo.closeSync(file);
      } catch (ignored) {
      }
    }
  }
}

@SuppressWarnings({ rules: [SuppressWarningsType.SYSCAP] })
export class MoonlightIdentity {
  static rootDir(dir: string): string {
    return dir + '/moonlight';
  }

  static generateClientIdentity(): MoonlightClientIdentity | undefined {
    let keyPair: cryptoFramework.KeyPair;
    let publicKeyInfo: Uint8Array = new Uint8Array(0);
    let privateKey: Uint8Array = new Uint8Array(0);
    try {
      const generator: cryptoFramework.AsyKeyGenerator =
        cryptoFramework.createAsyKeyGenerator('RSA2048|PRIMES_2');
      keyPair = generator.generateKeyPairSync();
      publicKeyInfo = keyPair.pubKey.getEncoded().data;
      privateKey = keyPair.priKey.getEncoded().data;
    } catch (error) {
      return undefined;
    }
    const serialSeed: string = MoonlightCrypto.randomHex(8);
    if (serialSeed.length === 0) {
      return undefined;
    }
    let serial: Uint8Array = MoonlightCrypto.fromHex(serialSeed) ?? new Uint8Array(0);
    if (serial.length === 0) {
      serial = new Uint8Array([0x01]);
    }
    const version: Uint8Array = derTlv(0xa0, derTlv(0x02, new Uint8Array([0x02])));
    const algorithm: Uint8Array = derSha256WithRsa();
    const name: Uint8Array = derName();
    const notBefore: Date = new Date();
    const notAfter: Date = new Date(notBefore.getTime());
    notAfter.setUTCFullYear(notBefore.getUTCFullYear() + 20);
    const validity: Uint8Array = derSequence([derUtcTime(notBefore), derUtcTime(notAfter)]);
    const tbs: Uint8Array = derSequence([
      version,
      derPositiveInteger(serial),
      algorithm,
      name,
      validity,
      name,
      publicKeyInfo
    ]);
    const signature: Uint8Array | undefined = MoonlightCrypto.signSha256(privateKey, tbs);
    if (signature === undefined) {
      return undefined;
    }
    const certificate: Uint8Array = derSequence([tbs, algorithm, derBitString(signature)]);
    const identity: MoonlightClientIdentity = new MoonlightClientIdentity();
    identity.certPem = pemEncode('CERTIFICATE', certificate);
    identity.keyPkcs8Der = privateKey;
    return identity;
  }

  static async ensureCredential(dir: string): Promise<MoonlightCredential> {
    const root: string = MoonlightIdentity.rootDir(dir);
    if (!ensureDir(root)) {
      const failed: MoonlightCredential = new MoonlightCredential();
      failed.error = 'dir';
      return failed;
    }
    const existing: MoonlightCredential = await MoonlightIdentity.loadCredential(dir);
    if (existing.ok) {
      return existing;
    }
    if (existing.error === 'corrupt') {
      // A new identity needs a fresh wrapping key and a fresh pairing.
      try {
        await huks.deleteKeyItem(HUKS_ALIAS, { properties: [] });
      } catch (ignored) {
      }
      MoonlightIdentity.clearPins(root);
      try {
        fileIo.unlinkSync(root + '/' + CLIENT_KEY_FILE);
      } catch (ignored) {
      }
    }
    const identity: MoonlightClientIdentity | undefined =
      MoonlightIdentity.generateClientIdentity();
    if (identity === undefined) {
      const failed: MoonlightCredential = new MoonlightCredential();
      failed.error = 'crypto';
      return failed;
    }
    if (!await ensureHuksKey()) {
      const failed: MoonlightCredential = new MoonlightCredential();
      failed.error = 'huks';
      return failed;
    }
    const nonceHex: string = MoonlightCrypto.randomHex(NONCE_SIZE);
    const nonce: Uint8Array | undefined = MoonlightCrypto.fromHex(nonceHex);
    if (nonce === undefined) {
      const failed: MoonlightCredential = new MoonlightCredential();
      failed.error = 'huks';
      return failed;
    }
    const cipher: Uint8Array | undefined = await huksEncrypt(nonce, identity.keyPkcs8Der);
    if (cipher === undefined) {
      const failed: MoonlightCredential = new MoonlightCredential();
      failed.error = 'huks';
      return failed;
    }
    const wrapped: Uint8Array = concat([MAGIC, nonce, cipher]);
    if (!writeBytes(root + '/' + CLIENT_CERT_FILE, MoonlightCrypto.utf8(identity.certPem))
      || !writeBytes(root + '/' + CLIENT_KEY_FILE, wrapped)) {
      const failed: MoonlightCredential = new MoonlightCredential();
      failed.error = 'write';
      return failed;
    }
    // Read it back through the decrypt path so a wrap/unwrap mismatch is caught
    // here rather than at the first request.
    return MoonlightIdentity.loadCredential(dir);
  }

  static async loadCredential(dir: string): Promise<MoonlightCredential> {
    const result: MoonlightCredential = new MoonlightCredential();
    const root: string = MoonlightIdentity.rootDir(dir);
    const certPem: string | undefined = readText(root + '/' + CLIENT_CERT_FILE);
    const wrapped: Uint8Array | undefined = readBytes(root + '/' + CLIENT_KEY_FILE);
    if (certPem === undefined || wrapped === undefined) {
      result.error = 'missing';
      return result;
    }
    const header: number = MAGIC.length + NONCE_SIZE;
    if (wrapped.length <= header + TAG_SIZE) {
      result.error = 'corrupt';
      return result;
    }
    for (let i = 0; i < MAGIC.length; i++) {
      if (wrapped[i] !== MAGIC[i]) {
        result.error = 'corrupt';
        return result;
      }
    }
    try {
      const exists: boolean = await huks.isKeyItemExist(HUKS_ALIAS, { properties: [] });
      if (!exists) {
        result.error = 'corrupt';
        return result;
      }
    } catch (error) {
      result.error = 'corrupt';
      return result;
    }
    const nonce: Uint8Array = wrapped.slice(MAGIC.length, header);
    const cipher: Uint8Array = wrapped.slice(header);
    const key: Uint8Array | undefined = await huksDecrypt(nonce, cipher);
    if (key === undefined) {
      result.error = 'corrupt';
      return result;
    }
    result.ok = true;
    result.certPem = certPem;
    result.keyPkcs8Der = key;
    return result;
  }

  static async clearCredential(dir: string): Promise<boolean> {
    const root: string = MoonlightIdentity.rootDir(dir);
    try {
      await huks.deleteKeyItem(HUKS_ALIAS, { properties: [] });
    } catch (ignored) {
    }
    try {
      fileIo.unlinkSync(root + '/' + CLIENT_CERT_FILE);
    } catch (ignored) {
    }
    try {
      fileIo.unlinkSync(root + '/' + CLIENT_KEY_FILE);
    } catch (ignored) {
    }
    MoonlightIdentity.clearPins(root);
    return true;
  }

  static loadPinnedServerCert(dir: string, hostKey: string): Uint8Array | undefined {
    return readBytes(MoonlightIdentity.rootDir(dir) + '/' + hostFileName(hostKey));
  }

  static storePinnedServerCert(dir: string, hostKey: string, der: Uint8Array): boolean {
    if (der.length === 0) {
      return false;
    }
    const root: string = MoonlightIdentity.rootDir(dir);
    if (!ensureDir(root)) {
      return false;
    }
    return writeBytes(root + '/' + hostFileName(hostKey), der);
  }

  static clearPinnedServerCert(dir: string, hostKey: string): boolean {
    try {
      fileIo.unlinkSync(MoonlightIdentity.rootDir(dir) + '/' + hostFileName(hostKey));
      return true;
    } catch (error) {
      return false;
    }
  }

  static clientCertPath(dir: string): string {
    return MoonlightIdentity.rootDir(dir) + '/' + CLIENT_CERT_FILE;
  }

  static writeClientKeyPem(dir: string, credential: MoonlightCredential): string | undefined {
    if (!credential.ok || credential.keyPkcs8Der.length === 0) {
      return undefined;
    }
    const path: string = MoonlightIdentity.rootDir(dir) + '/client.key.pem';
    const pem: string = pemEncode('PRIVATE KEY', credential.keyPkcs8Der);
    return writeBytes(path, MoonlightCrypto.utf8(pem)) ? path : undefined;
  }

  static removeClientKeyPem(dir: string): void {
    try {
      fileIo.unlinkSync(MoonlightIdentity.rootDir(dir) + '/client.key.pem');
    } catch (ignored) {
    }
  }

  static async serverPublicKeyHash(certDer: Uint8Array): Promise<string | undefined> {
    try {
      const blob: cert.EncodingBlob = {
        data: certDer,
        encodingFormat: cert.EncodingFormat.FORMAT_DER
      };
      const x509: cert.X509Cert = await cert.createX509Cert(blob);
      const spki: Uint8Array = x509.getPublicKey().getEncoded().data;
      const digest: Uint8Array | undefined = MoonlightCrypto.hash(true, spki);
      if (digest === undefined) {
        return undefined;
      }
      return new util.Base64Helper().encodeToStringSync(digest);
    } catch (error) {
      return undefined;
    }
  }

  static async serverPublicKey(certDer: Uint8Array): Promise<cryptoFramework.PubKey | undefined> {
    try {
      const blob: cert.EncodingBlob = {
        data: certDer,
        encodingFormat: cert.EncodingFormat.FORMAT_DER
      };
      const x509: cert.X509Cert = await cert.createX509Cert(blob);
      // The PubKey returned by the certificate is not accepted by
      // createVerify().initSync (platform FAQ), so re-import it from its SPKI.
      const spki: Uint8Array = x509.getPublicKey().getEncoded().data;
      const keyData: cryptoFramework.DataBlob = { data: spki };
      const pair: cryptoFramework.KeyPair =
        cryptoFramework.createAsyKeyGenerator('RSA2048|PRIMES_2').convertKeySync(keyData, null);
      return pair.pubKey;
    } catch (error) {
      return undefined;
    }
  }

  private static clearPins(root: string): void {
    let names: string[] = [];
    try {
      names = fileIo.listFileSync(root);
    } catch (error) {
      return;
    }
    for (let i = 0; i < names.length; i++) {
      const name: string = names[i];
      if (name.startsWith(PIN_PREFIX) && name.endsWith(PIN_SUFFIX)) {
        try {
          fileIo.unlinkSync(root + '/' + name);
        } catch (ignored) {
        }
      }
    }
  }
}
