import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { PendingConnection, SessionRequests } from '../model/RdpModels';
import { SessionSignal } from '../model/SessionSignal';
import { ConnectionState, SessionManager } from '../services/SessionManager';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState } from '../services/MoonlightHostState';
import { SessionBackend, SessionBackendRegistry } from '../services/SessionBackend';
import { MoonlightSessionBackend } from '../services/MoonlightSessionBackend';
import { RemoteCursor, RemoteCursorUpdate } from '../services/RemoteCursor';
import { MlVirtualKey } from '../utils/MoonlightKeyMapper';
import { moonlightDecodeIndicator } from '../utils/MoonlightCodec';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { WindowController } from '../services/WindowController';
import { ToolbarAutoHide, ToolbarAutoHideHost } from '../services/ToolbarAutoHide';
import { ML_CURSOR_HIDE_HOST, ML_CURSOR_HIDE_SYSTEM,
  SettingsStore } from '../services/SettingsStore';
import { lengthToNumber } from '../utils/UiUtil';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { StatusCard } from '../components/StatusCard';
import { TOOLBAR_BG, TOOLBAR_ROW_HEIGHT, TOOLBAR_ZONE_BAR, TOOLBAR_ZONE_NONE,
  TOOLBAR_ZONE_TRIGGER, SessionTitle, ToolbarChip, ToolbarRevealHint } from '../components/SessionChrome';

const DOMAIN: number = 0xD004;
const TAG: string = 'HmRdpMoonlightPage';

// The window opens while still Connecting (LiStartConnection needs the render
// target); hold the connecting card back this long so a fast start does not flash.
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
  // Kept so a retry can re-bind it: stopping a session releases the native
  // window, but this surface stays alive.
  private surfaceId: string = '';
  private surfaceWidth: number = 0;
  private surfaceHeight: number = 0;
  private componentWidth: number = 0;
  private componentHeight: number = 0;
  // Reference frame for absolute mouse positions: the resolved stream size from
  // the request (the 1080p default only covers a request that predates it).
  private streamWidth: number = 1920;
  private streamHeight: number = 1080;
  private modifiers: number = 0;
  // ML_CURSOR_*: OFF/HIDE_HOST never hide the system cursor; HIDE_SYSTEM hides
  // it over the picture; HIDE_HOST installs the host-forwarded shape instead.
  private cursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  private cursor: RemoteCursor = new RemoteCursor();
  private hostLabel: string = '';
  private targetLabel: string = '';
  // Fixed-dark toolbar material; a no-op without the material capability, which
  // keeps the flat bar colour.
  private toolbarMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.FLOATING));
  private sessionWindow: window.Window | undefined = undefined;
  private revealBand: number = 6;
  private toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  private hoverDelay: number = 700;
  private hideDelay: number = 250;
  private toolbarAutoHideArmed: boolean = true;
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
  private overlayTimer: number = -1;

  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接…';
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
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.cursorMode = settings.mlCursorMode;
    this.cursor.setEnabled(this.cursorMode === ML_CURSOR_HIDE_HOST);
    if (this.backend !== undefined) {
      this.backend.setCursorListener((update: RemoteCursorUpdate): void => {
        this.cursor.applyUpdate(update);
      });
    }
    this.loadSessionWindow();
    this.controller.setCallbacks(
      (surfaceId: string, width: number, height: number): void => {
        this.onSurfaceCreated(surfaceId, width, height);
      },
      (surfaceId: string, width: number, height: number): void => {
        // The surface id is stable across resizes, so the bound OHNativeWindow
        // stays valid; only the size is kept for a retry re-bind.
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
    this.updateConnectOverlay();
  }

  aboutToDisappear(): void {
    // The system cursor is process-wide: never leave it hidden for the next page.
    this.setLocalCursorHidden(false);
    this.backend?.clearCursorListener();
    this.cursor.setWindow(undefined);
    this.cursor.reset();
    this.toolbarAutoHideArmed = false;
    this.toolbarAutoHide.dispose();
    if (this.overlayTimer !== -1) {
      clearTimeout(this.overlayTimer);
      this.overlayTimer = -1;
    }
  }

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

  private onSignal(signal: SessionSignal): void {
  }

  private applyState(state: ConnectionState, message: string): void {
    this.state = state;
    this.statusText = message;
    this.showToolbar = state !== ConnectionState.Connected;
    // Not streaming: the process-wide system cursor must come back.
    if (state !== ConnectionState.Connected) {
      this.setLocalCursorHidden(false);
    }
    this.updateConnectOverlay();
    this.refreshToolbarAutoHide();
  }

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
      this.cursor.setWindow(win);
      this.isFullscreen = WindowController.isFullscreen(win);
      WindowController.watchStatusChange(win, (status: window.WindowStatusType) => {
        this.isFullscreen = status === window.WindowStatusType.FULL_SCREEN;
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

  private setToolbarPointerZone(zone: number): void {
    this.toolbarAutoHide.setPointerZone(zone);
  }

  private refreshToolbarAutoHide(): void {
    this.toolbarAutoHide.refresh();
  }

  // HIDE_SYSTEM only: the installer owns the process-global cursor switch and
  // restores it off the picture / on disconnect / on teardown.
  private setLocalCursorHidden(hidden: boolean): void {
    const shouldHide: boolean = hidden && this.cursorMode === ML_CURSOR_HIDE_SYSTEM
      && this.state === ConnectionState.Connected;
    this.cursor.setLocalHidden(shouldHide);
  }

  private handleMouse(event: MouseEvent): void {
    this.setToolbarPointerZone(event.y < this.revealBand
      ? TOOLBAR_ZONE_TRIGGER : TOOLBAR_ZONE_NONE);
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
    // A stopped session released its native window, so re-bind the still-alive
    // surface for the restarted stream; a second bind while active is refused
    // natively (the manager's state guard keeps retry out of that case).
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

  private async closeAppAndDisconnect(): Promise<void> {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    const context = this.getUIContext().getHostContext();
    if (request !== undefined && request.savedConnection !== undefined && context !== undefined) {
      const filesDir: string = (context as common.UIAbilityContext).filesDir;
      const ok: boolean = await MoonlightClient.cancel(filesDir, request.savedConnection.host,
        request.moonlightHttpsPort);
      if (ok) {
        // Clear the shared host target now, or the list would flip to 恢复 when
        // this window closes.
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
    this.cursor.setWindow(undefined);
    this.cursor.reset();
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

  private overlayVisible(): boolean {
    if (this.state === ConnectionState.Connecting) {
      return this.connectOverlayVisible;
    }
    return this.state !== ConnectionState.Connected;
  }

  build() {
    Column() {
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
            this.setLocalCursorHidden(isHover);
            if (isHover) {
              // The system may drop a custom cursor when the pointer re-enters,
              // and the host only sends a shape on change, so re-install it.
              this.cursor.reapply();
            }
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

        if (this.isFullscreen && this.state === ConnectionState.Connected) {
          Stack() {
            ToolbarRevealHint({ fill: this.revealHintFill })
          }
          .width('100%')
          .height('100%')
          .alignContent(Alignment.Top)
          .hitTestBehavior(HitTestMode.None)
        }

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
