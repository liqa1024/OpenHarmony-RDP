/*
 * HmRdp - owns background RDP connections started from the connection list.
 *
 * A connection is established here (without any window) so the list can show
 * progress, errors and the connected state. Only once it succeeds does the
 * session window open and take over the already-running native session.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PendingConnection, RdpConnectOrigin, RdpEvent, SessionRequests } from '../model/RdpModels';
import { CredentialStore } from './CredentialStore';
import { RdpNative } from './RdpNative';
import { WindowController } from './WindowController';

const DOMAIN: number = 0xD003;
const TAG: string = 'HmRdpSessionManager';
// Safety net for connection attempts that never report a terminal event (an
// unreachable host or a stalled handshake would otherwise spin forever).
const CONNECT_TIMEOUT_MS: number = 30000;

export enum ConnectionState {
  Idle = 0,
  Connecting = 1,
  Connected = 2,
  Failed = 3
}

export type SessionStateListener = (connectionId: string, state: ConnectionState,
  message: string, origin: RdpConnectOrigin) => void;
export type SessionWindowListener = (event: RdpEvent, data: string) => void;
export type SessionCloseListener = () => void;

class SessionEntry {
  connectionId: string = '';
  sessionKey: string = '';
  state: ConnectionState = ConnectionState.Idle;
  message: string = '';
  native: RdpNative = new RdpNative();
  request: PendingConnection = new PendingConnection();
  windowOpen: boolean = false;
  timeoutId: number = -1;
}

export class SessionManager {
  private static entries: Map<string, SessionEntry> = new Map<string, SessionEntry>();
  private static byKey: Map<string, string> = new Map<string, string>();
  private static stateListeners: SessionStateListener[] = [];
  private static windowListeners: Map<string, SessionWindowListener> =
    new Map<string, SessionWindowListener>();
  private static closeListeners: Map<string, SessionCloseListener> =
    new Map<string, SessionCloseListener>();

  static stateOf(connectionId: string): ConnectionState {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    return entry === undefined ? ConnectionState.Idle : entry.state;
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

  /** Drops every listener; the main window calls this on window destroy. */
  static clearStateListeners(): void {
    SessionManager.stateListeners = [];
  }

  /**
   * Starts a background connection. The session window is opened only after the
   * connection succeeds; failures are reported through the state listeners.
   */
  static connect(request: PendingConnection, context: common.UIAbilityContext): void {
    const connectionId: string = request.connectionId;
    const existing: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (existing !== undefined) {
      if (existing.state === ConnectionState.Connecting ||
        existing.state === ConnectionState.Connected) {
        return;
      }
      existing.native.stopSession();
      existing.native.clearListener();
      SessionManager.cleanup(existing);
    }
    const sessionKey: string = SessionRequests.newKey();
    SessionRequests.put(sessionKey, request);
    const native: RdpNative = new RdpNative();
    RdpNative.register(sessionKey, native);
    const entry: SessionEntry = new SessionEntry();
    entry.connectionId = connectionId;
    entry.sessionKey = sessionKey;
    entry.state = ConnectionState.Connecting;
    entry.native = native;
    entry.request = request;
    SessionManager.entries.set(connectionId, entry);
    SessionManager.byKey.set(sessionKey, connectionId);
    native.setListener((event: RdpEvent, data: string) => {
      SessionManager.onNativeEvent(entry, event, data, context);
    });
    SessionManager.notify(connectionId, ConnectionState.Connecting, '', request.origin);
    const handle: number = native.startSession(request.options);
    if (handle < 0) {
      entry.state = ConnectionState.Failed;
      entry.message = '无法创建会话';
      native.clearListener();
      SessionManager.cleanup(entry);
      SessionManager.notify(connectionId, ConnectionState.Failed, entry.message,
        request.origin);
      return;
    }
    SessionManager.armTimeout(entry);
  }

  /** User-initiated disconnect from the list; also closes the session window. */
  static disconnect(connectionId: string): void {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined) {
      return;
    }
    const closeCallback: SessionCloseListener | undefined =
      SessionManager.closeListeners.get(entry.sessionKey);
    const origin: RdpConnectOrigin = entry.request.origin;
    entry.native.stopSession();
    entry.native.clearListener();
    SessionManager.cleanup(entry);
    SessionManager.notify(connectionId, ConnectionState.Idle, '', origin);
    if (closeCallback !== undefined) {
      closeCallback();
    }
  }

  /** Retries a dropped session from inside the session window. */
  static retry(connectionId: string): void {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined) {
      return;
    }
    entry.state = ConnectionState.Connecting;
    entry.message = '';
    SessionManager.notify(connectionId, ConnectionState.Connecting, '',
      entry.request.origin);
    const handle: number = entry.native.startSession(entry.request.options);
    if (handle < 0) {
      entry.state = ConnectionState.Failed;
      entry.message = '无法创建会话';
      SessionManager.cleanup(entry);
      SessionManager.notify(connectionId, ConnectionState.Failed, entry.message,
        entry.request.origin);
      return;
    }
    SessionManager.armTimeout(entry);
  }

  /** Called when the session window is destroyed: tear down only its session. */
  static release(sessionKey: string): void {
    SessionManager.windowListeners.delete(sessionKey);
    SessionManager.closeListeners.delete(sessionKey);
    const connectionId: string | undefined = SessionManager.byKey.get(sessionKey);
    if (connectionId === undefined) {
      return;
    }
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    let origin: RdpConnectOrigin = RdpConnectOrigin.List;
    if (entry !== undefined) {
      origin = entry.request.origin;
      SessionManager.clearEntryTimeout(entry);
      entry.native.stopSession();
      entry.native.clearListener();
      SessionManager.entries.delete(connectionId);
    }
    SessionManager.byKey.delete(sessionKey);
    SessionManager.notify(connectionId, ConnectionState.Idle, '', origin);
  }

  /** Session window subscribes to native events forwarded by the manager. */
  static attachWindow(sessionKey: string, listener: SessionWindowListener,
    onClose: SessionCloseListener): void {
    SessionManager.windowListeners.set(sessionKey, listener);
    SessionManager.closeListeners.set(sessionKey, onClose);
    const connectionId: string | undefined = SessionManager.byKey.get(sessionKey);
    if (connectionId !== undefined) {
      const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
      if (entry !== undefined) {
        entry.windowOpen = true;
      }
    }
  }

  private static onNativeEvent(entry: SessionEntry, event: RdpEvent, data: string,
    context: common.UIAbilityContext): void {
    if (event === RdpEvent.Connected) {
      SessionManager.clearEntryTimeout(entry);
      entry.state = ConnectionState.Connected;
      entry.message = '';
      SessionManager.handleConnected(entry);
      SessionManager.notify(entry.connectionId, ConnectionState.Connected, '',
        entry.request.origin);
      if (!entry.windowOpen) {
        entry.windowOpen = true;
        WindowController.openSessionWindow(context, entry.sessionKey);
      }
    } else if (event === RdpEvent.Error) {
      const message: string = SessionManager.describeError(data);
      entry.state = ConnectionState.Failed;
      entry.message = message;
      entry.native.stopSession();
      entry.native.clearListener();
      SessionManager.cleanup(entry);
      SessionManager.notify(entry.connectionId, ConnectionState.Failed, message,
        entry.request.origin);
      return;
    } else if (event === RdpEvent.Disconnected) {
      if (entry.state === ConnectionState.Connected ||
        entry.state === ConnectionState.Connecting) {
        SessionManager.clearEntryTimeout(entry);
        const message: string = SessionManager.describeError(data, '连接已断开');
        entry.native.stopSession();
        entry.state = ConnectionState.Idle;
        entry.message = message;
        SessionManager.notify(entry.connectionId, ConnectionState.Idle, message,
          entry.request.origin);
      }
    }
    const windowListener: SessionWindowListener | undefined =
      SessionManager.windowListeners.get(entry.sessionKey);
    if (windowListener !== undefined) {
      windowListener(event, data);
    }
  }

  private static handleConnected(entry: SessionEntry): void {
    if (entry.request.origin !== RdpConnectOrigin.Editor) {
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
    SessionManager.windowListeners.delete(entry.sessionKey);
    SessionManager.closeListeners.delete(entry.sessionKey);
  }

  private static armTimeout(entry: SessionEntry): void {
    SessionManager.clearEntryTimeout(entry);
    entry.timeoutId = setTimeout(() => {
      entry.timeoutId = -1;
      if (entry.state !== ConnectionState.Connecting) {
        return;
      }
      entry.state = ConnectionState.Failed;
      entry.message = '连接超时，请检查地址或网络';
      entry.native.stopSession();
      entry.native.clearListener();
      SessionManager.cleanup(entry);
      SessionManager.notify(entry.connectionId, ConnectionState.Failed, entry.message,
        entry.request.origin);
    }, CONNECT_TIMEOUT_MS);
  }

  private static clearEntryTimeout(entry: SessionEntry): void {
    if (entry.timeoutId !== -1) {
      clearTimeout(entry.timeoutId);
      entry.timeoutId = -1;
    }
  }

  private static notify(connectionId: string, state: ConnectionState, message: string,
    origin: RdpConnectOrigin): void {
    const listeners: SessionStateListener[] = SessionManager.stateListeners.slice();
    for (let i = 0; i < listeners.length; i++) {
      listeners[i](connectionId, state, message, origin);
    }
  }

  /**
   * Turns the native "<code>|<message>" payload into a user-facing reason.
   * `fallback` is what to say when the payload carries no reason at all, so the
   * same parser serves both a failed connect and a dropped session.
   *
   * The code's high 16 bits are the FreeRDP error class: 1 = ERRINFO (the server's
   * ErrorInfo PDU, e.g. "another connection took over"), 2 = ERRCONNECT (a local
   * connect failure). See the patch's error.h.
   */
  static describeError(data: string, fallback: string = '连接失败'): string {
    const separator: number = data.indexOf('|');
    if (separator < 0) {
      return data.length > 0 ? data : fallback;
    }
    const code: number = Number.parseInt(data.substring(0, separator), 10);
    const raw: string = data.substring(separator + 1);
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

  /**
   * The server's reason for ending the session (ERRINFO_*, see the FreeRDP
   * error.h). These are what the disconnect overlay must distinguish: taken over
   * by another connection, logged off, idle timeout, a server-side crash, ...
   */
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
