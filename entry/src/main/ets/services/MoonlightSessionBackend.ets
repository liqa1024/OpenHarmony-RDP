/*
 * HmRdp - Moonlight session backend.
 *
 * Implements the protocol-neutral SessionBackend over the native Moonlight
 * session (hmrdp_moonlight_session.* / LiStartConnection). Unlike RDP, the
 * native start takes the render target, so requiresWindowAtStart() is true: the
 * manager opens the window first, the page binds its XComponent surface, and
 * only then does the manager call start() (via SessionManager.beginStream).
 *
 * The stream config (host, /launch result, resolved profile) is supplied by the
 * control layer once it has run serverinfo -> launch; the backend carries it
 * across to LiStartConnection and maps native events onto SessionSignal.
 */
import {
  ML_EVENT_STARTED, ML_EVENT_STAGE_COMPLETE, ML_EVENT_STAGE_FAILED, ML_EVENT_STAGE_STARTING,
  ML_EVENT_TERMINATED, MoonlightSessionConfig, moonlightKey, moonlightMouseButton,
  moonlightMouseMove, moonlightMousePosition, moonlightScroll, moonlightSessionActive,
  moonlightSessionClearSurface, moonlightSessionSetSurface, moonlightSessionStart,
  moonlightSessionStop, moonlightSetSessionListener
} from 'libhmrdp.so';
import { PendingConnection, PROTOCOL_MOONLIGHT } from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { SessionBackend, SessionSignalListener } from './SessionBackend';

export class MoonlightSessionBackend implements SessionBackend {
  private listener: SessionSignalListener | undefined = undefined;
  private config: MoonlightSessionConfig | undefined = undefined;
  private windowBound: boolean = false;

  constructor(request: PendingConnection) {
    // The control layer assembled the config before the connect; without one the
    // session reports a clear failure instead of misbehaving.
    this.config = request.moonlightConfig;
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
      this.emit(ML_EVENT_STAGE_FAILED, 0, '尚未配置串流参数（控制层未接入）');
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
