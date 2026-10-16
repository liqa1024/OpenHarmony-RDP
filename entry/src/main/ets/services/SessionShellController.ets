import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { SessionRequests } from '../model/Connections';
import { RemoteCursor } from './RemoteCursor';
import { SessionBackend, SessionBackendRegistry } from './SessionBackend';
import { ConnectionState, SessionManager } from './SessionManager';
import { SessionInput } from './SessionInput';
import { SessionShellHooks, SessionShellHost } from './SessionViewDelegate';
import { AppSettings, SettingsStore } from './SettingsStore';
import { ToolbarAutoHide, ToolbarAutoHideHost } from './ToolbarAutoHide';
import { TOUCH_REVEAL_HIDDEN, TOUCH_REVEAL_NONE, TOUCH_REVEAL_VISIBLE,
  TouchRevealGesture, TouchRevealHost } from './TouchRevealGesture';
import { WindowController } from './WindowController';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSessionShell';

export class SessionSurfaceController extends XComponentController {
  private createdCb: (surfaceId: string, width: number, height: number) => void =
    (surfaceId: string, width: number, height: number) => {};
  private changedCb: (surfaceId: string, width: number, height: number) => void =
    (surfaceId: string, width: number, height: number) => {};
  private destroyedCb: (surfaceId: string) => void = (surfaceId: string) => {};

  setCallbacks(created: (surfaceId: string, width: number, height: number) => void,
    changed: (surfaceId: string, width: number, height: number) => void,
    destroyed: (surfaceId: string) => void): void {
    this.createdCb = created;
    this.changedCb = changed;
    this.destroyedCb = destroyed;
  }

  onSurfaceCreated(surfaceId: string): void {
    const rect: SurfaceRect = this.getXComponentSurfaceRect();
    this.createdCb(surfaceId, rect.surfaceWidth, rect.surfaceHeight);
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    this.changedCb(surfaceId, rect.surfaceWidth, rect.surfaceHeight);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    this.destroyedCb(surfaceId);
  }
}

export class SessionShellController {
  private sessionKey: string;
  private host: SessionShellHost;
  private hooks: SessionShellHooks;
  private backend: SessionBackend | undefined = undefined;
  private input: SessionInput | undefined = undefined;
  private cursor: RemoteCursor;
  private surface: SessionSurfaceController;
  private toolbarAutoHide: ToolbarAutoHide;
  private touchReveal: TouchRevealGesture;
  private armed: boolean = true;
  private sessionWindow: window.Window | undefined = undefined;
  private connectionId: string = '';
  private state: ConnectionState = ConnectionState.Connecting;
  private closing: boolean = false;
  private windowFocused: boolean = true;
  private windowOnScreen: boolean = true;
  private windowMinimized: boolean = false;
  private windowVisible: boolean = true;
  private shortcutPassthrough: boolean = false;
  private interceptActive: boolean = false;
  private overlayTimer: number = -1;
  private overlayVisible: boolean = false;

  constructor(sessionKey: string, host: SessionShellHost, hooks: SessionShellHooks,
    surface: SessionSurfaceController, cursor: RemoteCursor = new RemoteCursor()) {
    this.sessionKey = sessionKey;
    this.host = host;
    this.hooks = hooks;
    this.surface = surface;
    this.cursor = cursor;
    const autoHideHost: ToolbarAutoHideHost = {
      isArmed: (): boolean => this.armed,
      isFullscreen: (): boolean => this.host.isFullscreen(),
      isConnected: (): boolean => this.state === ConnectionState.Connected,
      isPinned: (): boolean => this.hooks.isPinned(),
      hoverDelay: (): number => this.host.hoverDelay(),
      hideDelay: (): number => this.host.hideDelay(),
      show: (): boolean => this.host.isToolbarVisible(),
      setShow: (show: boolean): void => {
        this.host.setToolbarVisible(show);
      },
      revealFill: (): number => this.host.revealFill(),
      animateReveal: (fill: number, durationMs: number, curve: Curve): void => {
        this.host.animateReveal(fill, durationMs, curve);
      },
      animateTouchBar: (fill: number, durationMs: number, curve: Curve): void => {
        this.host.animateTouchBar(fill, durationMs, curve);
      },
      showTouchTip: (): void => {
        this.host.showTouchTip();
      }
    };
    this.toolbarAutoHide = new ToolbarAutoHide(autoHideHost);
    const revealHost: TouchRevealHost = {
      mode: (): number => this.touchRevealMode(),
      forward: (phase: TouchType, id: number, x: number, y: number,
        pressure: number): void => {
        this.input?.handleTouchPoint(phase, id, x, y, pressure);
      },
      noteContact: (): void => {
        this.input?.noteTouchContact();
      },
      onSwipe: (): void => {
        this.toolbarAutoHide.noteTouchSwipe();
      },
      onDismiss: (): void => {
        this.toolbarAutoHide.noteTouchDismiss();
      }
    };
    this.touchReveal = new TouchRevealGesture(revealHost);
  }

