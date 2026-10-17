import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo, picker } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { SR_BACKEND_XENGINE } from '../model/Profiles';
import {
  ConnectionExport, PROTOCOL_MOONLIGHT, PROTOCOL_RDP, SavedConnection
} from '../model/Connections';
import {
  ML_BITRATE_DEFAULT_KBPS, ML_CODEC_AUTO, ML_COLORSPACE_REC709,
  ML_RES_FOLLOW_SCREEN, ML_SCALE_AUTO
} from '../moonlight/MoonlightModels';
import { AppSettings, PINCH_MODE_TOUCH, PINCH_MODE_WHEEL, SettingsStore } from './SettingsStore';
import { ConnectionStore } from './ConnectionStore';
import { DeviceCapabilities } from './DeviceCapabilities';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpConfig';
const FILE_KIND: string = 'hmrdp-config';
const FILE_VERSION: number = 1;
const DEFAULT_FILE_NAME: string = 'hmrdp-config.json';
const ERROR_USER_CANCEL: number = 13900042;

export class ConfigTransferResult {
  success: boolean = false;
  cancelled: boolean = false;
  message: string = '';
  connectionCount: number = 0;
}


class ConfigFile {
  kind: string = FILE_KIND;
  version: number = FILE_VERSION;
  exportedAt: number = 0;
  settings: AppSettings = new AppSettings();
  connections: ConnectionExport[] = [];
}

export class ConfigTransfer {
  static parseExport(rec: Record<string, Object>): ConnectionExport {
    const item: ConnectionExport = new ConnectionExport();
    item.id = ConfigTransfer.readString(rec, 'id', '');
    item.host = ConfigTransfer.readString(rec, 'host', '');
    item.port = ConfigTransfer.readNumber(rec, 'port', 3389);
    item.name = ConfigTransfer.readString(rec, 'name', '');
    item.username = ConfigTransfer.readString(rec, 'username', '');
    item.domain = ConfigTransfer.readString(rec, 'domain', '');
    item.width = ConfigTransfer.readNumber(rec, 'width', 0);
    item.height = ConfigTransfer.readNumber(rec, 'height', 0);
    item.scalePercent = ConfigTransfer.readNumber(rec, 'scalePercent', 0);
    item.ignoreCertificate = ConfigTransfer.readBoolean(rec, 'ignoreCertificate', true);
    item.enableAudio = ConfigTransfer.readBoolean(rec, 'enableAudio', true);
    item.gatewayHost = ConfigTransfer.readString(rec, 'gatewayHost', '');
    item.gatewayPort = ConfigTransfer.readNumber(rec, 'gatewayPort', 443);
    item.gatewayUsername = ConfigTransfer.readString(rec, 'gatewayUsername', '');
    item.gatewayDomain = ConfigTransfer.readString(rec, 'gatewayDomain', '');
    item.useGlobalDisplay = ConfigTransfer.readBoolean(rec, 'useGlobalDisplay', true);
    item.useGlobalAdvanced = ConfigTransfer.readBoolean(rec, 'useGlobalAdvanced', true);
    item.useGlobalSr = ConfigTransfer.readBoolean(rec, 'useGlobalSr', true);
    item.resolutionAuto = ConfigTransfer.readBoolean(rec, 'resolutionAuto', false);
    item.scaleAuto = ConfigTransfer.readBoolean(rec, 'scaleAuto', false);
    item.srEnabled = ConfigTransfer.readBoolean(rec, 'srEnabled', false);
    item.srRatioPercent = ConfigTransfer.readNumber(rec, 'srRatioPercent', 0);
    item.srBackend = ConfigTransfer.readNumber(rec, 'srBackend', SR_BACKEND_XENGINE);
    item.protocol = ConfigTransfer.readNumber(rec, 'protocol', PROTOCOL_RDP);
    item.mlDefaultAppId = ConfigTransfer.readString(rec, 'mlDefaultAppId', '');
    item.mlDefaultAppName = ConfigTransfer.readString(rec, 'mlDefaultAppName', '');
    item.mlHostId = ConfigTransfer.readString(rec, 'mlHostId', '');
    item.mlMac = ConfigTransfer.readString(rec, 'mlMac', '');
    item.mlResMode = ConfigTransfer.readNumber(rec, 'mlResMode', ML_RES_FOLLOW_SCREEN);
    item.mlWidth = ConfigTransfer.readNumber(rec, 'mlWidth', 0);
    item.mlHeight = ConfigTransfer.readNumber(rec, 'mlHeight', 0);
    item.mlFps = ConfigTransfer.readNumber(rec, 'mlFps', 60);
    item.mlBitrateAuto = ConfigTransfer.readBoolean(rec, 'mlBitrateAuto', true);
    item.mlBitrateKbps = ConfigTransfer.readNumber(rec, 'mlBitrateKbps', ML_BITRATE_DEFAULT_KBPS);
    item.mlCodec = ConfigTransfer.readNumber(rec, 'mlCodec', ML_CODEC_AUTO);
    item.mlColorSpace = ConfigTransfer.readNumber(rec, 'mlColorSpace', ML_COLORSPACE_REC709);
    item.useGlobalStream = ConfigTransfer.readBoolean(rec, 'useGlobalStream', true);
    item.mlScaleMode = ConfigTransfer.readNumber(rec, 'mlScaleMode', ML_SCALE_AUTO);
    item.mlScalePercent = ConfigTransfer.readNumber(rec, 'mlScalePercent', 100);
    return item;
  }

