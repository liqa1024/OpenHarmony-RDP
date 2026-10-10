import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PROTOCOL_RDP, SavedConnection } from '../model/Connections';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpConnections';
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
    if (ConnectionStore.store !== undefined) {
      return;
    }
    try {
      ConnectionStore.store = preferences.getPreferencesSync(context, { name: STORE_NAME });
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'open connection store failed: %{public}s',
        JSON.stringify(error));
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
    try {
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
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'save connection failed: %{public}s', JSON.stringify(error));
    }
  }

  static remove(id: string): void {
    const store: preferences.Preferences | undefined = ConnectionStore.store;
    if (store === undefined || id.length === 0) {
      return;
    }
    try {
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
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'remove connection failed: %{public}s', JSON.stringify(error));
    }
  }

  static reorder(ids: string[]): void {
    const store: preferences.Preferences | undefined = ConnectionStore.store;
    if (store === undefined) {
      return;
    }
    try {
      const known: string[] = ConnectionStore.readIds(store);
      const ordered: string[] = [];
      for (const id of ids) {
        if (known.indexOf(id) >= 0 && ordered.indexOf(id) < 0) {
          ordered.push(id);
        }
      }
      // Keep any record the caller forgot so nothing silently disappears.
      for (const id of known) {
        if (ordered.indexOf(id) < 0) {
          ordered.push(id);
        }
      }
      store.putSync(IDS_KEY, ordered.join(','));
      store.flush();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'reorder connections failed: %{public}s', JSON.stringify(error));
    }
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
    return conn.toRecord().join(FIELD_SEPARATOR);
  }

  private static deserialize(id: string, text: string): SavedConnection | undefined {
    const parts: string[] = text.split(FIELD_SEPARATOR);
    if (parts.length < LEGACY_FIELD_COUNT) {
      return undefined;
    }
    const protocol: number = parts.length > 27
      ? ConnectionStore.parseNumber(parts[27], PROTOCOL_RDP) : PROTOCOL_RDP;
    const conn: SavedConnection = SavedConnection.create(protocol);
    conn.id = id;
    conn.readRecord(parts);
    return conn;
  }

  private static parseNumber(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    return Number.isNaN(value) ? fallback : value;
  }

  private static readString(store: preferences.Preferences, key: string,
    fallback: string): string {
    try {
      const value: preferences.ValueType = store.getSync(key, fallback);
      if (typeof value === 'string') {
        return value as string;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'read %{public}s failed: %{public}s', key, JSON.stringify(error));
    }
    return fallback;
  }
}
