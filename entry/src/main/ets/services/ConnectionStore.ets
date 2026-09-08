/*
 * HmRdp - saved connection metadata (non-sensitive) backed by preferences.
 *
 * Passwords are never stored here. Each connection is identified by a stable
 * UUID so renaming host/port does not invalidate the record; the password is
 * kept separately in CredentialStore (ASSET) and only after a verified login.
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { SavedConnection } from '../model/RdpModels';

const STORE_NAME: string = 'hmrdp_connections';
const IDS_KEY: string = 'ids';
const ENTRY_PREFIX: string = 'conn_';
const FIELD_SEPARATOR: string = '|';
// Records written before the per-connection display override existed only had
// 19 fields; they are still accepted and default to the global display.
const LEGACY_FIELD_COUNT: number = 19;

export class ConnectionStore {
  private static store: preferences.Preferences | undefined = undefined;

  static init(context: common.Context): void {
    if (ConnectionStore.store === undefined) {
      ConnectionStore.store = preferences.getPreferencesSync(context, { name: STORE_NAME });
    }
  }

  static list(): SavedConnection[] {
    const result: SavedConnection[] = [];
    const store: preferences.Preferences | undefined = ConnectionStore.store;
    if (store === undefined) {
      return result;
    }
    const ids: string[] = ConnectionStore.readIds(store);
    for (const id of ids) {
      const conn: SavedConnection | undefined = ConnectionStore.get(id);
      if (conn !== undefined) {
        result.push(conn);
      }
    }
    return result;
  }

  static get(id: string): SavedConnection | undefined {
    const store: preferences.Preferences | undefined = ConnectionStore.store;
    if (store === undefined || id.length === 0) {
      return undefined;
    }
    const text: string = ConnectionStore.readString(store, ENTRY_PREFIX + id, '');
    if (text.length === 0) {
      return undefined;
    }
    return ConnectionStore.deserialize(id, text);
  }

  static save(conn: SavedConnection): void {
    const store: preferences.Preferences | undefined = ConnectionStore.store;
    if (store === undefined || conn.id.length === 0) {
      return;
    }
    store.putSync(ENTRY_PREFIX + conn.id, ConnectionStore.serialize(conn));
    const ids: string[] = ConnectionStore.readIds(store);
    let known: boolean = false;
    for (const existing of ids) {
      if (existing === conn.id) {
        known = true;
        break;
      }
    }
    if (!known) {
      ids.push(conn.id);
      store.putSync(IDS_KEY, ids.join(','));
    }
    store.flush();
  }

  static remove(id: string): void {
    const store: preferences.Preferences | undefined = ConnectionStore.store;
    if (store === undefined || id.length === 0) {
      return;
    }
    store.deleteSync(ENTRY_PREFIX + id);
    const ids: string[] = ConnectionStore.readIds(store);
    const kept: string[] = [];
    for (const existing of ids) {
      if (existing !== id) {
        kept.push(existing);
      }
    }
    store.putSync(IDS_KEY, kept.join(','));
    store.flush();
  }

  private static readIds(store: preferences.Preferences): string[] {
    const text: string = ConnectionStore.readString(store, IDS_KEY, '');
    const ids: string[] = [];
    if (text.length === 0) {
      return ids;
    }
    for (const part of text.split(',')) {
      if (part.length > 0) {
        ids.push(part);
      }
    }
    return ids;
  }

  private static serialize(conn: SavedConnection): string {
    const parts: string[] = [];
    parts.push(ConnectionStore.encode(conn.host));
    parts.push(conn.port.toString());
    parts.push(ConnectionStore.encode(conn.username));
    parts.push(ConnectionStore.encode(conn.domain));
    parts.push(conn.width.toString());
    parts.push(conn.height.toString());
    parts.push(conn.colorDepth.toString());
    parts.push(conn.ignoreCertificate ? '1' : '0');
    parts.push(conn.enableClipboard ? '1' : '0');
    parts.push(conn.enableAudio ? '1' : '0');
    parts.push(conn.enableGfx ? '1' : '0');
    parts.push(conn.enableH264 ? '1' : '0');
    parts.push(conn.enableRemoteFx ? '1' : '0');
    parts.push(conn.performanceFlags.toString());
    parts.push(ConnectionStore.encode(conn.gatewayHost));
    parts.push(conn.gatewayPort.toString());
    parts.push(ConnectionStore.encode(conn.gatewayUsername));
    parts.push(ConnectionStore.encode(conn.gatewayDomain));
    parts.push(conn.updatedAt.toString());
    parts.push(conn.useGlobalDisplay ? '1' : '0');
    parts.push(conn.scalePercent.toString());
    return parts.join(FIELD_SEPARATOR);
  }

  private static deserialize(id: string, text: string): SavedConnection | undefined {
    const parts: string[] = text.split(FIELD_SEPARATOR);
    if (parts.length < LEGACY_FIELD_COUNT) {
      return undefined;
    }
    const conn: SavedConnection = new SavedConnection();
    conn.id = id;
    conn.host = ConnectionStore.decode(parts[0]);
    conn.port = ConnectionStore.parsePositive(parts[1], 3389);
    conn.username = ConnectionStore.decode(parts[2]);
    conn.domain = ConnectionStore.decode(parts[3]);
    conn.width = ConnectionStore.parsePositive(parts[4], 1920);
    conn.height = ConnectionStore.parsePositive(parts[5], 1080);
    conn.colorDepth = ConnectionStore.parsePositive(parts[6], 32);
    conn.ignoreCertificate = parts[7] === '1';
    conn.enableClipboard = parts[8] === '1';
    conn.enableAudio = parts[9] === '1';
    conn.enableGfx = parts[10] === '1';
    conn.enableH264 = parts[11] === '1';
    conn.enableRemoteFx = parts[12] === '1';
    conn.performanceFlags = ConnectionStore.parseNumber(parts[13], 0);
    conn.gatewayHost = ConnectionStore.decode(parts[14]);
    conn.gatewayPort = ConnectionStore.parsePositive(parts[15], 443);
    conn.gatewayUsername = ConnectionStore.decode(parts[16]);
    conn.gatewayDomain = ConnectionStore.decode(parts[17]);
    conn.updatedAt = ConnectionStore.parseNumber(parts[18], 0);
    conn.useGlobalDisplay = parts.length > 19 ? parts[19] === '1' : true;
    conn.scalePercent = parts.length > 20 ? ConnectionStore.parseNumber(parts[20], 0) : 0;
    return conn;
  }

  private static encode(text: string): string {
    try {
      return encodeURIComponent(text);
    } catch (error) {
      return text;
    }
  }

  private static decode(text: string): string {
    try {
      return decodeURIComponent(text);
    } catch (error) {
      return text;
    }
  }

  private static parseNumber(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    return Number.isNaN(value) ? fallback : value;
  }

  private static parsePositive(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    return Number.isNaN(value) || value <= 0 ? fallback : value;
  }

  private static readString(store: preferences.Preferences, key: string,
    fallback: string): string {
    const value: preferences.ValueType = store.getSync(key, fallback);
    if (typeof value === 'string') {
      return value as string;
    }
    return fallback;
  }
}
