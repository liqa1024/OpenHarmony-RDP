/*
 * HmRdp - owns background RDP connections started from the connection list.
 *
 * This is the single authority for a connection's lifecycle: it creates and
 * destroys the native session, owns the native event listener and publishes the
 * one state the list and the session window both render from. The session window
 * never starts or stops a session itself.
 *
 * The state machine is, per connection:
 *   Idle -> Connecting -> Connected -> Disconnecting -> Idle
 *                      \-> Failed -------------/
 * with one attempt = one session key = one epoch. An event only applies when its
 * epoch is still the entry's current one, so a late event from a session that was
 * already replaced can never drive the current one.
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
// An attempt is a full negotiation plus a logon on the remote, and its other end
// is a disconnect (the remote session stays as Disconnected, not logged off), so
// attempts are never repeated back to back: an attempt that
// is still running is never restarted, and two attempts for one connection are
// kept at least this far apart. Hammering a retry button (or a double tap on the
// list) therefore cannot turn into a connect/logon storm. The real gate is that
// the previous native session has been fully destroyed (RdpNative.stopSession
// waits for that), so this interval is only a secondary guard.
const MIN_ATTEMPT_INTERVAL_MS: number = 1500;

export enum ConnectionState {
  Idle = 0,
  Connecting = 1,
  Connected = 2,
  // The native session is being torn down; no new attempt may start until it is
  // gone (which stopSession() has already waited for by the time we leave it).
  Disconnecting = 3,
  Failed = 4
}

export type SessionStateListener = (connectionId: string, state: ConnectionState,
  message: string, origin: RdpConnectOrigin) => void;
/** Raw native events that only drive the session window's UI effects. */
export type SessionWindowEventListener = (event: RdpEvent, data: string) => void;
/** The one lifecycle state the session window renders (from the manager). */
export type SessionWindowStateListener = (state: ConnectionState, message: string) => void;
export type SessionCloseListener = () => void;

class SessionEntry {
  connectionId: string = '';
  sessionKey: string = '';
  state: ConnectionState = ConnectionState.Idle;
  message: string = '';
  native: RdpNative = new RdpNative();
  request: PendingConnection = new PendingConnection();
  // The context the window is launched from; kept so a retry can reopen the
  // window (only needed while it is not open yet).
  context: common.UIAbilityContext | undefined = undefined;
  // Bumped on every attempt for this connection. Events carry the epoch of the
  // attempt that produced them and are dropped once it is stale.
  epoch: number = 0;
  windowOpen: boolean = false;
  timeoutId: number = -1;
}

export class SessionManager {
  private static entries: Map<string, SessionEntry> = new Map<string, SessionEntry>();
  private static byKey: Map<string, string> = new Map<string, string>();
  private static stateListeners: SessionStateListener[] = [];
  // Start time of the last connect attempt per connection. Kept outside the
  // entries because a failed attempt deletes its entry, while the pacing has to
  // outlive it. Bounded by the number of connections (one number each).
  private static lastAttemptAt: Map<string, number> = new Map<string, number>();
  private static epochCounter: number = 0;
  private static windowEventListeners: Map<string, SessionWindowEventListener> =
    new Map<string, SessionWindowEventListener>();
  private static windowStateListeners: Map<string, SessionWindowStateListener> =
    new Map<string, SessionWindowStateListener>();
  private static closeListeners: Map<string, SessionCloseListener> =
    new Map<string, SessionCloseListener>();

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
    const state: ConnectionState = existing === undefined
      ? ConnectionState.Idle : existing.state;
    if (!SessionManager.acceptAttempt(connectionId, state)) {
      return;
    }
    if (existing !== undefined) {
      // Only reachable when the previous entry was already Idle/Failed, so its
      // native session is gone or going away; stopSession() is idempotent and
      // waits (bounded) for the teardown, so nothing leaks behind us.
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
    entry.context = context;
    entry.epoch = SessionManager.nextEpoch();
    SessionManager.entries.set(connectionId, entry);
    SessionManager.byKey.set(sessionKey, connectionId);
    const epoch: number = entry.epoch;
    native.setListener((event: RdpEvent, data: string) => {
      SessionManager.onNativeEvent(entry, epoch, event, data);
    });
    SessionManager.notify(connectionId, ConnectionState.Connecting, '', request.origin);
    const handle: number = native.startSession(request.options);
    if (handle < 0) {
      native.clearListener();
      SessionManager.publish(entry, ConnectionState.Failed, '无法创建会话');
      return;
    }
    SessionManager.armTimeout(entry);
  }

