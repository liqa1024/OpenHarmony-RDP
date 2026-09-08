/*
 * HmRdp - credential storage backed by HarmonyOS Asset Store Kit.
 *
 * Passwords are stored as ASSET secrets (encrypted in a secure environment),
 * never in plaintext files or preferences. Non-sensitive fields (username,
 * domain, display options) ride along as data labels on the same asset.
 */
import { asset } from '@kit.AssetStoreKit';
import { util } from '@kit.ArkTS';
import { SavedConnection } from '../model/RdpModels';

function toBytes(text: string): Uint8Array {
  return new util.TextEncoder().encodeInto(text);
}

function fromBytes(bytes: Uint8Array): string {
  return util.TextDecoder.create('utf-8').decodeToString(bytes);
}

export class CredentialStore {
  static aliasOf(host: string, port: number): string {
    return `${host}:${port}`;
  }

  static async saveConnection(conn: SavedConnection): Promise<void> {
    if (conn.password.length === 0) {
      await CredentialStore.remove(conn.alias);
      return;
    }
    await CredentialStore.remove(conn.alias);
    const attributes: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    attributes.set(asset.Tag.ALIAS, toBytes(conn.alias));
    attributes.set(asset.Tag.SECRET, toBytes(conn.password));
    attributes.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    attributes.set(asset.Tag.AUTH_TYPE, asset.AuthType.NONE);
    attributes.set(asset.Tag.SYNC_TYPE, asset.SyncType.NEVER);
    if (conn.username.length > 0) {
      attributes.set(asset.Tag.DATA_LABEL_NORMAL_1, toBytes(conn.username));
    }
    if (conn.domain.length > 0) {
      attributes.set(asset.Tag.DATA_LABEL_NORMAL_2, toBytes(conn.domain));
    }
    attributes.set(asset.Tag.DATA_LABEL_NORMAL_3, toBytes(CredentialStore.encodeOptions(conn)));
    await asset.add(attributes);
  }

  static async loadConnection(alias: string): Promise<SavedConnection | undefined> {
    try {
      const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
      query.set(asset.Tag.ALIAS, toBytes(alias));
      query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
      const results: Array<asset.AssetMap> = await asset.query(query);
      if (results.length === 0) {
        return undefined;
      }
      return CredentialStore.parseEntry(results[0], alias);
    } catch (error) {
      return undefined;
    }
  }

  static async listConnections(): Promise<SavedConnection[]> {
    const result: SavedConnection[] = [];
    const aliases: string[] = [];
    try {
      const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
      query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ATTRIBUTES);
      const results: Array<asset.AssetMap> = await asset.query(query);
      for (const entry of results) {
        const alias: string = CredentialStore.readString(entry, asset.Tag.ALIAS);
        if (alias.length > 0) {
          aliases.push(alias);
        }
      }
    } catch (error) {
      return result;
    }
    for (const alias of aliases) {
      const conn: SavedConnection | undefined = await CredentialStore.loadConnection(alias);
      if (conn !== undefined) {
        result.push(conn);
      }
    }
    return result;
  }

  static async remove(alias: string): Promise<void> {
    const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    query.set(asset.Tag.ALIAS, toBytes(alias));
    try {
      await asset.remove(query);
    } catch (error) {
      // Missing asset is not an error for callers.
    }
  }

  private static parseEntry(entry: asset.AssetMap, alias: string): SavedConnection {
    const conn: SavedConnection = new SavedConnection();
    conn.alias = alias;
    const separator: number = alias.lastIndexOf(':');
    if (separator > 0) {
      conn.host = alias.substring(0, separator);
      const port: number = Number.parseInt(alias.substring(separator + 1), 10);
      conn.port = Number.isNaN(port) || port <= 0 ? 3389 : port;
    } else {
      conn.host = alias;
    }
    conn.username = CredentialStore.readString(entry, asset.Tag.DATA_LABEL_NORMAL_1);
    conn.domain = CredentialStore.readString(entry, asset.Tag.DATA_LABEL_NORMAL_2);
    conn.password = CredentialStore.readString(entry, asset.Tag.SECRET);
    CredentialStore.decodeOptions(conn,
      CredentialStore.readString(entry, asset.Tag.DATA_LABEL_NORMAL_3));
    return conn;
  }

  private static encodeOptions(conn: SavedConnection): string {
    const parts: string[] = [];
    parts.push(conn.width.toString());
    parts.push(conn.height.toString());
    parts.push(conn.colorDepth.toString());
    parts.push(conn.enableClipboard ? '1' : '0');
    parts.push(conn.enableAudio ? '1' : '0');
    parts.push(conn.enableGfx ? '1' : '0');
    parts.push(conn.enableH264 ? '1' : '0');
    parts.push(conn.enableRemoteFx ? '1' : '0');
    parts.push(conn.ignoreCertificate ? '1' : '0');
    return parts.join('|');
  }

  private static decodeOptions(conn: SavedConnection, text: string): void {
    if (text.length === 0) {
      return;
    }
    const parts: string[] = text.split('|');
    if (parts.length < 9) {
      return;
    }
    const width: number = Number.parseInt(parts[0], 10);
    const height: number = Number.parseInt(parts[1], 10);
    const depth: number = Number.parseInt(parts[2], 10);
    if (!Number.isNaN(width) && width > 0) {
      conn.width = width;
    }
    if (!Number.isNaN(height) && height > 0) {
      conn.height = height;
    }
    if (!Number.isNaN(depth) && depth > 0) {
      conn.colorDepth = depth;
    }
    conn.enableClipboard = parts[3] === '1';
    conn.enableAudio = parts[4] === '1';
    conn.enableGfx = parts[5] === '1';
    conn.enableH264 = parts[6] === '1';
    conn.enableRemoteFx = parts[7] === '1';
    conn.ignoreCertificate = parts[8] === '1';
  }

  private static readString(entry: asset.AssetMap, tag: asset.Tag): string {
    const value: asset.Value | undefined = entry.get(tag);
    if (value === undefined || !(value instanceof Uint8Array)) {
      return '';
    }
    return fromBytes(value);
  }
}
