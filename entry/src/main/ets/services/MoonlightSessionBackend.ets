import {
  MoonlightEventExtra, MoonlightSessionConfig, bufferRead, bufferRelease,
  moonlightHostFeatures, moonlightSendClipboardData, moonlightSessionActive,
  moonlightSessionClearSurface, moonlightSessionSetSurface, moonlightSessionStart,
  moonlightSessionStop, moonlightSetSessionListener, moonlightSetShortcutPassthrough
} from 'libhmrdp.so';
import { ML_CURSOR_HIDE_HOST, SettingsStore } from './SettingsStore';
import { DeviceCapabilities } from './DeviceCapabilities';
import { RemoteCursorUpdate } from './RemoteCursor';
import { MoonlightConnection, PendingConnection, SavedConnection }
  from '../model/Connections';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { ConnectionStore } from './ConnectionStore';
import { MoonlightConnectResult, MoonlightConnector, MoonlightHostPolicy,
  MoonlightKnownHost, MoonlightPinProvider } from './MoonlightConnector';
import { MoonlightHostState } from './MoonlightHostState';
import { PairResult } from '../moonlight/MoonlightModels';
import { SessionBackend, SessionSignalListener } from './SessionBackend';

export const ML_EVENT_STARTED: number = 0;
export const ML_EVENT_TERMINATED: number = 1;
export const ML_EVENT_STAGE_STARTING: number = 2;
export const ML_EVENT_STAGE_COMPLETE: number = 3;
export const ML_EVENT_STAGE_FAILED: number = 4;
export const ML_EVENT_CURSOR_SHAPE: number = 5;
// Shortcut passthrough: a system-intercepted key, down/up carry the keyCode in value.
export const ML_EVENT_KEY_INTERCEPT_DOWN: number = 6;
export const ML_EVENT_KEY_INTERCEPT_UP: number = 7;
// Foundation-sunshine clipboard sync frame (opaque v1 wire bytes).
export const ML_EVENT_CLIPBOARD: number = 8;

export const ML_HOST_FEATURE_TOUCH: number = 0x01;
export const ML_HOST_FEATURE_TOUCHPAD_FRAME: number = 0x20;
// foundation-sunshine clipboard capabilities (advertised only with its GUI agent alive).
export const ML_HOST_FEATURE_CLIPBOARD_TEXT: number = 0x04;
export const ML_HOST_FEATURE_CLIPBOARD_IMAGE: number = 0x08;

export type MlKeyInterceptListener = (keyCode: number, down: boolean) => void;

export type MlCursorListener = (update: RemoteCursorUpdate) => void;

export type MlClipboardListener = (bytes: ArrayBuffer) => void;

export class MoonlightSessionBackend implements SessionBackend {
  private listener: SessionSignalListener | undefined = undefined;
  private cursorListener: MlCursorListener | undefined = undefined;
  private keyInterceptListener: MlKeyInterceptListener | undefined = undefined;
  private clipboardListener: MlClipboardListener | undefined = undefined;
  private request: PendingConnection;
  private config: MoonlightSessionConfig | undefined = undefined;
  // Scale (percent) in effect on the host virtual display for this session; 0 = unknown.
  private hostScalePercent: number = 0;
  private windowBound: boolean = false;

  constructor(request: PendingConnection) {
    this.request = request;
    // One process-global session at a time (moonlight-common-c is single connection), so the listener is registered here rather than per stream.
    moonlightSetSessionListener((event: number, value: number, text: string,
      extra: MoonlightEventExtra): void => {
      this.onNativeEvent(event, value, text, extra);
    });
  }

  setCursorListener(listener: MlCursorListener): void {
    this.cursorListener = listener;
  }

  hostDisplayScalePercent(): number {
    return this.hostScalePercent;
  }

  clearCursorListener(): void {
    this.cursorListener = undefined;
  }

  setKeyInterceptListener(listener: MlKeyInterceptListener): void {
    this.keyInterceptListener = listener;
  }

  clearKeyInterceptListener(): void {
    this.keyInterceptListener = undefined;
  }

  setClipboardListener(listener: MlClipboardListener): void {
    this.clipboardListener = listener;
  }

  clearClipboardListener(): void {
    this.clipboardListener = undefined;
  }

  sendClipboardData(bytes: ArrayBuffer): number {
    return moonlightSendClipboardData(bytes);
  }

  // Returns whether the requested state is in effect (ACL may refuse).
  setShortcutPassthrough(enabled: boolean): boolean {
    return moonlightSetShortcutPassthrough(enabled);
  }

  private onNativeEvent(event: number, value: number, text: string,
    extra: MoonlightEventExtra): void {
    if (event === ML_EVENT_KEY_INTERCEPT_DOWN || event === ML_EVENT_KEY_INTERCEPT_UP) {
      const listener: MlKeyInterceptListener | undefined = this.keyInterceptListener;
      if (listener !== undefined) {
        listener(value, event === ML_EVENT_KEY_INTERCEPT_DOWN);
      }
      return;
    }
    if (event === ML_EVENT_CURSOR_SHAPE) {
      const handle: number = extra.imageHandle ?? 0;
      const listener: MlCursorListener | undefined = this.cursorListener;
      if (listener === undefined) {
        if (handle > 0) {
          bufferRelease(handle);
        }
        return;
      }
      const update: RemoteCursorUpdate = new RemoteCursorUpdate();
      update.visible = extra.visible ?? true;
      update.hasShape = extra.hasShape ?? false;
      update.shapeKey = extra.shapeId ?? 0;
      update.width = extra.width ?? 0;
      update.height = extra.height ?? 0;
      update.hotX = extra.hotX ?? 0;
      update.hotY = extra.hotY ?? 0;
      update.imageHandle = handle;
      listener(update);
      return;
    }
    if (event === ML_EVENT_CLIPBOARD) {
      const handle: number = extra.dataHandle ?? 0;
      if (handle <= 0) {
        return;
      }
      const bytes: ArrayBuffer = bufferRead(handle);
      bufferRelease(handle);
      const listener: MlClipboardListener | undefined = this.clipboardListener;
      if (listener !== undefined && bytes.byteLength > 0) {
        listener(bytes);
      }
      return;
    }
    this.emit(event, value, text);
  }

