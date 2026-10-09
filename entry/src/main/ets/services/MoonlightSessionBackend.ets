import {
  MoonlightEventExtra, MoonlightSessionConfig, bufferRelease,
  moonlightKey, moonlightMouseButton, moonlightMouseMove,
  moonlightMousePosition, moonlightScroll, moonlightSessionActive, moonlightSessionClearSurface,
  moonlightSessionSetSurface, moonlightSessionStart, moonlightSessionStop, moonlightSetSessionListener
} from 'libhmrdp.so';
import { ML_CURSOR_HIDE_HOST, SettingsStore } from './SettingsStore';
import { RemoteCursorUpdate } from './RemoteCursor';
import { PendingConnection, PROTOCOL_MOONLIGHT, SavedConnection } from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { ConnectionStore } from './ConnectionStore';
import { MoonlightConnectResult, MoonlightConnector, MoonlightHostPolicy,
  MoonlightPinProvider } from './MoonlightConnector';
import { MoonlightHostState } from './MoonlightHostState';
import { PairResult } from '../moonlight/MoonlightModels';
import { SessionBackend, SessionSignalListener } from './SessionBackend';

export const ML_EVENT_STARTED: number = 0;
export const ML_EVENT_TERMINATED: number = 1;
export const ML_EVENT_STAGE_STARTING: number = 2;
export const ML_EVENT_STAGE_COMPLETE: number = 3;
export const ML_EVENT_STAGE_FAILED: number = 4;
export const ML_EVENT_CURSOR_SHAPE: number = 5;

export type MlCursorListener = (update: RemoteCursorUpdate) => void;

export class MoonlightSessionBackend implements SessionBackend {
  private listener: SessionSignalListener | undefined = undefined;
  private cursorListener: MlCursorListener | undefined = undefined;
  private request: PendingConnection;
  private config: MoonlightSessionConfig | undefined = undefined;
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

  clearCursorListener(): void {
    this.cursorListener = undefined;
  }

  private onNativeEvent(event: number, value: number, text: string,
    extra: MoonlightEventExtra): void {
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
    this.emit(event, value, text);
  }

  protocol(): number {
    return PROTOCOL_MOONLIGHT;
  }

  requiresPrepare(): boolean {
    return true;
  }

  async prepare(): Promise<boolean> {
    const request: PendingConnection = this.request;
    const conn = request.savedConnection;
    if (conn === undefined || request.filesDir.length === 0) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, '缺少连接参数');
      return false;
    }
    const pinProvider: MoonlightPinProvider =
      request.moonlightPinProvider !== undefined
        ? request.moonlightPinProvider
        : (pin: string, pairing: Promise<PairResult>): Promise<boolean> => Promise.resolve(false);
    const policy: MoonlightHostPolicy = new MoonlightHostPolicy();
    policy.expectedId = conn.mlHostId;
    const connections: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < connections.length; i++) {
      const other: SavedConnection = connections[i];
      if (other.isMoonlight() && other.id !== conn.id && other.mlHostId.length > 0) {
        policy.knownIds.push(other.mlHostId);
      }
    }
    const result: MoonlightConnectResult = await MoonlightConnector.prepare(request.filesDir,
      conn.host, conn.port, request.stream, request.moonlightAppId,
      request.stream.width, request.stream.height, pinProvider, policy);
    if (!result.ok) {
      MoonlightHostState.refresh(conn, request.filesDir);
      this.emit(ML_EVENT_STAGE_FAILED, 0, result.error);
      return false;
    }
    this.config = result.config;
    request.moonlightConfig = result.config;
    request.moonlightHttpsPort = result.httpsPort;
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
    config.localCursorMode = SettingsStore.load().mlCursorMode === ML_CURSOR_HIDE_HOST;
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

  mouseMove(deltaX: number, deltaY: number): void {
    moonlightMouseMove(deltaX, deltaY);
  }

  mousePosition(x: number, y: number, referenceWidth: number, referenceHeight: number): void {
    moonlightMousePosition(x, y, referenceWidth, referenceHeight);
  }

  mouseButton(action: number, button: number): void {
    moonlightMouseButton(action, button);
  }

  // Moonlight takes Windows virtual-key codes, not PS/2 scancodes.
  keyVirtual(vk: number, down: boolean, modifiers: number): void {
    moonlightKey(vk, down ? 1 : 0, modifiers);
  }

  scroll(clicks: number): void {
    moonlightScroll(clicks);
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
