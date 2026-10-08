/*
 * HmRdp - Moonlight session backend.
 *
 * Implements the protocol-neutral SessionBackend over the native Moonlight
 * session (hmrdp_moonlight_session.* / LiStartConnection). Unlike RDP, the
 * native start takes the render target, so requiresWindowAtStart() is true: the
 * manager opens the window first, the page binds its XComponent surface, and
 * only then does the manager call start() (via SessionManager.beginStream).
 *
 * The manager drives the control layer's pre-flight (serverinfo -> pair ->
 * launch) through prepare(); that assembles the stream config, which start()
 * then carries across to LiStartConnection. Native events map onto SessionSignal.
 */
import {
  MoonlightSessionConfig, moonlightKey, moonlightMouseButton, moonlightMouseMove,
  moonlightMousePosition, moonlightScroll, moonlightSessionActive, moonlightSessionClearSurface,
  moonlightSessionSetSurface, moonlightSessionStart, moonlightSessionStop, moonlightSetSessionListener
} from 'libhmrdp.so';
import { PendingConnection, PROTOCOL_MOONLIGHT, SavedConnection } from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { ConnectionStore } from './ConnectionStore';
import { MoonlightConnectResult, MoonlightConnector, MoonlightHostPolicy,
  MoonlightPinProvider } from './MoonlightConnector';
import { MoonlightHostState } from './MoonlightHostState';
import { PairResult } from '../moonlight/MoonlightModels';
import { SessionBackend, SessionSignalListener } from './SessionBackend';

/** Session event ids; mirror hmrdp::MoonlightSessionEvent (hmrdp_moonlight_session.h). */
export const ML_EVENT_STARTED: number = 0;
export const ML_EVENT_TERMINATED: number = 1;
export const ML_EVENT_STAGE_STARTING: number = 2;
export const ML_EVENT_STAGE_COMPLETE: number = 3;
export const ML_EVENT_STAGE_FAILED: number = 4;

export class MoonlightSessionBackend implements SessionBackend {
  private listener: SessionSignalListener | undefined = undefined;
  private request: PendingConnection;
  private config: MoonlightSessionConfig | undefined = undefined;
  private windowBound: boolean = false;

  constructor(request: PendingConnection) {
    // The manager drives the control-layer pre-flight through prepare(); the
    // request carries everything it needs (host, stream, files dir, PIN popup).
    this.request = request;
    // One process-global session at a time (moonlight-common-c is single
    // connection), so the listener is registered here rather than per stream.
    moonlightSetSessionListener((event: number, value: number, text: string): void => {
      this.emit(event, value, text);
    });
  }

  protocol(): number {
    return PROTOCOL_MOONLIGHT;
  }

  /**
   * The control layer (serverinfo -> pair -> launch) is async and must run
   * before the window can open, so the manager awaits it as part of Connecting.
   */
  requiresPrepare(): boolean {
    return true;
  }

  /**
   * Runs the Moonlight pre-flight and assembles the native stream config. The
   * manager has already published Connecting, so a failure here is reported as
   * an Error signal (its reason becomes the connection error message).
   */
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
      // A failed pre-flight may have relied on a launch that did not happen; let
      // the next probe settle the host target instead of trusting the optimistic
      // value a caller wrote before this ran.
      MoonlightHostState.refresh(conn, request.filesDir);
      this.emit(ML_EVENT_STAGE_FAILED, 0, result.error);
      return false;
    }
    this.config = result.config;
    request.moonlightConfig = result.config;
    request.moonlightHttpsPort = result.httpsPort;
    // The host now runs the launched app (or the auto-picked default); this is
    // the authoritative target for both the list row and the app page.
    MoonlightHostState.markLaunched(conn.id, result.effectiveAppId);
    // Learn the host's own id for the connection-uniqueness check (only when it
    // changed, so a normal connect does not write the store).
    if (result.hostId.length > 0 && conn.mlHostId !== result.hostId) {
      conn.mlHostId = result.hostId;
      conn.updatedAt = Date.now();
      ConnectionStore.save(conn);
    }
    return true;
  }

  /**
   * The manager opens the window first; the page binds the surface and then
   * asks the manager to begin the stream.
   */
  requiresWindowAtStart(): boolean {
    return true;
  }

  // Process-global session: it has no per-handle id (the window page only needs
  // it for RDP-specific worker routing, which Moonlight does not use).
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

  // --- protocol-neutral input verbs --------------------------------------
  // The Moonlight session window is protocol-specific and drives the concrete
  // verbs below; the generic ones are unused until an input model that both
  // protocols share is defined (the RDP page keeps using RdpSessionBackend).

  pointer(flags: number, x: number, y: number): void {
  }

  touch(flags: number, finger: number, pressure: number, x: number, y: number): void {
  }

  key(scancode: number, down: boolean, extended: boolean): void {
  }

  unicode(codepoint: number, down: boolean): void {
  }

  // --- concrete input (used by the Moonlight window) ---------------------

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

  /** Maps a native session event onto the protocol-neutral envelope. */
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