  pageRoute(): string {
    return 'pages/MoonlightSessionPage';
  }

  requiresPrepare(): boolean {
    return true;
  }

  async prepare(): Promise<boolean> {
    const request: PendingConnection = this.request;
    const saved = request.savedConnection;
    if (!(saved instanceof MoonlightConnection) || request.filesDir.length === 0) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, '缺少连接参数');
      return false;
    }
    const conn: MoonlightConnection = saved;
    const pinProvider: MoonlightPinProvider =
      request.moonlightPinProvider !== undefined
        ? request.moonlightPinProvider
        : (pin: string, pairing: Promise<PairResult>): Promise<boolean> => Promise.resolve(false);
    const policy: MoonlightHostPolicy = new MoonlightHostPolicy();
    policy.expectedId = conn.mlHostId;
    const connections: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < connections.length; i++) {
      const other: SavedConnection = connections[i];
      if (other instanceof MoonlightConnection && other.id !== conn.id
        && other.mlHostId.length > 0) {
        const known: MoonlightKnownHost = new MoonlightKnownHost();
        known.hostId = other.mlHostId;
        known.host = other.host;
        policy.known.push(known);
      }
    }
    const result: MoonlightConnectResult = await MoonlightConnector.prepare(request.filesDir,
      conn.host, conn.port, request.stream, request.moonlightAppId,
      request.stream.width, request.stream.height, pinProvider, policy,
      SettingsStore.load().enableAudio && DeviceCapabilities.audio().supported);
    if (!result.ok) {
      MoonlightHostState.refresh(conn, request.filesDir);
      this.emit(ML_EVENT_STAGE_FAILED, 0, result.error);
      return false;
    }
    this.config = result.config;
    this.hostScalePercent = result.hostScalePercent;
    request.moonlightConfig = result.config;
    request.moonlightHttpsPort = result.httpsPort;
    request.moonlightFpsNote = result.fpsNote;
    MoonlightHostState.markLaunched(conn.id, result.effectiveAppId);
    if (result.hostId.length > 0 && conn.mlHostId !== result.hostId) {
      conn.mlHostId = result.hostId;
      conn.updatedAt = Date.now();
      ConnectionStore.save(conn);
    }
    return true;
  }

  requiresWindowAtStart(): boolean {
    return true;
  }

  handleId(): number {
    return 0;
  }

  bindSurface(surfaceId: string, width: number, height: number): boolean {
    this.windowBound = moonlightSessionSetSurface(surfaceId, width, height);
    return this.windowBound;
  }

  clearSurface(): void {
    if (this.windowBound) {
      moonlightSessionClearSurface();
      this.windowBound = false;
    }
  }

  start(): number {
    const config: MoonlightSessionConfig | undefined = this.config;
    if (config === undefined) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, '尚未配置串流参数');
      return -1;
    }
    if (!this.windowBound) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, '会话窗口未就绪');
      return -1;
    }
    // HIDE_HOST is a no-op unless the host is a foundation-sunshine build.
    const settings = SettingsStore.load();
    config.localCursorMode = settings.mlCursorMode === ML_CURSOR_HIDE_HOST;
    config.lowLatencyAudio = settings.mlLowLatencyAudio;
    const error: string = moonlightSessionStart(config);
    if (error.length > 0) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, `无法启动串流（${error}）`);
      return -1;
    }
    return 1;
  }

  stop(): void {
    moonlightSessionStop();
    this.clearSurface();
    this.clearSignalListener();
  }

  active(): boolean {
    return moonlightSessionActive();
  }

  // LI_FF_* bits the host advertised; 0 until the streams are established.
  hostFeatureFlags(): number {
    return moonlightHostFeatures();
  }

  setSignalListener(listener: SessionSignalListener): void {
    this.listener = listener;
  }

  clearSignalListener(): void {
    this.listener = undefined;
  }

  pointer(flags: number, x: number, y: number): void {
  }

  touch(flags: number, finger: number, pressure: number, x: number, y: number): void {
  }

  key(scancode: number, down: boolean, extended: boolean): void {
  }

  unicode(codepoint: number, down: boolean): void {
  }

  private emit(event: number, value: number, text: string): void {
    const listener: SessionSignalListener | undefined = this.listener;
    if (listener === undefined) {
      return;
    }
    const signal: SessionSignal = new SessionSignal();
    signal.eventCode = event;
    signal.code = value;
    signal.message = text;
    if (event === ML_EVENT_STARTED) {
      signal.kind = SessionSignalKind.Connected;
    } else if (event === ML_EVENT_TERMINATED) {
      signal.kind = SessionSignalKind.Disconnected;
    } else if (event === ML_EVENT_STAGE_FAILED) {
      signal.kind = SessionSignalKind.Error;
    } else if (event === ML_EVENT_STAGE_STARTING || event === ML_EVENT_STAGE_COMPLETE) {
      signal.kind = SessionSignalKind.UiEvent;
    } else {
      signal.kind = SessionSignalKind.UiEvent;
    }
    listener(signal);
  }
}