  attach(shortcutPassthrough: boolean): void {
    const request = SessionRequests.get(this.sessionKey);
    if (request !== undefined) {
      this.connectionId = request.connectionId;
    }
    this.backend = SessionBackendRegistry.find(this.sessionKey);
    const settings: AppSettings = SettingsStore.load();
    this.shortcutPassthrough = shortcutPassthrough;
    this.hooks.configureCursor(this.cursor, settings);
    this.input = this.hooks.makeInput(this.backend, settings);
    this.surface.setCallbacks(
      (surfaceId: string, width: number, height: number): void => {
        this.hooks.onSurfaceCreated(surfaceId, width, height);
      },
      (surfaceId: string, width: number, height: number): void => {
        this.hooks.onSurfaceChanged(width, height);
      },
      (surfaceId: string): void => {
        this.hooks.onSurfaceDestroyed();
      });
    this.loadSessionWindow();
    SessionManager.attachWindow(this.sessionKey,
      (state: ConnectionState, message: string): void => {
        this.applyState(state, message);
      },
      (signal): void => {
        this.hooks.onSignal(signal);
      },
      (): void => {
        this.terminateWindow();
      });
    this.applyState(SessionManager.stateOf(this.connectionId),
      SessionManager.messageOf(this.connectionId));
  }

