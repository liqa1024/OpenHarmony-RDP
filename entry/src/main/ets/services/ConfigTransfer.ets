/*
 * HmRdp - configuration backup: import / export of global settings and every
 * saved connection through a user-selected file.
 *
 * Passwords are stored separately in the ASSET store (CredentialStore) and are
 * deliberately excluded from the exported file; the UI tells the user that
 * passwords must be entered again after an import.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo, picker } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { SavedConnection } from '../model/RdpModels';
import { AppSettings, SettingsStore } from './SettingsStore';
import { ConnectionStore } from './ConnectionStore';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpConfig';
const FILE_KIND: string = 'hmrdp-config';
const FILE_VERSION: number = 1;
const DEFAULT_FILE_NAME: string = 'hmrdp-config.json';
// Raised by the picker when the user closes the dialog without choosing.
const ERROR_USER_CANCEL: number = 13900042;

/** Outcome of an export / import attempt, ready to be shown in the UI. */
export class ConfigTransferResult {
  success: boolean = false;
  cancelled: boolean = false;
  message: string = '';
  connectionCount: number = 0;
}

/**
 * Connection fields written to the file. Kept separate from SavedConnection so
 * the exported JSON never contains the inherited password / gatewayPassword
 * fields and has a stable, explicit schema.
 */
class ExportedConnection {
  id: string = '';
  host: string = '';
  port: number = 3389;
  username: string = '';
  domain: string = '';
  width: number = 0;
  height: number = 0;
  scalePercent: number = 0;
  colorDepth: number = 32;
  ignoreCertificate: boolean = true;
  enableAudio: boolean = true;
  enableGfx: boolean = true;
  enableH264: boolean = true;
  enableRemoteFx: boolean = true;
  maxFps: number = 0;
  performanceFlags: number = 0;
  gatewayHost: string = '';
  gatewayPort: number = 443;
  gatewayUsername: string = '';
  gatewayDomain: string = '';
  useGlobalDisplay: boolean = true;
  useGlobalAdvanced: boolean = true;

  static fromSaved(conn: SavedConnection): ExportedConnection {
    const item: ExportedConnection = new ExportedConnection();
    item.id = conn.id;
    item.host = conn.host;
    item.port = conn.port;
    item.username = conn.username;
    item.domain = conn.domain;
    item.width = conn.width;
    item.height = conn.height;
    item.scalePercent = conn.scalePercent;
    item.colorDepth = conn.colorDepth;
    item.ignoreCertificate = conn.ignoreCertificate;
    item.enableAudio = conn.enableAudio;
    item.enableGfx = conn.enableGfx;
    item.enableH264 = conn.enableH264;
    item.enableRemoteFx = conn.enableRemoteFx;
    item.maxFps = conn.maxFps;
    item.performanceFlags = conn.performanceFlags;
    item.gatewayHost = conn.gatewayHost;
    item.gatewayPort = conn.gatewayPort;
    item.gatewayUsername = conn.gatewayUsername;
    item.gatewayDomain = conn.gatewayDomain;
    item.useGlobalDisplay = conn.useGlobalDisplay;
    item.useGlobalAdvanced = conn.useGlobalAdvanced;
    return item;
  }