  /** User-initiated disconnect from the list; also closes the session window. */
  static disconnect(connectionId: string): void {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined || entry.state === ConnectionState.Disconnecting) {
      return;
    }
    const closeCallback: SessionCloseListener | undefined =
      SessionManager.closeListeners.get(entry.sessionKey);
    const origin: RdpConnectOrigin = entry.request.origin;
    SessionManager.clearEntryTimeout(entry);
    // Tell every listener (and the window) that a teardown is in flight before
    // the session is stopped, so a click during it is refused by the state, not
    // by luck.
    SessionManager.publish(entry, ConnectionState.Disconnecting, '');
    entry.native.stopSession();
    entry.native.clearListener();
    SessionManager.cleanup(entry);
    SessionManager.notify(connectionId, ConnectionState.Idle, '', origin);
    if (closeCallback !== undefined) {
      closeCallback();
    }
  }

  /**
   * Retries a dropped session from inside the session window. Returns false when
   * the attempt was refused because one is already running / tearing down or the
   * last one is too recent, so the caller does not show progress for an attempt
   * that never started.
   */
  static retry(connectionId: string): boolean {
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    if (entry === undefined) {
      return false;
    }
    if (!SessionManager.acceptAttempt(connectionId, entry.state)) {
      return false;
    }
    entry.epoch = SessionManager.nextEpoch();
    const epoch: number = entry.epoch;
    entry.native.setListener((event: RdpEvent, data: string) => {
      SessionManager.onNativeEvent(entry, epoch, event, data);
    });
    SessionManager.publish(entry, ConnectionState.Connecting, '');
    const handle: number = entry.native.startSession(entry.request.options);
    if (handle < 0) {
      entry.native.clearListener();
      SessionManager.publish(entry, ConnectionState.Failed, '无法创建会话');
      return false;
    }
    SessionManager.armTimeout(entry);
    return true;
  }

  /**
   * Decides whether a new connect attempt may start now, and books it. Refused
   * while an attempt is in flight or still tearing down (replacing a running one
   * only ever adds a second), and inside MIN_ATTEMPT_INTERVAL_MS of the previous
   * one.
   */
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

  /** Called when the session window is destroyed: tear down only its session. */
  static release(sessionKey: string): void {
    SessionManager.windowEventListeners.delete(sessionKey);
    SessionManager.windowStateListeners.delete(sessionKey);
    SessionManager.closeListeners.delete(sessionKey);
    const connectionId: string | undefined = SessionManager.byKey.get(sessionKey);
    if (connectionId === undefined) {
      return;
    }
    const entry: SessionEntry | undefined = SessionManager.entries.get(connectionId);
    let origin: RdpConnectOrigin = RdpConnectOrigin.List;
    if (entry !== undefined && entry.sessionKey === sessionKey) {
      origin = entry.request.origin;
      SessionManager.clearEntryTimeout(entry);
      entry.native.stopSession();
      entry.native.clearListener();
      SessionManager.entries.delete(connectionId);
    }
    SessionManager.byKey.delete(sessionKey);
    SessionManager.notify(connectionId, ConnectionState.Idle, '', origin);
  }

  /**
   * Session window subscribes to the manager: the lifecycle state it renders, the
   * raw native events that only drive UI effects, and the close callback.
   */
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

  private static nextEpoch(): number {
    SessionManager.epochCounter++;
    return SessionManager.epochCounter;
  }

  /**
   * Applies one native event, but only when it still belongs to the entry's
   * current attempt: a late event from a session that was already replaced (or
   * whose window was closed) is dropped instead of driving the current state.
   */
  private static onNativeEvent(entry: SessionEntry, epoch: number, event: RdpEvent,
    data: string): void {
    if (SessionManager.entries.get(entry.connectionId) !== entry || entry.epoch !== epoch) {
      return;
    }
    // Captured before any cleanup below removes them from the maps.
    const eventListener: SessionWindowEventListener | undefined =
      SessionManager.windowEventListeners.get(entry.sessionKey);

    if (event === RdpEvent.Connected) {
      SessionManager.clearEntryTimeout(entry);
      SessionManager.handleConnected(entry);
      SessionManager.publish(entry, ConnectionState.Connected, '');
      if (!entry.windowOpen) {
        entry.windowOpen = true;
        if (entry.context !== undefined) {
          WindowController.openSessionWindow(entry.context, entry.sessionKey);
        }
      }
      return;
    }
    if (event === RdpEvent.Error) {
      // A failed attempt is terminal: kError is the only terminal event it sends
      // (the native side does not also send kDisconnected), so the reason is not
      // overwritten by "连接已断开". The entry is kept in Failed so the window's
      // 重试 (or a click on the list row) can start a fresh attempt.
      SessionManager.clearEntryTimeout(entry);
      const message: string = SessionManager.describeError(data);
      entry.native.stopSession();
      SessionManager.publish(entry, ConnectionState.Failed, message);
      return;
    }
    if (event === RdpEvent.Disconnected) {
      if (entry.state === ConnectionState.Connecting ||
        entry.state === ConnectionState.Connected) {
        SessionManager.clearEntryTimeout(entry);
        const message: string = SessionManager.describeError(data, '连接已断开');
        entry.native.stopSession();
        SessionManager.publish(entry, ConnectionState.Idle, message);
      }
      return;
    }
    // Everything else (FirstFrame / Resize / Metrics / clipboard / cursor) only
    // drives the window's UI; the connection state stays with the manager.
    if (eventListener !== undefined) {
      eventListener(event, data);
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
    SessionManager.windowEventListeners.delete(entry.sessionKey);
    SessionManager.windowStateListeners.delete(entry.sessionKey);
    SessionManager.closeListeners.delete(entry.sessionKey);
  }

  /** Sets the entry state and publishes it to both the list and the window. */
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
      // Keep the entry in Failed (not cleaned up) so the window's 重试 or a
      // click on the list row can start a fresh attempt.
      entry.native.stopSession();
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
