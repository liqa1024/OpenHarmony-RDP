import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { PendingConnection, SessionRequests } from '../model/Connections';
import { SessionSignal } from '../model/SessionSignal';
import { ConnectionState, SessionManager } from '../services/SessionManager';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState } from '../services/MoonlightHostState';
import { SessionBackend } from '../services/SessionBackend';
import { ML_HOST_FEATURE_TOUCH, ML_HOST_FEATURE_TOUCHPAD_FRAME, MoonlightInputSink,
  MoonlightSessionBackend } from '../services/MoonlightSessionBackend';
import { SessionInput } from '../services/SessionInput';
import { RemoteCursor } from '../services/RemoteCursor';
import { SessionShellController, SessionSurfaceController } from '../services/SessionShellController';
import { ForegroundReconnect, ForegroundReconnectHost } from '../services/ForegroundReconnect';
import { SessionShellHooks, SessionShellHost } from '../services/SessionViewDelegate';
import { moonlightDecodeIndicator } from '../utils/MoonlightCodec';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { ML_CURSOR_HIDE_HOST, ML_CURSOR_HIDE_SYSTEM, PINCH_MODE_NATIVE, PINCH_MODE_TOUCH,
  PINCH_MODE_WHEEL, PINCH_MODE_ZOOM, AppSettings, SettingsStore } from '../services/SettingsStore';
import { lengthToNumber } from '../utils/UiUtil';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { StatusCard } from '../components/StatusCard';
import { PinchProbeOverlay } from '../components/PinchProbeOverlay';
import { TOOLBAR_BG, TOOLBAR_NEUTRAL_CHIP_BG, TOOLBAR_ROW_HEIGHT, TOOLBAR_ZONE_BAR,
  TOOLBAR_ZONE_NONE, TOOLBAR_ZONE_TRIGGER, SessionTitle, ToolbarChip,
  ToolbarRevealHint } from '../components/SessionChrome';

const DOMAIN: number = 0xD004;
const TAG: string = 'HmRdpMoonlightPage';

// The window opens while still Connecting (LiStartConnection needs the render
// target); hold the connecting card back this long so a fast start does not flash.
const CONNECT_OVERLAY_DELAY_MS: number = 1000;
// The pinch emits frames far faster than the UI needs; the debug overlay is sampled.
const PINCH_PROBE_PUBLISH_MS: number = 50;