  static fromRecord(rec: Record<string, Object>): ExportedConnection {
    const item: ExportedConnection = new ExportedConnection();
    item.id = ConfigTransfer.readString(rec, 'id', '');
    item.host = ConfigTransfer.readString(rec, 'host', '');
    item.port = ConfigTransfer.readNumber(rec, 'port', 3389);
    item.username = ConfigTransfer.readString(rec, 'username', '');
    item.domain = ConfigTransfer.readString(rec, 'domain', '');
    item.width = ConfigTransfer.readNumber(rec, 'width', 0);
    item.height = ConfigTransfer.readNumber(rec, 'height', 0);
    item.scalePercent = ConfigTransfer.readNumber(rec, 'scalePercent', 0);
    item.colorDepth = ConfigTransfer.readNumber(rec, 'colorDepth', 32);
    item.ignoreCertificate = ConfigTransfer.readBoolean(rec, 'ignoreCertificate', true);
    item.enableAudio = ConfigTransfer.readBoolean(rec, 'enableAudio', true);
    item.enableGfx = ConfigTransfer.readBoolean(rec, 'enableGfx', true);
    item.enableH264 = ConfigTransfer.readBoolean(rec, 'enableH264', true);
    item.enableRemoteFx = ConfigTransfer.readBoolean(rec, 'enableRemoteFx', true);
    item.maxFps = ConfigTransfer.readNumber(rec, 'maxFps', 0);
    item.performanceFlags = ConfigTransfer.readNumber(rec, 'performanceFlags', 0);
    item.gatewayHost = ConfigTransfer.readString(rec, 'gatewayHost', '');
    item.gatewayPort = ConfigTransfer.readNumber(rec, 'gatewayPort', 443);
    item.gatewayUsername = ConfigTransfer.readString(rec, 'gatewayUsername', '');
    item.gatewayDomain = ConfigTransfer.readString(rec, 'gatewayDomain', '');
    item.useGlobalDisplay = ConfigTransfer.readBoolean(rec, 'useGlobalDisplay', true);
    item.useGlobalAdvanced = ConfigTransfer.readBoolean(rec, 'useGlobalAdvanced', true);
    return item;
  }

  toSaved(): SavedConnection {
    const conn: SavedConnection = new SavedConnection();
    conn.id = this.id.length > 0 ? this.id : util.generateRandomUUID(true);
    conn.host = this.host;
    conn.port = this.port > 0 ? this.port : 3389;
    conn.username = this.username;
    conn.domain = this.domain;
    conn.width = this.width;
    conn.height = this.height;
    conn.scalePercent = this.scalePercent;
    conn.colorDepth = this.colorDepth;
    conn.ignoreCertificate = this.ignoreCertificate;
    conn.enableAudio = this.enableAudio;
    conn.enableGfx = this.enableGfx;
    conn.enableH264 = this.enableH264;
    conn.enableRemoteFx = this.enableRemoteFx;
    conn.maxFps = this.maxFps;
    conn.performanceFlags = this.performanceFlags;
    conn.gatewayHost = this.gatewayHost;
    conn.gatewayPort = this.gatewayPort > 0 ? this.gatewayPort : 443;
    conn.gatewayUsername = this.gatewayUsername;
    conn.gatewayDomain = this.gatewayDomain;
    conn.useGlobalDisplay = this.useGlobalDisplay;
    conn.useGlobalAdvanced = this.useGlobalAdvanced;
    conn.updatedAt = Date.now();
    return conn;
  }
}

/** Top-level structure of the exported JSON file. */
class ConfigFile {
  kind: string = FILE_KIND;
  version: number = FILE_VERSION;
  exportedAt: number = 0;
  settings: AppSettings = new AppSettings();
  connections: ExportedConnection[] = [];
}

export class ConfigTransfer {
  static async exportToFile(context: common.UIAbilityContext): Promise<ConfigTransferResult> {
    const result: ConfigTransferResult = new ConfigTransferResult();
    try {
      const config: ConfigFile = new ConfigFile();
      config.exportedAt = Date.now();
      config.settings = SettingsStore.load();
      const saved: SavedConnection[] = ConnectionStore.list();
      const exported: ExportedConnection[] = [];
      for (const conn of saved) {
        exported.push(ExportedConnection.fromSaved(conn));
      }
      config.connections = exported;
      const text: string = JSON.stringify(config);

      const options: picker.DocumentSaveOptions = new picker.DocumentSaveOptions();
      options.newFileNames = [DEFAULT_FILE_NAME];
      const documentPicker: picker.DocumentViewPicker = new picker.DocumentViewPicker(context);
      const uris: string[] = await documentPicker.save(options);
      if (uris.length === 0) {
        result.cancelled = true;
        result.message = '已取消导出';
        return result;
      }
      if (!ConfigTransfer.writeText(uris[0], text)) {
        result.message = '导出失败：无法写入文件';
        return result;
      }
      result.success = true;
      result.connectionCount = exported.length;
      result.message = `已导出全局设置和 ${exported.length} 个连接（不含密码）`;
    } catch (error) {
      const err: BusinessError = error as BusinessError;
      hilog.error(DOMAIN, TAG, 'export failed: %{public}s', JSON.stringify(error));
      if (err.code === ERROR_USER_CANCEL) {
        result.cancelled = true;
        result.message = '已取消导出';
      } else {
        result.message = `导出失败（${err.code}）`;
      }
    }
    return result;
  }