  release(): void {
    this.input?.releaseAll();
    this.input?.dispose();
    if (this.interceptActive && this.backend !== undefined) {
      this.backend.setShortcutPassthrough(false);
      this.interceptActive = false;
      this.input?.setInterceptActive(false);
    }
    this.armed = false;
    this.touchReveal.dispose();
    this.toolbarAutoHide.dispose();
    this.clearOverlayTimer();
    this.hooks.onRelease();
    const win: window.Window | undefined = this.sessionWindow;
    this.sessionWindow = undefined;
    if (win !== undefined) {
      try {
        win.off('windowEvent');
        win.off('windowStatusChange');
        win.off('windowVisibilityChange');
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'off window events failed: %{public}s', JSON.stringify(error));
      }
    }
    this.cursor.setWindow(undefined);
    this.cursor.endSession();
  }

  sessionInput(): SessionInput | undefined {
    return this.input;
  }

  remoteCursor(): RemoteCursor {
    return this.cursor;
  }

  sessionBackend(): SessionBackend | undefined {
    return this.backend;
  }

  surfaceController(): XComponentController {
    return this.surface;
  }

  sessionConnectionId(): string {
    return this.connectionId;
  }

  setPointerZone(zone: number): void {
    this.toolbarAutoHide.setPointerZone(zone);
  }

  refreshAutoHide(): void {
    this.toolbarAutoHide.refresh();
  }

  // The pull-down reveal only exists where touch reaches the far end: a protocol whose sink drops
  // touch falls back to synthesized mouse events, which a withheld contact could not recall.
  private touchRevealMode(): number {
    if (!this.armed || !this.host.isFullscreen()
      || this.state !== ConnectionState.Connected
      || !(this.input?.touchForwarded() ?? false)) {
      return TOUCH_REVEAL_NONE;
    }
    if (this.hooks.isPinned()) {
      return TOUCH_REVEAL_NONE;
    }
    return this.host.isToolbarVisible() ? TOUCH_REVEAL_VISIBLE : TOUCH_REVEAL_HIDDEN;
  }

  routeMouse(event: MouseEvent): void {
    this.input?.handleMouseEvent(event);
  }

  routeTouch(event: TouchEvent): void {
    if (this.touchReveal.handle(event)) {
      return;
    }
    this.input?.handleTouch(event);
  }

  routeAxis(event: AxisEvent): void {
    // The engine's frames have no source of their own, so note the raw event before either route.
    this.input?.noteAxisEvent(event);
    if (this.hooks.consumesAxis(event)) {
      return;
    }
    this.input?.handleAxis(event);
  }

  routeKey(event: KeyEvent): void {
    this.input?.handleKey(event);
  }

  routeFocusAxis(event: FocusAxisEvent): void {
    this.input?.handleFocusAxis(event);
  }

  retry(): void {
    if (this.state === ConnectionState.Connecting
      || this.state === ConnectionState.Disconnecting) {
      return;
    }
    if (this.connectionId.length === 0) {
      return;
    }
    if (!SessionManager.retry(this.connectionId)) {
      return;
    }
    this.hooks.onBeforeRetry();
  }

  disconnect(): void {
    if (this.connectionId.length > 0) {
      SessionManager.disconnect(this.connectionId);
      return;
    }
    this.input?.releaseAll();
    this.hooks.onDisconnectFallback();
    this.terminateWindow();
  }

  minimize(): void {
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    WindowController.minimize(win);
  }

  toggleFullscreen(): void {
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    WindowController.toggleFullscreen(win, this.host.isFullscreen());
  }

  private loadSessionWindow(): void {
    const context = this.host.ui().getHostContext();
    if (context === undefined) {
      return;
    }
    WindowController.acquireWindow(context).then((win: window.Window | undefined) => {
      if (win === undefined) {
        return;
      }
      this.sessionWindow = win;
      this.cursor.setWindow(win);
      this.host.setFullscreen(WindowController.isFullscreen(win));
      WindowController.watchStatusChange(win, (status: window.WindowStatusType): void => {
        this.host.setFullscreen(status === window.WindowStatusType.FULL_SCREEN);
        this.setWindowMinimized(status === window.WindowStatusType.MINIMIZE);
        this.refreshAutoHide();
      });
      WindowController.watchFocus(win, (focused: boolean): void => {
        this.windowFocused = focused;
        if (focused) {
          this.input?.reanchorPointer();
        } else {
          this.input?.releaseAll();
          this.hooks.onReleaseInputs();
        }
        this.updateShortcutPassthrough();
      });
      WindowController.watchVisibility(win, (visible: boolean): void => {
        this.setWindowOnScreen(visible);
      });
    });
  }

  private applyState(state: ConnectionState, message: string): void {
    this.state = state;
    this.host.setState(state);
    this.host.setStatusText(message);
    this.host.setToolbarVisible(state !== ConnectionState.Connected);
    this.updateOverlay();
    this.refreshAutoHide();
    this.updateShortcutPassthrough();
    this.hooks.onStateChanged(state, message);
  }

  private updateOverlay(): void {
    if (this.state === ConnectionState.Connecting) {
      if (this.overlayVisible || this.overlayTimer !== -1) {
        return;
      }
      const delay: number = this.hooks.overlayDelayMs();
      if (delay <= 0) {
        this.setOverlay(true);
        return;
      }
      this.overlayTimer = setTimeout((): void => {
        this.overlayTimer = -1;
        if (this.state === ConnectionState.Connecting) {
          this.setOverlay(true);
        }
      }, delay);
      return;
    }
    this.clearOverlayTimer();
    this.setOverlay(this.state !== ConnectionState.Connected);
  }

  private setOverlay(visible: boolean): void {
    this.overlayVisible = visible;
    this.host.setOverlayVisible(visible);
  }

  private clearOverlayTimer(): void {
    if (this.overlayTimer !== -1) {
      clearTimeout(this.overlayTimer);
      this.overlayTimer = -1;
    }
  }

  private updateShortcutPassthrough(): void {
    const backend: SessionBackend | undefined = this.backend;
    if (backend === undefined) {
      return;
    }
    const wanted: boolean = this.shortcutPassthrough
      && this.state === ConnectionState.Connected
      && this.windowFocused;
    if (wanted === this.interceptActive) {
      return;
    }
    if (wanted) {
      // Keep the real registration result: a denied ACL permission leaves this false.
      this.interceptActive = backend.setShortcutPassthrough(true);
    } else {
      backend.setShortcutPassthrough(false);
      this.interceptActive = false;
    }
    this.input?.setInterceptActive(this.interceptActive);
  }

  private setWindowOnScreen(onScreen: boolean): void {
    if (this.windowOnScreen === onScreen) {
      return;
    }
    this.windowOnScreen = onScreen;
    this.syncWindowVisible();
  }

  private setWindowMinimized(minimized: boolean): void {
    if (this.windowMinimized === minimized) {
      return;
    }
    this.windowMinimized = minimized;
    this.syncWindowVisible();
  }

  private syncWindowVisible(): void {
    const visible: boolean = this.windowOnScreen && !this.windowMinimized;
    if (this.windowVisible === visible) {
      return;
    }
    this.windowVisible = visible;
    this.hooks.onWindowVisible(visible);
  }

  private terminateWindow(): void {
    if (this.closing) {
      return;
    }
    this.closing = true;
    this.hooks.onRelease();
    this.cursor.setWindow(undefined);
    this.cursor.endSession();
    const context = this.host.ui().getHostContext();
    if (context === undefined) {
      return;
    }
    const abilityContext: common.UIAbilityContext = context as common.UIAbilityContext;
    if (WindowController.closeSession(abilityContext)) {
      return;
    }
    abilityContext.terminateSelf().catch((error: BusinessError): void => {
      hilog.error(DOMAIN, TAG, 'terminate session failed: %{public}s', JSON.stringify(error));
    });
  }
}
