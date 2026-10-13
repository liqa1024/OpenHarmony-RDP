import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ConnectOrigin, PendingConnection, SessionRequests } from '../model/Connections';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { CredentialStore } from './CredentialStore';
import { SessionBackend, SessionBackendRegistry } from './SessionBackend';
import { SessionBackendFactory } from './SessionBackendFactory';
import { WindowController } from './WindowController';

const DOMAIN: number = 0xD003;
const TAG: string = 'HmRdpSessionManager';
const CONNECT_TIMEOUT_MS: number = 30000;
const MIN_ATTEMPT_INTERVAL_MS: number = 1500;

export enum ConnectionState {
  Idle = 0,
  Connecting = 1,
  Connected = 2,
  Disconnecting = 3,
  Failed = 4
}

export type SessionStateListener = (connectionId: string, state: ConnectionState,
  message: string, origin: ConnectOrigin) => void;
export type SessionWindowEventListener = (signal: SessionSignal) => void;
export type SessionWindowStateListener = (state: ConnectionState, message: string) => void;
export type SessionCloseListener = () => void;

class SessionEntry {
  connectionId: string = '';
  sessionKey: string = '';
  state: ConnectionState = ConnectionState.Idle;
  message: string = '';
  backend: SessionBackend | undefined = undefined;
  request: PendingConnection = new PendingConnection();
  context: common.UIAbilityContext | undefined = undefined;
  epoch: number = 0;
  windowOpen: boolean = false;
  timeoutId: number = -1;
}

export class SessionManager {
  private static entries: Map<string, SessionEntry> = new Map<string, SessionEntry>();
  private static byKey: Map<string, string> = new Map<string, string>();
  private static stateListeners: SessionStateListener[] = [];
  private static lastAttemptAt: Map<string, number> = new Map<string, number>();
  private static epochCounter: number = 0;
  private static windowEventListeners: Map<string, SessionWindowEventListener> =
    new Map<string, SessionWindowEventListener>();
  private static windowStateListeners: Map<string, SessionWindowStateListener> =
    new Map<string, SessionWindowStateListener>();
  private static closeListeners: Map<string, SessionCloseListener> =
    new Map<string, SessionCloseListener>();
  private static windowKeys: Set<string> = new Set<string>();

  static stateOf(connectionId: string): ConnectionState {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    return entry === undefined ? ConnectionState.Idle : entry.state;
  }

  static messageOf(connectionId: string): string {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    return entry === undefined ? '' : entry.message;
  }

  static isConnected(connectionId: string): boolean {
    return SessionManager.stateOf(connectionId) === ConnectionState.Connected;
  }

  static subscribe(listener: SessionStateListener): void {
    SessionManager.stateListeners.push(listener);
  }

  static unsubscribe(listener: SessionStateListener): void {
    const index: number = SessionManager.stateListeners.indexOf(listener);
    if (index >= 0) {
      SessionManager.stateListeners.splice(index, 1);
    }
  }

  static clearStateListeners(): void {
    SessionManager.stateListeners = [];
  }

  static connect(request: PendingConnection, context: common.UIAbilityContext): void {
    const connectionId: string = request.connectionId;
    const existing: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    const state: ConnectionState = existing === undefined
      ? ConnectionState.Idle : existing.state;
    if (!SessionManager.acceptAttempt(connectionId, state)) {
      return;
    }
    SessionManager.cancelOtherAttempts(connectionId);
    const backend: SessionBackend | undefined = SessionBackendFactory.create(request);
    if (backend === undefined) {
      SessionManager.notify(connectionId, ConnectionState.Failed,
        '该协议的会话将在后续版本接入', request.origin);
      return;
    }
    if (existing !== undefined) {
      if (existing.backend !== undefined) {
        existing.backend.stop();
      }
      SessionManager.cleanup(existing);
    }
    const sessionKey: string = SessionRequests.newKey();
    SessionRequests.put(sessionKey, request);
    SessionBackendRegistry.register(sessionKey, backend);
    const entry: SessionEntry = new SessionEntry();
    entry.connectionId = connectionId;
    entry.sessionKey = sessionKey;
    entry.state = ConnectionState.Connecting;
    entry.backend = backend;
    entry.request = request;
    entry.context = context;
    entry.epoch = SessionManager.nextEpoch();
    SessionManager.entries.set(connectionId, entry);
    SessionManager.byKey.set(sessionKey, connectionId);
    const epoch: number = entry.epoch;
    backend.setSignalListener((signal: SessionSignal): void => {
      SessionManager.onSessionSignal(entry, epoch, signal);
    });
    SessionManager.notify(connectionId, ConnectionState.Connecting, '', request.origin);
    SessionManager.launch(entry, epoch);
  }