  static async importFromFile(context: common.UIAbilityContext): Promise<ConfigTransferResult> {
    const result: ConfigTransferResult = new ConfigTransferResult();
    try {
      const options: picker.DocumentSelectOptions = new picker.DocumentSelectOptions();
      options.maxSelectNumber = 1;
      options.fileSuffixFilters = ['配置文件|.json'];
      const documentPicker: picker.DocumentViewPicker = new picker.DocumentViewPicker(context);
      const uris: string[] = await documentPicker.select(options);
      if (uris.length === 0) {
        result.cancelled = true;
        result.message = '已取消导入';
        return result;
      }
      const text: string = ConfigTransfer.readText(uris[0]);
      if (text.length === 0) {
        result.message = '配置文件为空或无法读取';
        return result;
      }
      const root: Record<string, Object> = JSON.parse(text) as Record<string, Object>;
      if (ConfigTransfer.readString(root, 'kind', '') !== FILE_KIND) {
        result.message = '不是有效的 RDP 配置文件';
        return result;
      }

      const settings: AppSettings = ConfigTransfer.readSettings(root['settings']);
      SettingsStore.save(settings);

      let count: number = 0;
      const raw: Object | undefined = root['connections'];
      if (raw !== undefined && raw !== null && Array.isArray(raw)) {
        const items: Array<Object> = raw as Array<Object>;
        for (const item of items) {
          const rec: Record<string, Object> = item as Record<string, Object>;
          const conn: SavedConnection = ExportedConnection.fromRecord(rec).toSaved();
          if (conn.host.length === 0) {
            continue;
          }
          ConnectionStore.save(conn);
          count++;
        }
      }
      result.success = true;
      result.connectionCount = count;
      result.message = `已导入全局设置和 ${count} 个连接，密码需重新输入`;
    } catch (error) {
      const err: BusinessError = error as BusinessError;
      hilog.error(DOMAIN, TAG, 'import failed: %{public}s', JSON.stringify(error));
      if (err.code === ERROR_USER_CANCEL) {
        result.cancelled = true;
        result.message = '已取消导入';
      } else {
        result.message = '导入失败：文件格式错误或无法读取';
      }
    }
    return result;
  }

