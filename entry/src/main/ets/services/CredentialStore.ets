/*
 * HmRdp - password storage backed by HarmonyOS Asset Store Kit.
 *
 * Each password is stored as an ASSET secret keyed by the connection's stable
 * id. Passwords are only written after a session has connected successfully,
 * so an unverified (possibly wrong) password is never persisted. Connection
 * metadata lives in ConnectionStore and is independent of the password.
 */
import { asset } from '@kit.AssetStoreKit';
import { util } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpCredentials';

function toBytes(text: string): Uint8Array {
  return new util.TextEncoder().encodeInto(text);
}

function fromBytes(bytes: Uint8Array): string {
  return util.TextDecoder.create('utf-8').decodeToString(bytes);
}

export class CredentialStore {
  static async savePassword(connectionId: string, password: string): Promise<void> {
    if (connectionId.length === 0) {
      return;
    }
    await CredentialStore.removePassword(connectionId);
    if (password.length === 0) {
      return;
    }
    const attributes: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    attributes.set(asset.Tag.ALIAS, toBytes(connectionId));
    attributes.set(asset.Tag.SECRET, toBytes(password));
    attributes.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    attributes.set(asset.Tag.AUTH_TYPE, asset.AuthType.NONE);
    attributes.set(asset.Tag.SYNC_TYPE, asset.SyncType.NEVER);
    try {
      await asset.add(attributes);
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'save password failed: %{public}s', JSON.stringify(error));
    }
  }

  static async loadPassword(connectionId: string): Promise<string> {
    if (connectionId.length === 0) {
      return '';
    }
    try {
      const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
      query.set(asset.Tag.ALIAS, toBytes(connectionId));
      query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
      const results: Array<asset.AssetMap> = await asset.query(query);
      if (results.length === 0) {
        return '';
      }
      const value: asset.Value | undefined = results[0].get(asset.Tag.SECRET);
      if (value === undefined || !(value instanceof Uint8Array)) {
        return '';
      }
      return fromBytes(value);
    } catch (error) {
      return '';
    }
  }

  static async removePassword(connectionId: string): Promise<void> {
    if (connectionId.length === 0) {
      return;
    }
    const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    query.set(asset.Tag.ALIAS, toBytes(connectionId));
    try {
      await asset.remove(query);
    } catch (error) {
      // Missing asset is not an error for callers.
    }
  }
}