@Entry({ useSharedStorage: true })
@Component
struct MoonlightSessionPage {
  @LocalStorageLink('sessionKey') sessionKey: string = '';
  private controller: SessionSurfaceController = new SessionSurfaceController();
  private shell: SessionShellController | undefined = undefined;
  private foreground: ForegroundReconnect | undefined = undefined;
  private connectionId: string = '';
  private started: boolean = false;
  private surfaceId: string = '';
  private surfaceWidth: number = 0;
  private surfaceHeight: number = 0;
  private componentWidth: number = 0;
  private componentHeight: number = 0;
  // Reference frame for absolute mouse positions: the resolved stream size from
  // the request (the 1080p default only covers a request that predates it).
  private streamWidth: number = 1920;
  private streamHeight: number = 1080;
  // ML_CURSOR_*: OFF/HIDE_HOST never hide the system cursor; HIDE_SYSTEM hides
  // it over the picture; HIDE_HOST installs the host-forwarded shape instead.
  private cursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  private hostLabel: string = '';
  private targetLabel: string = '';
  private mlPinchMode: number = PINCH_MODE_WHEEL;
  private developerMode: boolean = false;
  private probeTimer: number = -1;
  private probeLiveActive: boolean = false;
  private probeLive: number[] = [0.5, 0.5, 0.5, 0.5];
  private toolbarMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.FLOATING));
  private revealBand: number = 6;
  private toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  private hoverDelay: number = 700;
  private hideDelay: number = 250;

  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接…';
  @State connectOverlayVisible: boolean = false;
  @State showToolbar: boolean = true;
  @State revealHintFill: number = 0;
  @State isFullscreen: boolean = false;
  @State probeActive: boolean = false;
  @State probeP1X: number = 0.5;
  @State probeP1Y: number = 0.5;
  @State probeP2X: number = 0.5;
  @State probeP2Y: number = 0.5;

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
    const settings: AppSettings = SettingsStore.load();
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.cursorMode = settings.mlCursorMode;
    this.developerMode = settings.developerMode;

    const foregroundHost: ForegroundReconnectHost = {
      context: (): common.Context | undefined => this.getUIContext().getHostContext(),
      state: (): ConnectionState => this.state,
      reconnect: (): void => {
        this.shell?.retry();
      }
    };
    this.foreground = new ForegroundReconnect(foregroundHost, settings.mlReconnectOnForeground);

    const host: SessionShellHost = {
      ui: (): UIContext => this.getUIContext(),
      setState: (state: ConnectionState): void => {
        this.state = state;
      },
      setStatusText: (text: string): void => {
        this.statusText = text;
      },
      setOverlayVisible: (visible: boolean): void => {
        this.connectOverlayVisible = visible;
      },
      setToolbarVisible: (visible: boolean): void => {
        this.showToolbar = visible;
      },
      setFullscreen: (fullscreen: boolean): void => {
        this.isFullscreen = fullscreen;
      },
      setRevealFill: (fill: number): void => {
        this.revealHintFill = fill;
      },
      isFullscreen: (): boolean => this.isFullscreen,
      isToolbarVisible: (): boolean => this.showToolbar,
      revealFill: (): number => this.revealHintFill,
      toolbarHeight: (): number => this.toolbarHeight,
      hoverDelay: (): number => this.hoverDelay,
      hideDelay: (): number => this.hideDelay,
      animateReveal: (fill: number, durationMs: number, curve: Curve): void => {
        this.getUIContext().animateTo({ duration: durationMs, curve: curve }, () => {
          this.revealHintFill = fill;
        });
      }
    };
    const hooks: SessionShellHooks = {
      overlayDelayMs: (): number => CONNECT_OVERLAY_DELAY_MS,
      isPinned: (): boolean => false,
      makeInput: (backend: SessionBackend | undefined, load: AppSettings): SessionInput | undefined =>
        this.makeInput(backend, load),
      configureCursor: (cursor: RemoteCursor, load: AppSettings): void => {
        cursor.setEnabled(load.mlCursorMode === ML_CURSOR_HIDE_HOST);
      },
      onStateChanged: (state: ConnectionState, message: string): void => {
        this.onStateChanged(state, message);
      },
      onSignal: (signal: SessionSignal): void => {
      },
      onSurfaceCreated: (surfaceId: string, width: number, height: number): void => {
        this.onSurfaceCreated(surfaceId, width, height);
      },
      onSurfaceChanged: (width: number, height: number): void => {
        // The surface id is stable across resizes, so the bound OHNativeWindow
        // stays valid; only the size is kept for a retry re-bind.
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        hilog.info(DOMAIN, TAG, 'surface changed %{public}dx%{public}d', width, height);
      },
      onSurfaceDestroyed: (): void => {
        this.backend()?.clearSurface();
      },
      onWindowVisible: (visible: boolean): void => {
      },
      consumesAxis: (event: AxisEvent): boolean => false,
      onReleaseInputs: (): void => {
      },
      onBeforeRetry: (): void => {
        const backend: MoonlightSessionBackend | undefined = this.backend();
        if (backend !== undefined && this.surfaceId.length > 0) {
          backend.bindSurface(this.surfaceId, this.surfaceWidth, this.surfaceHeight);
        }
      },
      onDisconnectFallback: (): void => {
      },
      onRelease: (): void => {
        this.onRelease();
      }
    };
    const shell: SessionShellController =
      new SessionShellController(this.sessionKey, host, hooks, this.controller);
    this.shell = shell;
    shell.attach(settings.mlShortcutPassthrough);
  }

  aboutToDisappear(): void {
    // The system cursor is process-wide: never leave it hidden for the next page.
    this.shell?.release();
  }

  onPageShow(): void {
    this.foreground?.onPageShow();
  }

  onPageHide(): void {
    this.foreground?.onPageHide();
  }

  private backend(): MoonlightSessionBackend | undefined {
    const backend: SessionBackend | undefined = this.shell?.sessionBackend();
    return backend instanceof MoonlightSessionBackend ? backend : undefined;
  }

  private makeInput(backend: SessionBackend | undefined, settings: AppSettings): SessionInput | undefined {
    if (!(backend instanceof MoonlightSessionBackend)) {
      return undefined;
    }
    backend.setCursorListener((update): void => {
      this.shell?.remoteCursor().applyUpdate(update);
    });
    backend.setKeyInterceptListener((keyCode: number, down: boolean): void => {
      const input: SessionInput | undefined = this.shell?.sessionInput();
      input?.handleInterceptedKey(keyCode, down);
    });
    const sink: MoonlightInputSink = new MoonlightInputSink(backend);
    sink.setReferenceSize(this.streamWidth, this.streamHeight);
    const input: SessionInput = new SessionInput(sink);
    input.setWinKeySubstitute(settings.mlWinKeySubstitute);
    input.setPointerSpace(this.streamWidth, this.streamHeight);
    input.setPointMapper((x: number, y: number): number[] => this.mapPoint(x, y));
    input.setTouchpadScrollSpeed(settings.mlTouchpadScrollSpeed);
    input.setTouchpadMiddleDoubleTap(settings.mlTouchpadMiddleDoubleTap);
    input.setPinchZoomSpeed(settings.mlPinchZoomSpeed);
    input.setPinchTouchDistance(settings.mlPinchTouchDistance);
    input.setPinchTouchAngle(settings.mlPinchTouchAngle);
    input.setPinchCursorHandler((active: boolean, zoomIn: boolean): void => {
      this.shell?.remoteCursor().setPinchOverride(active, zoomIn);
    });
    if (this.developerMode) {
      input.setPinchProbeHandler((active: boolean, p1x: number, p1y: number, p2x: number,
        p2y: number): void => {
        this.onPinchProbe(active, p1x, p1y, p2x, p2y);
      });
    }
    this.mlPinchMode = settings.mlPinchMode;
    return input;
  }

  private onStateChanged(state: ConnectionState, message: string): void {
    if (state === ConnectionState.Connected) {
      const backend: MoonlightSessionBackend | undefined = this.backend();
      const features: number = backend !== undefined ? backend.hostFeatureFlags() : 0;
      const touchSupported: boolean = (features & ML_HOST_FEATURE_TOUCH) !== 0;
      const nativeTouchpadSupported: boolean = (features & ML_HOST_FEATURE_TOUCHPAD_FRAME) !== 0;
      const input: SessionInput | undefined = this.shell?.sessionInput();
      input?.setTouchEnabled(touchSupported);
      input?.setPinchMode(this.effectivePinchMode(touchSupported, nativeTouchpadSupported));
      return;
    }
    // Not streaming: the process-wide system cursor must come back.
    this.setLocalCursorHidden(false);
  }

  // Direct zoom is RDP-presenter only; touch-simulate needs host touch; native touchpad needs the
  // foundation-sunshine capability. Unsupported choices fall back to Ctrl+wheel.
  private effectivePinchMode(touchSupported: boolean, nativeTouchpadSupported: boolean): number {
    if (this.mlPinchMode === PINCH_MODE_ZOOM) {
      return PINCH_MODE_WHEEL;
    }
    if (this.mlPinchMode === PINCH_MODE_NATIVE) {
      return nativeTouchpadSupported ? PINCH_MODE_NATIVE : PINCH_MODE_WHEEL;
    }
    if (this.mlPinchMode === PINCH_MODE_TOUCH && !touchSupported) {
      return PINCH_MODE_WHEEL;
    }
    return this.mlPinchMode;
  }

  private onSurfaceCreated(surfaceId: string, width: number, height: number): void {
    this.surfaceId = surfaceId;
    this.surfaceWidth = width;
    this.surfaceHeight = height;
    const backend: MoonlightSessionBackend | undefined = this.backend();
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

  private onRelease(): void {
    // Restore before the window goes away (setPointerVisible is process-wide).
    this.setLocalCursorHidden(false);
    const backend: MoonlightSessionBackend | undefined = this.backend();
    backend?.clearKeyInterceptListener();
    backend?.clearCursorListener();
    this.foreground?.release();
    this.foreground = undefined;
    if (this.probeTimer !== -1) {
      clearTimeout(this.probeTimer);
      this.probeTimer = -1;
    }
  }

  private onPinchProbe(active: boolean, p1x: number, p1y: number, p2x: number, p2y: number): void {
    this.probeLiveActive = active;
    this.probeLive = [p1x, p1y, p2x, p2y];
    if (!active) {
      if (this.probeTimer !== -1) {
        clearTimeout(this.probeTimer);
        this.probeTimer = -1;
      }
      this.publishProbe();
      return;
    }
    if (this.probeTimer === -1) {
      this.probeTimer = setTimeout(() => {
        this.probeTimer = -1;
        this.publishProbe();
      }, PINCH_PROBE_PUBLISH_MS);
    }
  }

  private publishProbe(): void {
    this.probeActive = this.probeLiveActive;
    this.probeP1X = this.probeLive[0];
    this.probeP1Y = this.probeLive[1];
    this.probeP2X = this.probeLive[2];
    this.probeP2Y = this.probeLive[3];
  }

  // HIDE_SYSTEM only: the installer owns the process-global cursor switch and
  // restores it off the picture / on disconnect / on teardown.
  private setLocalCursorHidden(hidden: boolean): void {
    const shouldHide: boolean = hidden && this.cursorMode === ML_CURSOR_HIDE_SYSTEM
      && this.state === ConnectionState.Connected;
    this.shell?.remoteCursor().setLocalHidden(shouldHide);
  }

  private mapPoint(x: number, y: number): number[] {
    if (this.componentWidth <= 0 || this.componentHeight <= 0) {
      return [0, 0];
    }
    const remoteX: number = Math.round(x / this.componentWidth * this.streamWidth);
    const remoteY: number = Math.round(y / this.componentHeight * this.streamHeight);
    return [remoteX, remoteY];
  }

  private handleMouse(event: MouseEvent): void {
    this.shell?.setPointerZone(event.y < this.revealBand
      ? TOOLBAR_ZONE_TRIGGER : TOOLBAR_ZONE_NONE);
    this.setLocalCursorHidden(true);
    this.shell?.routeMouse(event);
  }

  private handleAxis(event: AxisEvent): void {
    this.shell?.routeAxis(event);
  }

  private handleTouch(event: TouchEvent): void {
    this.shell?.routeTouch(event);
  }

  private handleKey(event: KeyEvent): void {
    this.shell?.routeKey(event);
  }

  private retry(): void {
    this.shell?.retry();
  }

  private disconnect(): void {
    if (this.shell !== undefined) {
      this.shell.disconnect();
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
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    const abilityContext: common.UIAbilityContext = context as common.UIAbilityContext;
    abilityContext.terminateSelf().catch((error: BusinessError): void => {
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
          this.shell?.toggleFullscreen();
        }
      })
        .margin({ right: 8 })
      ToolbarChip({
        label: '最小化',
        onTap: () => {
          this.shell?.minimize();
        }
      })
        .margin({ right: 8 })
      ToolbarChip({
        label: '断开',
        chipBg: TOOLBAR_NEUTRAL_CHIP_BG,
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
      this.shell?.setPointerZone(isHover ? TOOLBAR_ZONE_BAR : TOOLBAR_ZONE_NONE);
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
          .onTouch((event: TouchEvent) => {
            this.handleTouch(event);
          })
          .onHover((isHover: boolean) => {
            this.setLocalCursorHidden(isHover);
            if (isHover) {
              // The system may drop a custom cursor when the pointer re-enters,
              // and the host only sends a shape on change, so re-install it.
              this.shell?.remoteCursor().reapply();
            }
          })
          .onAxisEvent((event: AxisEvent) => {
            this.handleAxis(event);
          })
          .onKeyEvent((event: KeyEvent) => {
            this.handleKey(event);
          })
          .onBlur(() => {
            // Keys/buttons held when focus leaves would never get their Up event.
            const input: SessionInput | undefined = this.shell?.sessionInput();
            input?.releaseAll();
          })
          .onAreaChange((oldArea: Area, newArea: Area) => {
            this.componentWidth = lengthToNumber(newArea.width);
            this.componentHeight = lengthToNumber(newArea.height);
          })

        if (this.developerMode) {
          Column() {
            PinchProbeOverlay({
              active: this.probeActive,
              p1x: this.probeP1X,
              p1y: this.probeP1Y,
              p2x: this.probeP2X,
              p2y: this.probeP2Y
            })
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.End)
          .alignItems(HorizontalAlign.Start)
          .padding({ left: 12, bottom: 12 })
          .hitTestBehavior(HitTestMode.None)
        }

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