  static async exportToFile(context: common.UIAbilityContext): Promise<ConfigTransferResult> {
    const result: ConfigTransferResult = new ConfigTransferResult();
    try {
      const config: ConfigFile = new ConfigFile();
      config.exportedAt = Date.now();
      config.settings = SettingsStore.load();
      const saved: SavedConnection[] = ConnectionStore.list();
      const exported: ConnectionExport[] = [];
      for (const conn of saved) {
        exported.push(conn.toExport());
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
          const conn: SavedConnection =
            SavedConnection.fromExport(ConfigTransfer.parseExport(rec));
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
    settings.toolbarTriggerWidthPercent =
      ConfigTransfer.readNumber(rec, 'toolbarTriggerWidthPercent',
        settings.toolbarTriggerWidthPercent);
    settings.toolbarWidthPercent = Math.min(Math.max(
      ConfigTransfer.readNumber(rec, 'toolbarWidthPercent', settings.toolbarWidthPercent),
      50), 100);
    settings.touchpadScrollSpeed =
      ConfigTransfer.readNumber(rec, 'touchpadScrollSpeed', settings.touchpadScrollSpeed);
    settings.scrollEngineFallback = ConfigTransfer.readBoolean(rec,
      'scrollEngineFallback', settings.scrollEngineFallback);
    settings.pinchZoomSpeed =
      ConfigTransfer.readNumber(rec, 'pinchZoomSpeed', settings.pinchZoomSpeed);
    settings.touchpadMiddleDoubleTap = ConfigTransfer.readBoolean(rec,
      'touchpadMiddleDoubleTap', settings.touchpadMiddleDoubleTap);
    // A file written before the third mode carries the old two-way boolean.
    const legacyPinchTouch: boolean = ConfigTransfer.readBoolean(rec, 'pinchAsTouch', false);
    settings.pinchMode = ConfigTransfer.readNumber(rec, 'pinchMode',
      legacyPinchTouch ? PINCH_MODE_TOUCH : PINCH_MODE_WHEEL);
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
    settings.winKeySubstitute =
      ConfigTransfer.readNumber(rec, 'winKeySubstitute', settings.winKeySubstitute);
    settings.shortcutPassthrough =
      ConfigTransfer.readBoolean(rec, 'shortcutPassthrough', settings.shortcutPassthrough);
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
      ConfigTransfer.readBoolean(rec, 'enableAudio', settings.enableAudio)
      && DeviceCapabilities.audio().supported;
    settings.ignoreCertificate =
      ConfigTransfer.readBoolean(rec, 'ignoreCertificate', settings.ignoreCertificate);
    const srEnabled: boolean = ConfigTransfer.readBoolean(rec, 'srEnabled', false);
    const srBackend: number =
      ConfigTransfer.readNumber(rec, 'srBackend', SR_BACKEND_XENGINE);
    if (srEnabled && SettingsStore.srBackendUsable(srBackend, PROTOCOL_RDP)) {
      settings.srEnabled = true;
      settings.srBackend = srBackend;
      settings.srRatioPercent =
        ConfigTransfer.readNumber(rec, 'srRatioPercent', settings.srRatioPercent);
    }
    settings.developerMode =
      ConfigTransfer.readBoolean(rec, 'developerMode', settings.developerMode);
    settings.gfxCapture = ConfigTransfer.readBoolean(rec, 'gfxCapture', settings.gfxCapture);
    settings.mlFps = ConfigTransfer.readNumber(rec, 'mlFps', settings.mlFps);
    settings.mlBitrateAuto = ConfigTransfer.readBoolean(rec, 'mlBitrateAuto', settings.mlBitrateAuto);
    settings.mlBitrateKbps =
      ConfigTransfer.readNumber(rec, 'mlBitrateKbps', settings.mlBitrateKbps);
    settings.mlCodec = ConfigTransfer.readNumber(rec, 'mlCodec', settings.mlCodec);
    settings.mlColorSpace = ConfigTransfer.readNumber(rec, 'mlColorSpace', settings.mlColorSpace);
    const mlSrEnabled: boolean = ConfigTransfer.readBoolean(rec, 'mlSrEnabled', false);
    const mlSrBackend: number =
      ConfigTransfer.readNumber(rec, 'mlSrBackend', SR_BACKEND_XENGINE);
    if (mlSrEnabled && SettingsStore.srBackendUsable(mlSrBackend, PROTOCOL_MOONLIGHT)) {
      settings.mlSrEnabled = true;
      settings.mlSrBackend = mlSrBackend;
      settings.mlSrRatioPercent =
        ConfigTransfer.readNumber(rec, 'mlSrRatioPercent', settings.mlSrRatioPercent);
    }
    settings.mlResMode = ConfigTransfer.readNumber(rec, 'mlResMode', settings.mlResMode);
    settings.mlWidth = ConfigTransfer.readNumber(rec, 'mlWidth', settings.mlWidth);
    settings.mlHeight = ConfigTransfer.readNumber(rec, 'mlHeight', settings.mlHeight);
    settings.mlScaleMode = ConfigTransfer.readNumber(rec, 'mlScaleMode', settings.mlScaleMode);
    settings.mlScalePercent =
      ConfigTransfer.readNumber(rec, 'mlScalePercent', settings.mlScalePercent);
    settings.lastNewProtocol =
      ConfigTransfer.readNumber(rec, 'lastNewProtocol', settings.lastNewProtocol);
    settings.lastSettingsTab =
      ConfigTransfer.readNumber(rec, 'lastSettingsTab', settings.lastSettingsTab);
    settings.mlTouchpadScrollSpeed =
      ConfigTransfer.readNumber(rec, 'mlTouchpadScrollSpeed', settings.mlTouchpadScrollSpeed);
    settings.mlTouchpadScrollAlwaysEngine = ConfigTransfer.readBoolean(rec,
      'mlTouchpadScrollAlwaysEngine', settings.mlTouchpadScrollAlwaysEngine);
    settings.mlPinchZoomSpeed =
      ConfigTransfer.readNumber(rec, 'mlPinchZoomSpeed', settings.mlPinchZoomSpeed);
    settings.mlTouchpadMiddleDoubleTap = ConfigTransfer.readBoolean(rec,
      'mlTouchpadMiddleDoubleTap', settings.mlTouchpadMiddleDoubleTap);
    settings.mlPinchMode = ConfigTransfer.readNumber(rec, 'mlPinchMode', settings.mlPinchMode);
    settings.mlPinchTouchAngle =
      ConfigTransfer.readNumber(rec, 'mlPinchTouchAngle', settings.mlPinchTouchAngle);
    settings.mlPinchTouchDistance =
      ConfigTransfer.readNumber(rec, 'mlPinchTouchDistance', settings.mlPinchTouchDistance);
    settings.mlCursorMode = ConfigTransfer.readNumber(rec, 'mlCursorMode', settings.mlCursorMode);
    settings.mlRelativeMouse = ConfigTransfer.readBoolean(rec, 'mlRelativeMouse',
      settings.mlRelativeMouse);
    settings.mlRelativeSpeed = ConfigTransfer.readNumber(rec, 'mlRelativeSpeed',
      settings.mlRelativeSpeed);
    // No output device ⇒ no playback to make low-latency (like enableAudio).
    settings.mlLowLatencyAudio = ConfigTransfer.readBoolean(rec, 'mlLowLatencyAudio',
      settings.mlLowLatencyAudio) && DeviceCapabilities.audio().supported;
    settings.mlReconnectOnForeground = ConfigTransfer.readBoolean(rec,
      'mlReconnectOnForeground', settings.mlReconnectOnForeground);
    settings.mlWinKeySubstitute =
      ConfigTransfer.readNumber(rec, 'mlWinKeySubstitute', settings.mlWinKeySubstitute);
    settings.mlShortcutPassthrough = ConfigTransfer.readBoolean(rec,
      'mlShortcutPassthrough', settings.mlShortcutPassthrough);
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
