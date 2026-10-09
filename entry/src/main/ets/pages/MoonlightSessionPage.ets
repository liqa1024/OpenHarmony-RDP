/*
 * HmRdp - Moonlight (GameStream) session window.
 *
 * The protocol-specific sibling of SessionPage: it binds the XComponent surface
 * and only then asks SessionManager to start the native Moonlight session
 * (LiStartConnection takes the render target, so the window must exist first),
 * forwards input through the MoonlightSessionBackend's concrete verbs, and
 * renders the session state published by the manager. See
 * doc_agent/moonlight-native.md §3 and session-and-input.md §1.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { image } from '@kit.ImageKit';
import { pointer } from '@kit.InputKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { PendingConnection, SessionRequests } from '../model/RdpModels';
import { SessionSignal } from '../model/SessionSignal';
import { ConnectionState, SessionManager } from '../services/SessionManager';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState } from '../services/MoonlightHostState';
import { SessionBackend, SessionBackendRegistry } from '../services/SessionBackend';
import { MlCursorShape, MoonlightSessionBackend } from '../services/MoonlightSessionBackend';
import { MlVirtualKey } from '../utils/MoonlightKeyMapper';
import { moonlightDecodeIndicator } from '../utils/MoonlightCodec';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { WindowController } from '../services/WindowController';
import { ToolbarAutoHide, ToolbarAutoHideHost } from '../services/ToolbarAutoHide';
import { ML_CURSOR_HIDE_HOST, ML_CURSOR_HIDE_SYSTEM, ML_CURSOR_OFF,
  SettingsStore } from '../services/SettingsStore';
import { lengthToNumber } from '../utils/UiUtil';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { StatusCard } from '../components/StatusCard';
import { TOOLBAR_BG, TOOLBAR_ROW_HEIGHT, TOOLBAR_ZONE_BAR, TOOLBAR_ZONE_NONE,
  TOOLBAR_ZONE_TRIGGER, SessionTitle, ToolbarChip, ToolbarRevealHint } from '../components/SessionChrome';

const DOMAIN: number = 0xD004;
const TAG: string = 'HmRdpMoonlightPage';

// The session window opens while the stream is still Connecting (LiStartConnection
// needs the render target). A connecting-status card that appeared immediately
// would flash; like RDP, whose window only opens once the session is up, the card
// is held back this long and only shown if the stream has not come up by then.
const CONNECT_OVERLAY_DELAY_MS: number = 1000;

class MlSurfaceController extends XComponentController {
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

@Entry({ useSharedStorage: true })
@Component
struct MoonlightSessionPage {
  @LocalStorageLink('sessionKey') sessionKey: string = '';
  private controller: MlSurfaceController = new MlSurfaceController();
  private backend: MoonlightSessionBackend | undefined = undefined;
  private connectionId: string = '';
  private started: boolean = false;
  private closing: boolean = false;
  // The bound XComponent surface. Kept so a retry can re-bind it: stopping a
  // session releases the native window, but this surface stays alive.
  private surfaceId: string = '';
  private surfaceWidth: number = 0;
  private surfaceHeight: number = 0;
  // Component size in vp (for mapping pointer coords to the stream frame).
  private componentWidth: number = 0;
  private componentHeight: number = 0;
  // The reference frame absolute mouse positions are expressed in. It is the
  // resolved stream size carried on the request (see MoonlightLauncher); the
  // 1080p default only covers a request that predates that resolution.
  private streamWidth: number = 1920;
  private streamHeight: number = 1080;
  // Modifier state tracked from key events (Moonlight wants a modifier bitmask).
  private modifiers: number = 0;
  // Moonlight cursor handling (ML_CURSOR_*), read once per session:
  //  - OFF / HIDE_HOST: the system cursor is never hidden;
  //  - HIDE_SYSTEM: it is hidden while the pointer is over the picture (the
  //    host's cursor baked into the video is the only one), and shown back over
  //    the toolbar / when the stream is not up;
  //  - HIDE_HOST (foundation-sunshine): the host forwards the pointer and the
  //    shape below is installed as the system cursor.
  private cursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  private localCursorHidden: boolean = false;
  // Host cursor shape installed as the system cursor (HIDE_HOST).
  private cursorPixelMap: image.PixelMap | undefined = undefined;
  private cursorHotX: number = 0;
  private cursorHotY: number = 0;
  private cursorShapeHash: number = 0;
  private cursorShapeHashValid: boolean = false;
  // Toolbar title: the connection's display name (name or host, same as RDP) and
  // the target label the connecting card shows.
  private hostLabel: string = '';
  private targetLabel: string = '';
  // Fixed-dark toolbar material (see component/SessionChrome); a no-op on a
  // system without the material capability, which keeps the flat bar colour.
  private toolbarMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.FLOATING));
  private sessionWindow: window.Window | undefined = undefined;
  // The strip is `toolbarHeight` tall; the band that reveals it is much shorter.
  private revealBand: number = 6;
  private toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  private hoverDelay: number = 700;
  private hideDelay: number = 250;
  // Cleared in aboutToDisappear so a hover-exit during teardown arms nothing.
  private toolbarAutoHideArmed: boolean = true;
  // Fullscreen auto-hide: same controller as the RDP page (services/ToolbarAutoHide);
  // the page supplies live state and mirrors show/reveal back into @State.
  private autoHideHost: ToolbarAutoHideHost = {
    isArmed: () => this.toolbarAutoHideArmed,
    isFullscreen: () => this.isFullscreen,
    isConnected: () => this.state === ConnectionState.Connected,
    isPinned: () => false,
    hoverDelay: () => this.hoverDelay,
    hideDelay: () => this.hideDelay,
    show: () => this.showToolbar,
    setShow: (show: boolean) => {
      this.showToolbar = show;
    },
    revealFill: () => this.revealHintFill,
    animateReveal: (fill: number, durationMs: number, curve: Curve) => {
      this.getUIContext().animateTo({ duration: durationMs, curve: curve }, () => {
        this.revealHintFill = fill;
      });
    }
  };
  private toolbarAutoHide: ToolbarAutoHide = new ToolbarAutoHide(this.autoHideHost);
  // Pending "show the connecting card" timer (see CONNECT_OVERLAY_DELAY_MS); -1
  // when not running.
  private overlayTimer: number = -1;

  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接…';
  // Whether the connecting card has passed its delay and may be shown (failed /
  // disconnected states show immediately).
  @State connectOverlayVisible: boolean = false;
  @State showToolbar: boolean = true;
  @State revealHintFill: number = 0;
  @State isFullscreen: boolean = false;

  aboutToAppear(): void {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    if (request !== undefined) {
      this.connectionId = request.connectionId;
      const conn = request.savedConnection;
      if (conn !== undefined) {
        this.hostLabel = conn.displayName();
        this.targetLabel = conn.targetLabel();
      }
      if (request.stream.width > 0 && request.stream.height > 0) {
        this.streamWidth = request.stream.width;
        this.streamHeight = request.stream.height;
      }
    }
    const backend: SessionBackend | undefined = SessionBackendRegistry.find(this.sessionKey);
    if (backend instanceof MoonlightSessionBackend) {
      this.backend = backend;
    }
    const settings = SettingsStore.load();
    // The toolbar and its auto-hide delays are app-level, shared with RDP.
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.cursorMode = settings.mlCursorMode;
    // Only the local-cursor mode consumes the host cursor channel; the other
    // modes leave the buffer to be released by the backend.
    if (this.cursorMode === ML_CURSOR_HIDE_HOST && this.backend !== undefined) {
      this.backend.setCursorListener((shape: MlCursorShape): void => {
        this.onCursorShape(shape);
      });
    }
    this.loadSessionWindow();
    this.controller.setCallbacks(
      (surfaceId: string, width: number, height: number): void => {
        this.onSurfaceCreated(surfaceId, width, height);
      },
      (surfaceId: string, width: number, height: number): void => {
        // The surface id is stable across resizes; the bound OHNativeWindow
        // stays valid, so nothing to redo here (onSurfaceCreated rebinds). The
        // size is kept for a later retry re-bind.
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        hilog.info(DOMAIN, TAG, 'surface changed %{public}dx%{public}d', width, height);
      },
      (surfaceId: string): void => {
        this.backend?.clearSurface();
      });
    SessionManager.attachWindow(this.sessionKey,
      (state: ConnectionState, message: string): void => {
        this.applyState(state, message);
      },
      (signal: SessionSignal): void => {
        this.onSignal(signal);
      },
      () => {
        this.terminateWindow();
      });
    // Arm the connecting delay for the state the window opened in (Connecting).
    this.updateConnectOverlay();
  }

  aboutToDisappear(): void {
    // The system cursor is process-wide: never leave it hidden for the next page.
    this.setLocalCursorHidden(false);
    this.backend?.clearCursorListener();
    this.resetHostCursor();
    this.toolbarAutoHideArmed = false;
    this.toolbarAutoHide.dispose();
    if (this.overlayTimer !== -1) {
      clearTimeout(this.overlayTimer);
      this.overlayTimer = -1;
    }
  }

  /** Binds the surface and starts the stream (once). */
  private onSurfaceCreated(surfaceId: string, width: number, height: number): void {
    this.surfaceId = surfaceId;
    this.surfaceWidth = width;
    this.surfaceHeight = height;
    const backend: MoonlightSessionBackend | undefined = this.backend;
    if (backend === undefined || this.started) {
      return;
    }
    if (!backend.bindSurface(surfaceId, width, height)) {
      hilog.error(DOMAIN, TAG, 'bind surface failed');
      return;
    }
    this.started = true;
    SessionManager.beginStream(this.connectionId);
  }

  /** Moonlight's stage events are not shown: the card keeps RDP's format. */
  private onSignal(signal: SessionSignal): void {
  }

  /** Renders one manager state, holding back the connecting card (see the const). */
  private applyState(state: ConnectionState, message: string): void {
    this.state = state;
    this.statusText = message;
    // Windowed mode lays the strip out above the picture anyway; hiding it in
    // fullscreen is what the auto-hide then manages.
    this.showToolbar = state !== ConnectionState.Connected;
    // Not streaming: the system cursor must come back (it is process-wide).
    if (state !== ConnectionState.Connected) {
      this.setLocalCursorHidden(false);
    }
    this.updateConnectOverlay();
    this.refreshToolbarAutoHide();
  }

  /** Arms / clears the "show the connecting card" delay. */
  private updateConnectOverlay(): void {
    if (this.state === ConnectionState.Connecting) {
      if (this.connectOverlayVisible || this.overlayTimer !== -1) {
        return;
      }
      this.overlayTimer = setTimeout(() => {
        this.overlayTimer = -1;
        if (this.state === ConnectionState.Connecting) {
          this.connectOverlayVisible = true;
        }
      }, CONNECT_OVERLAY_DELAY_MS);
      return;
    }
    if (this.overlayTimer !== -1) {
      clearTimeout(this.overlayTimer);
      this.overlayTimer = -1;
    }
    this.connectOverlayVisible = false;
  }

  /** Tracks the window's fullscreen state; drives the strip's auto-hide. */
  private loadSessionWindow(): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    WindowController.acquireWindow(context).then((win: window.Window | undefined) => {
      if (win === undefined) {
        return;
      }
      this.sessionWindow = win;
      this.isFullscreen = WindowController.isFullscreen(win);
      WindowController.watchStatusChange(win, (status: window.WindowStatusType) => {
        this.isFullscreen = status === window.WindowStatusType.FULL_SCREEN;
        // The auto-hide only exists in fullscreen: drop or re-arm its timers.
        this.refreshToolbarAutoHide();
      });
    });
  }

  private minimizeWindow(): void {
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    WindowController.minimize(win);
  }

  private toggleFullscreen(): void {
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    WindowController.toggleFullscreen(win, this.isFullscreen);
  }

  /** Reports which zone the pointer is in; zones come from the surface or the bar. */
  private setToolbarPointerZone(zone: number): void {
    this.toolbarAutoHide.setPointerZone(zone);
  }

  /** Re-evaluates the bar's auto-hide (delegates to the shared controller). */
  private refreshToolbarAutoHide(): void {
    this.toolbarAutoHide.refresh();
  }

  /**
   * HIDE_SYSTEM only: hides the HarmonyOS system cursor while the pointer is
   * over the picture and the stream is up, so the host cursor baked into the
   * video is the only one. setPointerVisible is process-wide, so it is always
   * restored off the picture, on disconnect and on teardown. The other modes
   * (OFF / HIDE_HOST) never hide the system cursor here.
   */
  private setLocalCursorHidden(hidden: boolean): void {
    const shouldHide: boolean = hidden && this.cursorMode === ML_CURSOR_HIDE_SYSTEM
      && this.state === ConnectionState.Connected;
    if (this.localCursorHidden === shouldHide) {
      return;
    }
    try {
      pointer.setPointerVisibleSync(!shouldHide);
      this.localCursorHidden = shouldHide;
    } catch (error) {
      // A device without a mouse throws here: stop trying and leave it alone.
      this.cursorMode = ML_CURSOR_OFF;
      hilog.warn(DOMAIN, TAG, 'set pointer visibility failed: %{public}s', JSON.stringify(error));
    }
  }

  /**
   * Installs a host cursor update (HIDE_HOST, foundation-sunshine local cursor
   * mode) as the system cursor: a visibility-only change shows/hides the default
   * pointer, a shape change rebuilds the PixelMap and installs it.
   */
  private onCursorShape(shape: MlCursorShape): void {
    if (this.cursorMode !== ML_CURSOR_HIDE_HOST) {
      return;
    }
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    try {
      if (!shape.hasShape) {
        pointer.setPointerVisibleSync(shape.visible);
        return;
      }
      const bytes: ArrayBuffer | undefined = shape.bgra;
      if (bytes === undefined || shape.width <= 0 || shape.height <= 0
        || shape.width > 256 || shape.height > 256) {
        return;
      }
      const hash: number = MoonlightSessionPage.cursorHash(shape.width, shape.height,
        shape.hotX, shape.hotY, bytes);
      if (this.cursorShapeHashValid && hash === this.cursorShapeHash) {
        return;
      }
      const options: image.InitializationOptions = {
        editable: false,
        pixelFormat: image.PixelMapFormat.BGRA_8888,
        size: { width: shape.width, height: shape.height }
      };
      this.cursorPixelMap = image.createPixelMapSync(bytes, options);
      this.cursorHotX = Math.max(0, Math.min(shape.hotX, shape.width));
      this.cursorHotY = Math.max(0, Math.min(shape.hotY, shape.height));
      this.cursorShapeHash = hash;
      this.cursorShapeHashValid = true;
      const windowId: number = win.getWindowProperties().id;
      pointer.setPointerVisibleSync(true);
      pointer.setCustomCursorSync(windowId, this.cursorPixelMap, this.cursorHotX, this.cursorHotY);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'install host cursor failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Drops the host cursor shape and restores the system pointer on teardown. */
  private resetHostCursor(): void {
    this.cursorPixelMap = undefined;
    this.cursorShapeHashValid = false;
    this.cursorShapeHash = 0;
    if (this.cursorMode === ML_CURSOR_HIDE_HOST) {
      try {
        pointer.setPointerVisibleSync(true);
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'restore cursor failed: %{public}s', JSON.stringify(error));
      }
    }
  }

  /** Dedup key over the cursor geometry and pixels (host resends a shape id). */
  private static cursorHash(width: number, height: number, hotX: number,
    hotY: number, bytes: ArrayBuffer): number {
    const view: Uint8Array = new Uint8Array(bytes);
    let hash: number = 0;
    hash = (hash * 31 + width) | 0;
    hash = (hash * 31 + height) | 0;
    hash = (hash * 31 + hotX) | 0;
    hash = (hash * 31 + hotY) | 0;
    for (let i = 0; i < view.length; i++) {
      hash = (hash * 31 + view[i]) | 0;
    }
    return hash;
  }

  private handleMouse(event: MouseEvent): void {
    // The top reveal band only matters in fullscreen; reporting it always keeps
    // the decision in one place (the controller ignores it outside fullscreen).
    this.setToolbarPointerZone(event.y < this.revealBand
      ? TOOLBAR_ZONE_TRIGGER : TOOLBAR_ZONE_NONE);
    // Over the picture: hide the system cursor so only the host's shows.
    this.setLocalCursorHidden(true);
    const backend: MoonlightSessionBackend | undefined = this.backend;
    if (backend === undefined) {
      return;
    }
    if (this.componentWidth > 0 && this.componentHeight > 0) {
      const x: number = Math.round(event.x / this.componentWidth * this.streamWidth);
      const y: number = Math.round(event.y / this.componentHeight * this.streamHeight);
      backend.mousePosition(x, y, this.streamWidth, this.streamHeight);
    }
    if (event.action === MouseAction.Press || event.action === MouseAction.Release) {
      const action: number = event.action === MouseAction.Press
        ? MlVirtualKey.ACTION_PRESS : MlVirtualKey.ACTION_RELEASE;
      backend.mouseButton(action, MlVirtualKey.buttonFor(event.button));
    }
  }

  private handleAxis(event: AxisEvent): void {
    const direction: number = Math.sign(event.axisVertical ?? 0);
    if (direction !== 0) {
      this.backend?.scroll(direction > 0 ? 1 : -1);
    }
  }

  private handleKey(event: KeyEvent): void {
    const backend: MoonlightSessionBackend | undefined = this.backend;
    if (backend === undefined) {
      return;
    }
    const vk: number = MlVirtualKey.map(event.keyCode);
    if (vk < 0) {
      return;
    }
    const down: boolean = event.type === KeyType.Down;
    const modifier: number = MlVirtualKey.modifierFor(vk);
    if (modifier !== 0) {
      this.modifiers = down ? (this.modifiers | modifier) : (this.modifiers & ~modifier);
    }
    backend.keyVirtual(vk, down, this.modifiers);
  }

  private retry(): void {
    if (this.connectionId.length === 0) {
      return;
    }
    // Stopping a session released its native window (the retry only shows in a
    // Failed/Idle state, i.e. after that stop), so re-bind the XComponent surface
    // — it is still alive — to give the restarted stream a render target again.
    // A second bind while a session is active is refused natively; the manager's
    // state guard keeps retry out of that case.
    const backend: MoonlightSessionBackend | undefined = this.backend;
    if (backend !== undefined && this.surfaceId.length > 0) {
      backend.bindSurface(this.surfaceId, this.surfaceWidth, this.surfaceHeight);
    }
    SessionManager.retry(this.connectionId);
  }

  private disconnect(): void {
    if (this.connectionId.length > 0) {
      SessionManager.disconnect(this.connectionId);
      return;
    }
    this.terminateWindow();
  }

  /** The second disconnect mode: stop the host app (/cancel), then disconnect. */
  private async closeAppAndDisconnect(): Promise<void> {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    const context = this.getUIContext().getHostContext();
    if (request !== undefined && request.savedConnection !== undefined && context !== undefined) {
      const filesDir: string = (context as common.UIAbilityContext).filesDir;
      const ok: boolean = await MoonlightClient.cancel(filesDir, request.savedConnection.host,
        request.moonlightHttpsPort);
      if (ok) {
        // Clear the shared host target now: the app page / list must not flip to
        // 恢复 (as if the app were still running) when this window closes.
        MoonlightHostState.markStopped(request.savedConnection.id);
        MoonlightHostState.refreshAfter(request.savedConnection, filesDir, HOST_REPROBE_DELAY_MS);
      }
    }
    this.disconnect();
  }

  private terminateWindow(): void {
    // Restore before the window goes away (setPointerVisible is process-wide).
    this.setLocalCursorHidden(false);
    this.backend?.clearCursorListener();
    this.resetHostCursor();
    if (this.closing) {
      return;
    }
    this.closing = true;
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    const abilityContext: common.UIAbilityContext = context as common.UIAbilityContext;
    if (WindowController.closeSession(abilityContext)) {
      return;
    }
    abilityContext.terminateSelf().catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'terminate session failed: %{public}s', JSON.stringify(error));
    });
  }

  @Builder
  sessionToolbar() {
    Row() {
      SessionTitle({
        name: this.hostLabel,
        detail: this.streamWidth + ' × ' + this.streamHeight + moonlightDecodeIndicator()
      })
      Blank()
      ToolbarChip({
        label: this.isFullscreen ? '退出全屏' : '全屏',
        onTap: () => {
          this.toggleFullscreen();
        }
      })
        .margin({ right: 8 })
      ToolbarChip({
        label: '最小化',
        onTap: () => {
          this.minimizeWindow();
        }
      })
        .margin({ right: 8 })
      ToolbarChip({
        label: '断开',
        onTap: () => {
          this.disconnect();
        }
      })
        .margin({ right: 8 })
        .bindTips('仅断开与主机的连接，主机上的应用保持运行')
      ToolbarChip({
        label: '关闭应用',
        chipBg: $r('app.color.danger'),
        onTap: () => {
          this.closeAppAndDisconnect();
        }
      })
        .bindTips('关闭主机上正在运行的应用，并断开连接')
    }
    .width('100%')
    .height(TOOLBAR_ROW_HEIGHT)
    .padding({ left: 16, right: 16 })
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : TOOLBAR_BG)
    .attributeModifier(this.toolbarMaterial)
    // In fullscreen the bar covers the surface, so only its own hover reports it.
    .onHover((isHover: boolean) => {
      this.setToolbarPointerZone(isHover ? TOOLBAR_ZONE_BAR : TOOLBAR_ZONE_NONE);
    })
    .transition(TransitionEffect.translate({ y: -this.toolbarHeight })
      .animation({ duration: 180, curve: Curve.EaseOut }))
  }

  /**
   * True while the dark status overlay is up. A connecting attempt only shows it
   * after CONNECT_OVERLAY_DELAY_MS (so a fast start is not a flash); failed /
   * disconnecting / dropped states show it immediately.
   */
  private overlayVisible(): boolean {
    if (this.state === ConnectionState.Connecting) {
      return this.connectOverlayVisible;
    }
    return this.state !== ConnectionState.Connected;
  }

  build() {
    Column() {
      // Windowed mode: the strip is laid out above the picture, so the image
      // starts below it instead of under it. It belongs to a live session only.
      if (!this.isFullscreen && this.state === ConnectionState.Connected) {
        this.sessionToolbar()
      }
      Stack({ alignContent: Alignment.TopStart }) {
        XComponent({
          id: 'moonlightSurface',
          type: XComponentType.SURFACE,
          controller: this.controller
        })
          .width('100%')
          .height('100%')
          .backgroundColor('#000000')
          .focusable(true)
          .defaultFocus(true)
          .onMouse((event: MouseEvent) => {
            this.handleMouse(event);
          })
          .onHover((isHover: boolean) => {
            // The system cursor follows the pointer: hidden over the picture,
            // shown again as soon as it leaves (toolbar / outside the window).
            this.setLocalCursorHidden(isHover);
          })
          .onAxisEvent((event: AxisEvent) => {
            this.handleAxis(event);
          })
          .onKeyEvent((event: KeyEvent) => {
            this.handleKey(event);
          })
          .onAreaChange((oldArea: Area, newArea: Area) => {
            this.componentWidth = lengthToNumber(newArea.width);
            this.componentHeight = lengthToNumber(newArea.height);
          })

        // Fullscreen: the reveal hint hugs the top edge, behind the bar (which
        // slides in over it).
        if (this.isFullscreen && this.state === ConnectionState.Connected) {
          Stack() {
            ToolbarRevealHint({ fill: this.revealHintFill })
          }
          .width('100%')
          .height('100%')
          .alignContent(Alignment.Top)
          .hitTestBehavior(HitTestMode.None)
        }

        // Fullscreen mode: the strip floats over the image and auto-hides.
        if (this.isFullscreen && this.showToolbar && this.state === ConnectionState.Connected) {
          this.sessionToolbar()
        }

        if (this.overlayVisible()) {
          Column() {
            if (this.state === ConnectionState.Connecting) {
              StatusCard({
                title: '正在连接',
                subtitle: this.targetLabel,
                primary: '取消',
                loading: true,
                onPrimary: () => {
                  this.disconnect();
                }
              })
            } else if (this.state === ConnectionState.Disconnecting) {
              StatusCard({
                title: '正在断开',
                subtitle: this.statusText
              })
            } else if (this.state === ConnectionState.Failed) {
              StatusCard({
                title: '连接失败',
                subtitle: this.statusText,
                primary: '重试',
                secondary: '返回',
                icon: $r('app.media.ic_proto_moonlight'),
                onPrimary: () => {
                  this.retry();
                },
                onSecondary: () => {
                  this.disconnect();
                }
              })
            } else {
              StatusCard({
                title: '连接已断开',
                subtitle: this.statusText,
                primary: '重试',
                secondary: '返回',
                icon: $r('app.media.ic_proto_moonlight'),
                onPrimary: () => {
                  this.retry();
                },
                onSecondary: () => {
                  this.disconnect();
                }
              })
            }
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.Center)
          .alignItems(HorizontalAlign.Center)
          .backgroundColor('#66000000')
        }
      }
      .width('100%')
      .layoutWeight(1)
      .clip(true)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