  private static writeText(uri: string, text: string): boolean {
    let file: fileIo.File | undefined = undefined;
    try {
      file = fileIo.openSync(uri, fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.TRUNC);
      fileIo.writeSync(file.fd, text);
      return true;
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'write config failed: %{public}s', JSON.stringify(error));
      return false;
    } finally {
      ConfigTransfer.closeQuietly(file);
    }
  }

  private static readText(uri: string): string {
    let file: fileIo.File | undefined = undefined;
    try {
      file = fileIo.openSync(uri, fileIo.OpenMode.READ_ONLY);
      const size: number = fileIo.statSync(file.fd).size;
      if (size <= 0) {
        return '';
      }
      const buffer: ArrayBuffer = new ArrayBuffer(size);
      fileIo.readSync(file.fd, buffer);
      return util.TextDecoder.create('utf-8').decodeToString(new Uint8Array(buffer));
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'read config failed: %{public}s', JSON.stringify(error));
      return '';
    } finally {
      ConfigTransfer.closeQuietly(file);
    }
  }

  private static closeQuietly(file: fileIo.File | undefined): void {
    if (file === undefined) {
      return;
    }
    try {
      fileIo.closeSync(file);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'close config file failed: %{public}s', JSON.stringify(error));
    }
  }

  private static readSettings(raw: Object | undefined): AppSettings {
    const settings: AppSettings = new AppSettings();
    if (raw === undefined || raw === null) {
      return settings;
    }
    const rec: Record<string, Object> = raw as Record<string, Object>;
    settings.toolbarHoverDelay =
      ConfigTransfer.readNumber(rec, 'toolbarHoverDelay', settings.toolbarHoverDelay);
    settings.toolbarHideDelay =
      ConfigTransfer.readNumber(rec, 'toolbarHideDelay', settings.toolbarHideDelay);
    settings.touchpadScrollSpeed =
      ConfigTransfer.readNumber(rec, 'touchpadScrollSpeed', settings.touchpadScrollSpeed);
    settings.pinchZoomSpeed =
      ConfigTransfer.readNumber(rec, 'pinchZoomSpeed', settings.pinchZoomSpeed);
    settings.pinchAsTouch =
      ConfigTransfer.readBoolean(rec, 'pinchAsTouch', settings.pinchAsTouch);
    settings.pinchTouchAngle =
      ConfigTransfer.readNumber(rec, 'pinchTouchAngle', settings.pinchTouchAngle);
    settings.pinchTouchDistance =
      ConfigTransfer.readNumber(rec, 'pinchTouchDistance', settings.pinchTouchDistance);
    settings.touchHighRate =
      ConfigTransfer.readBoolean(rec, 'touchHighRate', settings.touchHighRate);
    settings.useRdpCursor =
      ConfigTransfer.readBoolean(rec, 'useRdpCursor', settings.useRdpCursor);
    settings.reconnectOnForeground =
      ConfigTransfer.readBoolean(rec, 'reconnectOnForeground', settings.reconnectOnForeground);
    settings.mainWindowSizeEnabled =
      ConfigTransfer.readBoolean(rec, 'mainWindowSizeEnabled', settings.mainWindowSizeEnabled);
    settings.mainWindowWidth =
      ConfigTransfer.readNumber(rec, 'mainWindowWidth', settings.mainWindowWidth);
    settings.mainWindowHeight =
      ConfigTransfer.readNumber(rec, 'mainWindowHeight', settings.mainWindowHeight);
    settings.sessionWindowSizeEnabled =
      ConfigTransfer.readBoolean(rec, 'sessionWindowSizeEnabled', settings.sessionWindowSizeEnabled);
    settings.sessionWindowWidth =
      ConfigTransfer.readNumber(rec, 'sessionWindowWidth', settings.sessionWindowWidth);
    settings.sessionWindowHeight =
      ConfigTransfer.readNumber(rec, 'sessionWindowHeight', settings.sessionWindowHeight);
    settings.sessionFullscreen =
      ConfigTransfer.readBoolean(rec, 'sessionFullscreen', settings.sessionFullscreen);
    settings.autoHideMainWindow =
      ConfigTransfer.readBoolean(rec, 'autoHideMainWindow', settings.autoHideMainWindow);
    settings.resolutionAuto =
      ConfigTransfer.readBoolean(rec, 'resolutionAuto', settings.resolutionAuto);
    settings.manualWidth =
      ConfigTransfer.readNumber(rec, 'manualWidth', settings.manualWidth);
    settings.manualHeight =
      ConfigTransfer.readNumber(rec, 'manualHeight', settings.manualHeight);
    settings.scaleAuto =
      ConfigTransfer.readBoolean(rec, 'scaleAuto', settings.scaleAuto);
    settings.manualScalePercent =
      ConfigTransfer.readNumber(rec, 'manualScalePercent', settings.manualScalePercent);
    settings.enableAudio =
      ConfigTransfer.readBoolean(rec, 'enableAudio', settings.enableAudio);
    settings.enableGfx =
      ConfigTransfer.readBoolean(rec, 'enableGfx', settings.enableGfx);
    settings.enableH264 =
      ConfigTransfer.readBoolean(rec, 'enableH264', settings.enableH264);
    settings.ignoreCertificate =
      ConfigTransfer.readBoolean(rec, 'ignoreCertificate', settings.ignoreCertificate);
    settings.maxFps =
      ConfigTransfer.readNumber(rec, 'maxFps', settings.maxFps);
    return settings;
  }

  static readNumber(rec: Record<string, Object>, key: string, fallback: number): number {
    const value: Object | undefined = rec[key];
    if (typeof value === 'number') {
      return value as number;
    }
    return fallback;
  }

  static readBoolean(rec: Record<string, Object>, key: string, fallback: boolean): boolean {
    const value: Object | undefined = rec[key];
    if (typeof value === 'boolean') {
      return value as boolean;
    }
    return fallback;
  }

  static readString(rec: Record<string, Object>, key: string, fallback: string): string {
    const value: Object | undefined = rec[key];
    if (typeof value === 'string') {
      return value as string;
    }
    return fallback;
  }
}