  private static launch(entry: SessionEntry, epoch: number): void {
    const backend: SessionBackend | undefined = entry.backend;
    if (backend === undefined) {
      return;
    }
    if (!backend.requiresPrepare()) {
      SessionManager.proceed(entry, epoch);
      return;
    }
    backend.prepare().then((ok: boolean): void => {
      if (SessionManager.entries.get(entry.connectionId) !== entry || entry.epoch !== epoch) {
        return;
      }
      if (entry.state !== ConnectionState.Connecting) {
        return;
      }
      if (!ok) {
        if (entry.backend !== undefined) {
          entry.backend.stop();
        }
        SessionManager.publish(entry, ConnectionState.Failed, '连接失败');
        return;
      }
      SessionManager.proceed(entry, epoch);
    }).catch((error: BusinessError): void => {
      hilog.error(DOMAIN, TAG, 'prepare failed: %{public}s', JSON.stringify(error));
      if (SessionManager.entries.get(entry.connectionId) !== entry || entry.epoch !== epoch) {
        return;
      }
      if (entry.state === ConnectionState.Connecting) {
        if (entry.backend !== undefined) {
          entry.backend.stop();
        }
        SessionManager.publish(entry, ConnectionState.Failed, '连接失败');
      }
    });
  }

  private static proceed(entry: SessionEntry, epoch: number): void {
    const backend: SessionBackend | undefined = entry.backend;
    if (backend === undefined || entry.epoch !== epoch) {
      return;
    }
    if (backend.requiresWindowAtStart()) {
      if (!entry.windowOpen) {
        SessionManager.openWindowForEntry(entry);
        SessionManager.armTimeout(entry);
        return;
      }
      SessionManager.beginStream(entry.connectionId);
      return;
    }
    const handle: number = backend.start();
    if (handle < 0) {
      backend.clearSignalListener();
      if (entry.state === ConnectionState.Connecting) {
        SessionManager.publish(entry, ConnectionState.Failed, '无法创建会话');
      }
      return;
    }
    SessionManager.armTimeout(entry);
  }

