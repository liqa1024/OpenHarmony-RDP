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

export enum ConnectionState {
  Idle = 0,
  Connecting = 1,
  Connected = 2,
  Failed = 3
}

export type SessionStateListener = (connectionId: string, state: ConnectionState,
  message: string) => void;
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
    SessionManager.notify(connectionId, ConnectionState.Connecting, '');
    const handle: number = native.startSession(request.options);
    if (handle < 0) {
      entry.state = ConnectionState.Failed;
      entry.message = '无法创建会话';
      native.clearListener();
      SessionManager.cleanup(entry);
      SessionManager.notify(connectionId, ConnectionState.Failed, entry.message);
    }
  }

  /** User-initiated disconnect from the list; also closes the session window. */
  static disconnect(connectionId: string): void {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined) {
      return;
    }
    const closeCallback: SessionCloseListener | undefined =
      SessionManager.closeListeners.get(entry.sessionKey);
    entry.native.stopSession();
    entry.native.clearListener();
    SessionManager.cleanup(entry);
    SessionManager.notify(connectionId, ConnectionState.Idle, '');
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
    SessionManager.notify(connectionId, ConnectionState.Connecting, '');
    const handle: number = entry.native.startSession(entry.request.options);
    if (handle < 0) {
      entry.state = ConnectionState.Failed;
      entry.message = '无法创建会话';
      SessionManager.cleanup(entry);
      SessionManager.notify(connectionId, ConnectionState.Failed, entry.message);
    }
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
    if (entry !== undefined) {
      entry.native.stopSession();
      entry.native.clearListener();
      SessionManager.entries.delete(connectionId);
    }
    SessionManager.byKey.delete(sessionKey);
    SessionManager.notify(connectionId, ConnectionState.Idle, '');
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
      entry.state = ConnectionState.Connected;
      entry.message = '';
      SessionManager.handleConnected(entry);
      SessionManager.notify(entry.connectionId, ConnectionState.Connected, '');
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
      SessionManager.notify(entry.connectionId, ConnectionState.Failed, message);
      return;
    } else if (event === RdpEvent.Disconnected) {
      if (entry.state === ConnectionState.Connected ||
        entry.state === ConnectionState.Connecting) {
        const message: string = SessionManager.describeError(data);
        entry.native.stopSession();
        entry.state = ConnectionState.Idle;
        entry.message = message;
        SessionManager.notify(entry.connectionId, ConnectionState.Idle, message);
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
    SessionManager.entries.delete(entry.connectionId);
    SessionManager.byKey.delete(entry.sessionKey);
    SessionManager.windowListeners.delete(entry.sessionKey);
    SessionManager.closeListeners.delete(entry.sessionKey);
  }

  private static notify(connectionId: string, state: ConnectionState, message: string): void {
    for (let i = 0; i < SessionManager.stateListeners.length; i++) {
      SessionManager.stateListeners[i](connectionId, state, message);
    }
  }

  /** Turns the native "<code>|<message>" payload into a user-facing reason. */
  static describeError(data: string): string {
    const separator: number = data.indexOf('|');
    if (separator < 0) {
      return data.length > 0 ? data : '连接失败';
    }
    const code: number = Number.parseInt(data.substring(0, separator), 10);
    const raw: string = data.substring(separator + 1);
    const type: number = code & 0xFFFF;
    const errorClass: number = (code >> 16) & 0xFFFF;
    if (errorClass === 2) {
      return SessionManager.connectErrorText(type, raw);
    }
    return raw.length > 0 ? raw : '连接失败';
  }

  private static connectErrorText(type: number, raw: string): string {
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
    return raw.length > 0 ? raw : '连接失败';
  }
}