  static beginStream(connectionId: string): boolean {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined || entry.backend === undefined) {
      return false;
    }
    if (entry.state !== ConnectionState.Connecting) {
      return false;
    }
    const handle: number = entry.backend.start();
    if (handle < 0) {
      entry.backend.clearSignalListener();
      if (entry.state === ConnectionState.Connecting) {
        SessionManager.publish(entry, ConnectionState.Failed, '无法创建会话');
      }
      return false;
    }
    SessionManager.armTimeout(entry);
    return true;
  }

  static disconnect(connectionId: string): void {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined || entry.state === ConnectionState.Disconnecting) {
      return;
    }
    const closeCallback: SessionCloseListener | undefined =
      SessionManager.closeListeners.get(entry.sessionKey);
    const origin: ConnectOrigin = entry.request.origin;
    SessionManager.clearEntryTimeout(entry);
    SessionManager.publish(entry, ConnectionState.Disconnecting, '');
    if (entry.backend !== undefined) {
      entry.backend.stop();
    }
    SessionManager.cleanup(entry);
    SessionManager.notify(connectionId, ConnectionState.Idle, '', origin);
    if (closeCallback !== undefined) {
      closeCallback();
    }
  }

  static retry(connectionId: string): boolean {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined || entry.backend === undefined) {
      return false;
    }
    if (!SessionManager.acceptAttempt(connectionId, entry.state)) {
      return false;
    }
    SessionManager.cancelOtherAttempts(connectionId);
    const backend: SessionBackend = entry.backend;
    entry.epoch = SessionManager.nextEpoch();
    const epoch: number = entry.epoch;
    backend.setSignalListener((signal: SessionSignal): void => {
      SessionManager.onSessionSignal(entry, epoch, signal);
    });
    SessionManager.publish(entry, ConnectionState.Connecting, '');
    SessionManager.launch(entry, epoch);
    return true;
  }

  private static cancelOtherAttempts(keepConnectionId: string): void {
    const ids: string[] = [];
    SessionManager.entries.forEach((entry: SessionEntry): void => {
      if (entry.connectionId !== keepConnectionId
        && entry.state === ConnectionState.Connecting) {
        ids.push(entry.connectionId);
      }
    });
    for (let i = 0; i < ids.length; i++) {
      SessionManager.disconnect(ids[i]);
    }
  }

  private static acceptAttempt(connectionId: string, state: ConnectionState): boolean {
    if (state === ConnectionState.Connecting || state === ConnectionState.Connected ||
      state === ConnectionState.Disconnecting) {
      return false;
    }
    const now: number = Date.now();
    const last: number | undefined = SessionManager.lastAttemptAt.get(connectionId);
    if (last !== undefined && now - last < MIN_ATTEMPT_INTERVAL_MS) {
      return false;
    }
    SessionManager.lastAttemptAt.set(connectionId, now);
    return true;
  }

  static release(sessionKey: string): void {
    SessionManager.windowEventListeners.delete(sessionKey);
    SessionManager.windowStateListeners.delete(sessionKey);
    SessionManager.closeListeners.delete(sessionKey);
    SessionManager.windowKeys.delete(sessionKey);
    const connectionId: string | undefined = SessionManager.byKey.get(sessionKey);
    if (connectionId === undefined) {
      return;
    }
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    let origin: ConnectOrigin = ConnectOrigin.List;
    if (entry !== undefined && entry.sessionKey === sessionKey) {
      origin = entry.request.origin;
      SessionManager.clearEntryTimeout(entry);
      if (entry.backend !== undefined) {
        entry.backend.stop();
      }
      SessionManager.entries.delete(connectionId);
    }
    SessionManager.byKey.delete(sessionKey);
    SessionBackendRegistry.unregister(sessionKey);
    SessionManager.notify(connectionId, ConnectionState.Idle, '', origin);
  }

  static attachWindow(sessionKey: string, onState: SessionWindowStateListener,
    onEvent: SessionWindowEventListener, onClose: SessionCloseListener): void {
    SessionManager.windowStateListeners.set(sessionKey, onState);
    SessionManager.windowEventListeners.set(sessionKey, onEvent);
    SessionManager.closeListeners.set(sessionKey, onClose);
    const connectionId: string | undefined = SessionManager.byKey.get(sessionKey);
    if (connectionId !== undefined) {
      const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
      if (entry !== undefined) {
        entry.windowOpen = true;
      }
    }
  }

  static registerWindow(sessionKey: string): void {
    if (sessionKey.length > 0) {
      SessionManager.windowKeys.add(sessionKey);
    }
  }

  static hasSessionWindow(): boolean {
    return SessionManager.windowKeys.size > 0;
  }

  static closeAllSessions(): void {
    const ids: string[] = [];
    SessionManager.entries.forEach((entry: SessionEntry): void => {
      ids.push(entry.connectionId);
    });
    for (let i = 0; i < ids.length; i++) {
      SessionManager.disconnect(ids[i]);
    }
    SessionManager.windowKeys.clear();
  }

  private static openWindowForEntry(entry: SessionEntry): void {
    if (entry.windowOpen) {
      return;
    }
    entry.windowOpen = true;
    if (entry.context !== undefined) {
      WindowController.openSessionWindow(entry.context, entry.sessionKey);
    }
  }

  private static nextEpoch(): number {
    SessionManager.epochCounter++;
    return SessionManager.epochCounter;
  }

  private static onSessionSignal(entry: SessionEntry, epoch: number,
    signal: SessionSignal): void {
    if (SessionManager.entries.get(entry.connectionId) !== entry || entry.epoch !== epoch) {
      return;
    }
    const eventListener: SessionWindowEventListener | undefined =
      SessionManager.windowEventListeners.get(entry.sessionKey);

    if (signal.kind === SessionSignalKind.Connected) {
      SessionManager.clearEntryTimeout(entry);
      SessionManager.handleConnected(entry);
      SessionManager.publish(entry, ConnectionState.Connected, '');
      SessionManager.openWindowForEntry(entry);
      return;
    }
    if (signal.kind === SessionSignalKind.Error) {
      SessionManager.clearEntryTimeout(entry);
      const message: string = SessionManager.describeError(signal.message, signal.code);
      if (entry.backend !== undefined) {
        entry.backend.stop();
      }
      SessionManager.publish(entry, ConnectionState.Failed, message);
      return;
    }
    if (signal.kind === SessionSignalKind.Disconnected) {
      if (entry.state === ConnectionState.Connecting ||
        entry.state === ConnectionState.Connected) {
        SessionManager.clearEntryTimeout(entry);
        const message: string = SessionManager.describeError(signal.message, signal.code, '连接已断开');
        if (entry.backend !== undefined) {
          entry.backend.stop();
        }
        SessionManager.publish(entry, ConnectionState.Idle, message);
      }
      return;
    }
    if (eventListener !== undefined) {
      eventListener(signal);
    }
  }

  private static handleConnected(entry: SessionEntry): void {
    if (entry.request.origin !== ConnectOrigin.Editor) {
      return;
    }
    const connectionId: string = entry.connectionId;
    const password: string = entry.request.options.password;
    if (connectionId.length > 0 && password.length > 0) {
      CredentialStore.savePassword(connectionId, password).catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'save password failed: %{public}s', JSON.stringify(error));
      });
    }
    const callback: (() => void) | undefined = entry.request.onConnected;
    entry.request.onConnected = undefined;
    if (callback !== undefined) {
      callback();
    }
  }

  private static cleanup(entry: SessionEntry): void {
    SessionManager.clearEntryTimeout(entry);
    SessionManager.entries.delete(entry.connectionId);
    SessionManager.byKey.delete(entry.sessionKey);
    SessionBackendRegistry.unregister(entry.sessionKey);
    SessionManager.windowEventListeners.delete(entry.sessionKey);
    SessionManager.windowStateListeners.delete(entry.sessionKey);
    SessionManager.closeListeners.delete(entry.sessionKey);
  }

  private static publish(entry: SessionEntry, state: ConnectionState, message: string): void {
    entry.state = state;
    entry.message = message;
    SessionManager.notify(entry.connectionId, state, message, entry.request.origin);
    const windowListener: SessionWindowStateListener | undefined =
      SessionManager.windowStateListeners.get(entry.sessionKey);
    if (windowListener !== undefined) {
      windowListener(state, message);
    }
  }

  private static armTimeout(entry: SessionEntry): void {
    SessionManager.clearEntryTimeout(entry);
    entry.timeoutId = setTimeout(() => {
      entry.timeoutId = -1;
      if (SessionManager.entries.get(entry.connectionId) !== entry ||
        entry.state !== ConnectionState.Connecting) {
        return;
      }
      if (entry.backend !== undefined) {
        entry.backend.stop();
      }
      SessionManager.publish(entry, ConnectionState.Failed, '连接超时，请检查地址或网络');
    }, CONNECT_TIMEOUT_MS);
  }

  private static clearEntryTimeout(entry: SessionEntry): void {
    if (entry.timeoutId !== -1) {
      clearTimeout(entry.timeoutId);
      entry.timeoutId = -1;
    }
  }

  private static notify(connectionId: string, state: ConnectionState, message: string,
    origin: ConnectOrigin): void {
    const listeners: SessionStateListener[] = SessionManager.stateListeners.slice();
    for (let i = 0; i < listeners.length; i++) {
      listeners[i](connectionId, state, message, origin);
    }
  }

  // The code's high 16 bits are the FreeRDP error class: 1 = ERRINFO (server
  // ErrorInfo PDU), 2 = ERRCONNECT (local connect failure); see the patch's error.h.
  static describeError(raw: string, code: number, fallback: string = '连接失败'): string {
    if (code === 0) {
      return raw.length > 0 ? raw : fallback;
    }
    const type: number = code & 0xFFFF;
    const errorClass: number = (code >> 16) & 0xFFFF;
    if (errorClass === 1) {
      return SessionManager.disconnectErrorText(type, raw, fallback);
    }
    if (errorClass === 2) {
      return SessionManager.connectErrorText(type, raw, fallback);
    }
    return raw.length > 0 ? raw : fallback;
  }

  private static disconnectErrorText(type: number, raw: string, fallback: string): string {
    if (type === 0x01) {
      return '远端服务结束了会话';
    }
    if (type === 0x02) {
      return '远端已注销';
    }
    if (type === 0x03) {
      return '因空闲超时被断开';
    }
    if (type === 0x04) {
      return '登录超时';
    }
    if (type === 0x05) {
      return '已被另一个连接接管（同一账户在其他位置登录）';
    }
    if (type === 0x06) {
      return '远端资源不足';
    }
    if (type === 0x07) {
      return '远端拒绝了连接';
    }
    if (type === 0x09) {
      return '远端权限不足';
    }
    if (type === 0x0A) {
      return '需要重新输入凭据';
    }
    if (type === 0x0B) {
      return '远端用户断开了连接';
    }
    if (type === 0x0C) {
      return '远端用户已注销';
    }
    if (type === 0x0F || type === 0x11 || type === 0x12) {
      return '远端显示驱动异常，会话被断开';
    }
    if (type === 0x10) {
      return '远端桌面窗口管理器异常，会话被断开';
    }
    if (type === 0x17 || type === 0x18) {
      return '远端系统进程异常，会话被断开';
    }
    return raw.length > 0 ? raw : fallback;
  }

  private static connectErrorText(type: number, raw: string, fallback: string): string {
    if (type === 0x01) {
      return '连接初始化失败';
    }
    if (type === 0x03) {
      return '连接建立后初始化失败';
    }
    if (type === 0x04) {
      return '无法解析服务器地址';
    }
    if (type === 0x05) {
      return '找不到服务器，请检查地址';
    }
    if (type === 0x06) {
      return '无法连接到服务器，请检查网络';
    }
    if (type === 0x07) {
      return '连接协商失败';
    }
    if (type === 0x08) {
      return 'TLS 安全连接失败';
    }
    if (type === 0x09) {
      return '身份验证失败';
    }
    if (type === 0x0A) {
      return '权限不足';
    }
    if (type === 0x0C) {
      return '安全协商失败';
    }
    if (type === 0x0D) {
      return '网络连接中断';
    }
    if (type === 0x0E) {
      return '密码已过期';
    }
    if (type === 0x12) {
      return '账户已被禁用';
    }
    if (type === 0x13) {
      return '需要先修改密码';
    }
    if (type === 0x14) {
      return '登录失败，请检查用户名或密码';
    }
    if (type === 0x15) {
      return '用户名或密码错误';
    }
    if (type === 0x16) {
      return '访问被拒绝';
    }
    if (type === 0x17) {
      return '账户受限，无法登录';
    }
    if (type === 0x18) {
      return '账户已被锁定';
    }
    if (type === 0x19) {
      return '账户已过期';
    }
    if (type === 0x1A) {
      return '不允许此登录方式';
    }
    if (type === 0x1B) {
      return '缺少用户名或密码';
    }
    if (type === 0x1C) {
      return '连接超时';
    }
    return raw.length > 0 ? raw : fallback;
  }
}
