import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { moonlightTelemetry, MoonlightTelemetry,
  moonlightSessionResizeSurface } from 'libhmrdp.so';
import { PendingConnection, SessionRequests } from '../model/Connections';
import { SessionSignal } from '../model/SessionSignal';
import { ConnectionState, SessionManager } from '../services/SessionManager';
import { HOST_REPROBE_DELAY_MS, MoonlightHostState } from '../services/MoonlightHostState';
import { SessionBackend } from '../services/SessionBackend';
import { ML_HOST_FEATURE_CLIPBOARD_IMAGE, ML_HOST_FEATURE_CLIPBOARD_TEXT, ML_HOST_FEATURE_TOUCH,
  ML_HOST_FEATURE_TOUCHPAD_FRAME,
  MoonlightSessionBackend } from '../services/MoonlightSessionBackend';
import { SessionInput } from '../services/SessionInput';
import { SessionInputPort } from '../services/InputPort';
import { InputProtocol } from '../services/InputProtocol';
import { GamepadInput } from '../services/input/GamepadInput';
import { TOUCH_MOVE_INTERVAL_FALLBACK_MS } from '../services/input/InputPolicy';
import { RemoteCursor } from '../services/RemoteCursor';
import { SessionShellController, SessionSurfaceController } from '../services/SessionShellController';
import { ForegroundReconnect, ForegroundReconnectHost } from '../services/ForegroundReconnect';
import { SessionShellHooks, SessionShellHost } from '../services/SessionViewDelegate';
import { moonlightDecodeIndicator } from '../utils/MoonlightCodec';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { ML_CURSOR_HIDE_HOST, ML_CURSOR_HIDE_SYSTEM, PINCH_MODE_NATIVE, PINCH_MODE_TOUCH,
  PINCH_MODE_WHEEL, PINCH_MODE_ZOOM, AppSettings, DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT,
  SettingsStore, srRatioLabel } from '../services/SettingsStore';
import { SR_BACKEND_FSR } from '../model/Profiles';
import { mapPicturePoint } from '../utils/PictureFit';
import { ImmersiveModifier, MaterialSurface } from '../services/SystemMaterial';
import { SessionPicture } from '../components/SessionPicture';
import { StatusCard } from '../components/StatusCard';
import { PadProbeOverlay } from '../components/PadProbeOverlay';
import { MoonlightCounters, MoonlightTelemetryAggregator } from '../services/MoonlightTelemetry';
import { OVERLOAD_HOST, OVERLOAD_LOCAL, OVERLOAD_NONE, OverloadWatch }
  from '../services/OverloadWatch';
import { AudioPlayer } from '../services/AudioPlayer';
import { ClipboardController, ClipboardControllerHost } from '../services/ClipboardController';
import { MoonlightClipboardBridge } from '../services/MoonlightClipboardBridge';
import { showNotice, showToast } from '../utils/UiUtil';
import { ClipboardButtons, METRIC_BAD, TOOLBAR_BG, TOOLBAR_FG_SECONDARY, TOOLBAR_NEUTRAL_CHIP_BG, TOOLBAR_ROW_HEIGHT,
  TOOLBAR_TELEMETRY_HEIGHT, TOOLBAR_ZONE_BAR, TOOLBAR_ZONE_NONE, TOOLBAR_ZONE_TRIGGER,
  TelemetryField, TelemetryRow, SessionTitle, ToolbarChip, ToolbarRevealHint,
  ToolbarTouchBar,
  formatLatency, metricColor } from '../components/SessionChrome';

const DOMAIN: number = 0xD004;
const TAG: string = 'HmRdpMoonlightPage';

// The window opens while still Connecting (LiStartConnection needs the render
// target); hold the connecting card back this long so a fast start does not flash.
const CONNECT_OVERLAY_DELAY_MS: number = 1000;
// The pinch emits frames far faster than the UI needs; the debug overlay is sampled.
const PINCH_PROBE_PUBLISH_MS: number = 50;

const METRICS_INTERVAL_MS: number = 1000;
// Fixed widths so a changing digit count never reflows the fields after it.
const METRIC_WIDTH_NETWORK: number = 66;
const METRIC_WIDTH_HOST: number = 62;
const METRIC_WIDTH_LOCAL: number = 66;
const METRIC_WIDTH_FPS: number = 62;
const METRIC_WIDTH_FPS_WIDE: number = 78;
const METRIC_WIDTH_BITRATE: number = 132;
// Fixed width for one detail reading (`在途 12`); the digits vary, so a content-width field would
// shift its neighbours on every sample.
const METRIC_WIDTH_DETAIL: number = 82;
const METRIC_GAP_TIGHT: number = 6;
const METRIC_GAP_GROUP: number = 8;
// Latency colour thresholds: network RTT warns late (distance-dominated). The two
// local readings include codec/output-buffer wait, so colour only past a multiple of the frame period.
const NETWORK_WARN_MS: number = 100;
const NETWORK_BAD_MS: number = 200;
const LOCAL_FALLBACK_FPS: number = 60;
const LOCAL_WARN_PERIODS: number = 2;
const LOCAL_BAD_PERIODS: number = 3;
// Long enough to read one line of advice, short enough not to sit over the picture.
const OVERLOAD_TOAST_MS: number = 2500;

@Entry({ useSharedStorage: true })
@Component
struct MoonlightSessionPage {
  @LocalStorageLink('sessionKey') sessionKey: string = '';
  private controller: SessionSurfaceController = new SessionSurfaceController();
  private shell: SessionShellController | undefined = undefined;
  private sink: SessionInputPort | undefined = undefined;
  private foreground: ForegroundReconnect | undefined = undefined;
  private connectionId: string = '';
  private started: boolean = false;
  private audioStarted: boolean = false;
  private surfaceId: string = '';
  private surfaceWidth: number = 0;
  private surfaceHeight: number = 0;
  private componentWidth: number = 0;
  private componentHeight: number = 0;
  // Reference frame for absolute mouse positions: the resolved stream size from
  // the request (the 1080p default only covers a request that predates it).
  private streamWidth: number = 1920;
  private streamHeight: number = 1080;
  // 超分 indicator for the toolbar title (" (FSR 1.5x)"), "" when the session does not upscale.
  private srIndicator: string = '';
  // ML_CURSOR_*: OFF/HIDE_HOST never hide the system cursor; HIDE_SYSTEM hides
  // it over the picture; HIDE_HOST installs the host-forwarded shape instead.
  private cursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  // HIDE_SYSTEM only: the host cursor owns its position and Windows applies its own pointer curve.
  private relativeMouse: boolean = false;
  // When true the native renderer plays, so this page starts no ArkTS renderer.
  private lowLatencyAudio: boolean = true;
  private hostLabel: string = '';
  private targetLabel: string = '';
  private mlPinchMode: number = PINCH_MODE_WHEEL;
  private developerMode: boolean = false;
  private probeTimer: number = -1;
  private probeLiveActive: boolean = false;
  private probeLive: number[] = [0.5, 0.5, 0.5, 0.5];
  private toolbarMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(MaterialSurface.FLOATING, TOOLBAR_BG);
  private revealBand: number = 6;
  private revealWidthPercent: number = DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT;
  private scrollEngine: boolean = true;
  private touchpadAlwaysEngine: boolean = false;
  private nativeTouchpadSupported: boolean = false;
  private vmouseHintShown: boolean = false;
  private enginePointerX: number = 0;
  private enginePointerY: number = 0;
  private toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  private hoverDelay: number = 700;
  private hideDelay: number = 250;

  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接…';
  @State connectOverlayVisible: boolean = false;
  @State showToolbar: boolean = true;
  @State revealHintFill: number = 0;
  @State touchBarFill: number = 0;
  @State isFullscreen: boolean = false;
  @State probeActive: boolean = false;
  @State probeNearPointer: boolean = false;
  @State probeP1X: number = 0.5;
  @State probeP1Y: number = 0.5;
  @State probeP2X: number = 0.5;
  @State probeP2Y: number = 0.5;
  @State metricsRtt: number = -1;
  @State metricsJitter: number = -1;
  @State metricsHostMs: number = -1;
  @State metricsDecodeMs: number = -1;
  @State metricsFps: number = 0;
  @State metricsBytesPerSec: number = 0;
  @State metricsPending: number = 0;
  @State metricsDroppedPerSec: number = 0;
  @State metricsLossBp: number = -1;
  // Why this session's frame rate was capped; shown once when it becomes the live stream.
  private fpsNote: string = '';
  private fpsNoteShown: boolean = false;
  private targetFps: number = 0;
  private targetBitrateKbps: number = 0;
  private metricsTimer: number = -1;
  private metricsLastAt: number = 0;
  private telemetry: MoonlightTelemetryAggregator = new MoonlightTelemetryAggregator();
  private overload: OverloadWatch = new OverloadWatch();
  private mlClipboard: MoonlightClipboardBridge | undefined = undefined;
  private clipboard: ClipboardController | undefined = undefined;
  @State clipboardAvailable: boolean = false;

  // The system paste control grants a one-time clipboard read without the READ_PASTEBOARD permission.
  private pasteButtonCallback: PasteButtonCallback =
    (event: ClickEvent, result: PasteButtonOnClickResult): void => {
      if (result === PasteButtonOnClickResult.SUCCESS) {
        this.clipboard?.pasteLocalToRemote();
        return;
      }
      hilog.warn(DOMAIN, TAG, 'paste authorization failed');
      this.toast('未获得剪贴板授权');
    };

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
      this.fpsNote = request.moonlightFpsNote;
      const config = request.moonlightConfig;
      if (config !== undefined) {
        // 超分: the picture and the input reference use the *session* (pre-upscale) resolution, like
        // the RDP session page; the presenter upscales back to the requested output size.
        if (config.streamWidth > 0 && config.streamHeight > 0) {
          this.streamWidth = config.streamWidth;
          this.streamHeight = config.streamHeight;
        }
        this.targetFps = config.fps;
        this.targetBitrateKbps = config.bitrateKbps;
        if (config.streamWidth > 0 && config.srRatioPercent > 100) {
          const backend: string = config.srBackend === SR_BACKEND_FSR ? 'FSR' : 'XEngine';
          this.srIndicator = ' (' + backend + ' ' + srRatioLabel(config.srRatioPercent) + ')';
        }
      }
    }
    if (request !== undefined) {
      const clipHost: string = request.savedConnection !== undefined ? request.savedConnection.host : '';
      this.mlClipboard = new MoonlightClipboardBridge(clipHost, request.moonlightHttpsPort,
        request.filesDir);
      this.clipboard = new ClipboardController(this.mlClipboard, this.makeClipboardHost());
    }
    const settings: AppSettings = SettingsStore.load();
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.revealWidthPercent = settings.toolbarTriggerWidthPercent;
    this.cursorMode = settings.mlCursorMode;
    this.relativeMouse = settings.mlRelativeMouse && settings.mlCursorMode === ML_CURSOR_HIDE_SYSTEM;
    this.lowLatencyAudio = settings.mlLowLatencyAudio;
    this.developerMode = settings.developerMode;
    this.scrollEngine = !settings.scrollEngineFallback;
    this.touchpadAlwaysEngine = settings.mlTouchpadScrollAlwaysEngine;
    this.vmouseHintShown = settings.mlVmouseHintShown;
    this.toolbarHeight = this.developerMode
      ? TOOLBAR_ROW_HEIGHT + TOOLBAR_TELEMETRY_HEIGHT
      : TOOLBAR_ROW_HEIGHT;

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
      animateTouchBar: (fill: number, durationMs: number, curve: Curve): void => {
        this.getUIContext().animateTo({ duration: durationMs, curve: curve }, () => {
          this.touchBarFill = fill;
        });
      },
      showTouchTip: (): void => {
        this.toast('再次下滑显示工具栏');
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
        // 超分: the presenter owns the swapchain, so the resize has to reach it (a no-op on the
        // direct-to-surface path, which the window scaling mode fits by itself).
        moonlightSessionResizeSurface(width, height);
        hilog.info(DOMAIN, TAG, 'surface changed %{public}dx%{public}d', width, height);
      },
      onSurfaceDestroyed: (): void => {
        this.backend()?.clearSurface();
      },
      onWindowVisible: (visible: boolean): void => {
      },
      consumesAxis: (event: AxisEvent): boolean =>
        this.shell?.sessionInput()?.consumesAxis(event) ?? false,
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
    this.stopAudio();
    this.backend()?.clearClipboardListener();
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

  private toast(message: string, durationMs: number = 1500): void {
    showToast(this.getUIContext(), message, durationMs);
  }

  // No file transfer in Moonlight, so the transfer hooks are inert.
  private makeClipboardHost(): ClipboardControllerHost {
    const clipboardHost: ClipboardControllerHost = {
      toast: (message: string): void => {
        this.toast(message);
      },
      confirmCancelUpload: (onConfirm: () => void): void => {
        onConfirm();
      },
      setTransferProgress: (direction: number, percent: number, text: string,
        speedBytesPerSecond: number, fileIndex: number, fileCount: number): void => {
      },
      setTransferWaiting: (direction: number, fileIndex: number, fileCount: number): void => {
      },
      clearTransfer: (): void => {
      },
      onRemoteChanged: (): void => {
      }
    };
    return clipboardHost;
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
    backend.setClipboardListener((bytes: ArrayBuffer): void => {
      this.mlClipboard?.onIncoming(bytes);
    });
    const sink: SessionInputPort = new SessionInputPort(InputProtocol.MOONLIGHT, 0,
      backend.hostDisplayScalePercent());
    sink.setReferenceSize(this.streamWidth, this.streamHeight);
    this.sink = sink;
    const input: SessionInput = new SessionInput(sink);
    // Moonlight only: OH reports gamepad buttons as key events and sticks as focus axis events.
    input.setGamepad(new GamepadInput(sink));
    input.setEngineEnabled(this.scrollEngine);
    input.setWinKeySubstitute(settings.mlWinKeySubstitute);
    input.setPointerSpace(this.streamWidth, this.streamHeight);
    input.setPointMapper((x: number, y: number): number[] => this.mapPoint(x, y));
    input.setTouchMoveIntervalMs(TOUCH_MOVE_INTERVAL_FALLBACK_MS);
    input.setTouchpadScrollSpeed(settings.mlTouchpadScrollSpeed);
    input.setTouchpadMiddleDoubleTap(settings.mlTouchpadMiddleDoubleTap);
    input.setRelativePointer(this.relativeMouse);
    input.setRelativeSpeed(settings.mlRelativeSpeed);
    input.setPinchZoomSpeed(settings.mlPinchZoomSpeed);
    input.setPinchTouchDistance(settings.mlPinchTouchDistance);
    input.setPinchTouchAngle(settings.mlPinchTouchAngle);
    input.setPinchCursorHandler((active: boolean, zoomIn: boolean): void => {
      this.shell?.remoteCursor().setPinchOverride(active, zoomIn);
    });
    if (this.developerMode) {
      input.setPadProbeHandler((active: boolean, p1x: number, p1y: number, p2x: number,
        p2y: number): void => {
        this.onPadProbe(active, p1x, p1y, p2x, p2y);
      });
    }
    this.mlPinchMode = settings.mlPinchMode;
    return input;
  }

  private onStateChanged(state: ConnectionState, message: string): void {
    if (state === ConnectionState.Connected) {
      this.shell?.remoteCursor().beginSession();
      const backend: MoonlightSessionBackend | undefined = this.backend();
      const features: number = backend !== undefined ? backend.hostFeatureFlags() : 0;
      if (backend !== undefined) {
        this.sink?.setHostScale(backend.hostDisplayScalePercent());
      }
      const touchSupported: boolean = (features & ML_HOST_FEATURE_TOUCH) !== 0;
      const nativeTouchpadSupported: boolean = (features & ML_HOST_FEATURE_TOUCHPAD_FRAME) !== 0;
      // Only when the host advertises it (foundation-sunshine with its GUI agent alive).
      this.clipboardAvailable =
        (features & (ML_HOST_FEATURE_CLIPBOARD_TEXT | ML_HOST_FEATURE_CLIPBOARD_IMAGE)) !== 0;
      const input: SessionInput | undefined = this.shell?.sessionInput();
      input?.setTouchEnabled(touchSupported);
      input?.setPinchMode(this.effectivePinchMode(touchSupported, nativeTouchpadSupported));
      this.nativeTouchpadSupported = nativeTouchpadSupported;
      input?.setTouchpadPanEnabled(!this.touchpadAlwaysEngine && nativeTouchpadSupported);
      if (this.relativeMouse) {
        input?.reportPointerPosition();
      }
      this.suggestVmouseOff(nativeTouchpadSupported);
      this.showFpsNote();
      this.startMetricsTimer();
      this.startAudio();
      return;
    }
    // Not streaming: the process-wide system cursor must come back, and a late
    // host hide must not bring it down again while the window stays up.
    this.stopAudio();
    this.stopMetricsTimer();
    this.clipboardAvailable = false;
    this.shell?.remoteCursor().endSession();
    if (state === ConnectionState.Idle || state === ConnectionState.Failed) {
      this.refreshHostState();
    }
  }

  // The native config gate (enableAudio + output device) decides whether any
  // audio flows; a disabled stream never reports a format, so the worker builds
  // no renderer and this is a harmless no-op.
  private startAudio(): void {
    if (!this.lowLatencyAudio && !this.audioStarted) {
      this.audioStarted = true;
      AudioPlayer.startMoonlight();
    }
  }

  private stopAudio(): void {
    if (this.audioStarted) {
      this.audioStarted = false;
      AudioPlayer.stopMoonlight();
    }
  }

  private refreshHostState(): void {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    const context = this.getUIContext().getHostContext();
    if (request === undefined || request.savedConnection === undefined || context === undefined) {
      return;
    }
    MoonlightHostState.refresh(request.savedConnection,
      (context as common.UIAbilityContext).filesDir);
  }

  private startMetricsTimer(): void {
    if (this.metricsTimer !== -1 || !this.developerMode) {
      return;
    }
    this.metricsLastAt = Date.now();
    this.metricsTimer = setInterval(() => {
      this.refreshMetrics();
    }, METRICS_INTERVAL_MS);
  }

  private stopMetricsTimer(): void {
    if (this.metricsTimer === -1) {
      return;
    }
    clearInterval(this.metricsTimer);
    this.metricsTimer = -1;
  }

  private refreshMetrics(): void {
    const now: number = Date.now();
    const windowMs: number = this.metricsLastAt > 0 ? now - this.metricsLastAt : METRICS_INTERVAL_MS;
    this.metricsLastAt = now;
    const raw: MoonlightTelemetry = moonlightTelemetry();
    const counters: MoonlightCounters = this.telemetry.sample(raw, windowMs,
      this.sink?.inputDropped() ?? 0);
    this.metricsRtt = counters.rttMs;
    this.metricsJitter = counters.rttVarianceMs;
    this.metricsHostMs = counters.hostLatencyMs;
    this.metricsDecodeMs = counters.decodeMs;
    this.metricsFps = counters.fps;
    this.metricsBytesPerSec = counters.bytesPerSec;
    this.metricsPending = counters.pendingVideoFrames;
    this.metricsDroppedPerSec = counters.droppedPerSec;
    this.metricsLossBp = counters.lossBp;
    this.reportOverload();
  }

  // The readings colour themselves; what a colour cannot say is what to do about it, so a side that
  // stays red gets one prompt. Both sides read per-frame wall clock against the frame period (see
  // doc_agent/moonlight-session.md §2).
  private reportOverload(): void {
    const overload: number = this.overload.sample(this.metricsDecodeMs, this.metricsHostMs,
      this.localBadMs());
    if (overload === OVERLOAD_NONE) {
      return;
    }
    // One line, one action: fewer pixels is the strongest lever on either side (less to decode
    // locally, less to encode on the host); super resolution turns that into "lower the resolution
    // and upscale" — the lower stream costs both sides fewer pixels.
    if (overload === OVERLOAD_LOCAL) {
      hilog.warn(DOMAIN, TAG, 'local decode overloaded: %{public}fms/frame pending=%{public}d drop=%{public}d/s',
        this.metricsDecodeMs, this.metricsPending, this.metricsDroppedPerSec);
      this.toast('本机解码跟不上，建议降低分辨率并开启超分', OVERLOAD_TOAST_MS);
      return;
    }
    hilog.warn(DOMAIN, TAG, 'host encode overloaded: %{public}fms per frame', this.metricsHostMs);
    this.toast('主机编码跟不上，建议降低分辨率并开启超分', OVERLOAD_TOAST_MS);
  }

  private rttLabel(): string {
    return formatLatency('网络', this.metricsRtt);
  }

  private hostEncodeLabel(): string {
    return formatLatency('主机', this.metricsHostMs);
  }

  private localLabel(): string {
    return formatLatency('本机', this.metricsDecodeMs);
  }

  private localFrameFps(): number {
    return this.targetFps > 0 ? this.targetFps : LOCAL_FALLBACK_FPS;
  }

  private localWarnMs(): number {
    return LOCAL_WARN_PERIODS * 1000 / this.localFrameFps();
  }

  private localBadMs(): number {
    return LOCAL_BAD_PERIODS * 1000 / this.localFrameFps();
  }

  private fpsLabel(): string {
    return this.targetFps > 0
      ? 'FPS ' + this.metricsFps + '/' + this.targetFps
      : 'FPS ' + this.metricsFps;
  }

  // A 3-digit target widens the current/target pair (`FPS 59/120`); keeping the narrow
  // width otherwise avoids a gap when the target is 2-digit. The target is fixed per session.
  private fpsWidth(): number {
    return this.targetFps >= 100 ? METRIC_WIDTH_FPS_WIDE : METRIC_WIDTH_FPS;
  }

  private bitrateLabel(): string {
    const currentKbps: number = this.metricsBytesPerSec * 8 / 1000;
    if (this.targetBitrateKbps >= 1000) {
      return '码率 ' + (currentKbps / 1000).toFixed(1) + '/'
        + (this.targetBitrateKbps / 1000).toFixed(1) + 'Mbps';
    }
    return '码率 ' + Math.round(currentKbps) + '/' + Math.round(this.targetBitrateKbps) + 'kbps';
  }

  private telemetryFields(): TelemetryField[] {
    const fields: TelemetryField[] = [];
    fields.push(new TelemetryField(this.rttLabel(), METRIC_WIDTH_NETWORK,
      metricColor(this.metricsRtt, NETWORK_WARN_MS, NETWORK_BAD_MS), 0));
    fields.push(new TelemetryField(this.hostEncodeLabel(), METRIC_WIDTH_HOST,
      metricColor(this.metricsHostMs, this.localWarnMs(), this.localBadMs()), METRIC_GAP_TIGHT));
    fields.push(new TelemetryField(this.localLabel(), METRIC_WIDTH_LOCAL,
      metricColor(this.metricsDecodeMs, this.localWarnMs(), this.localBadMs()), METRIC_GAP_GROUP));
    fields.push(new TelemetryField(this.fpsLabel(), this.fpsWidth(),
      TOOLBAR_FG_SECONDARY, METRIC_GAP_GROUP));
    fields.push(new TelemetryField(this.bitrateLabel(), METRIC_WIDTH_BITRATE,
      TOOLBAR_FG_SECONDARY, METRIC_GAP_GROUP));
    // Input budget drops are invisible otherwise: any non-zero count names a stream over its budget.
    const dropped: number = this.sink?.inputDropped() ?? 0;
    if (dropped > 0) {
      fields.push(new TelemetryField('输入 ' + dropped, 0, METRIC_BAD, METRIC_GAP_GROUP));
    }
    return fields;
  }

  // What the readings above hide, in the same order as the readings themselves: the network's
  // jitter and loss behind the RTT, then the decode queue state behind the local round trip. A row,
  // not a tooltip: a bound tooltip's text is read once, so a live number in one is stuck at whatever
  // it was when the pointer first arrived.
  private detailFields(): TelemetryField[] {
    const jitter: string = this.metricsJitter < 0 ? '--' : this.metricsJitter + 'ms';
    const loss: string = this.metricsLossBp < 0 ? '--'
      : (this.metricsLossBp / 100).toFixed(2) + '%';
    const fields: TelemetryField[] = [];
    fields.push(new TelemetryField('抖动 ' + jitter, METRIC_WIDTH_DETAIL, TOOLBAR_FG_SECONDARY, 0));
    fields.push(new TelemetryField('丢包 ' + loss, METRIC_WIDTH_DETAIL, TOOLBAR_FG_SECONDARY,
      METRIC_GAP_TIGHT));
    fields.push(new TelemetryField('在途 ' + this.metricsPending + ' 帧', METRIC_WIDTH_DETAIL,
      TOOLBAR_FG_SECONDARY, METRIC_GAP_GROUP));
    fields.push(new TelemetryField('丢 ' + this.metricsDroppedPerSec + ' 帧/s', METRIC_WIDTH_DETAIL,
      TOOLBAR_FG_SECONDARY, METRIC_GAP_TIGHT));
    return fields;
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

  // A capped rate is a visible change to what was asked for, so say so once per session.
  private showFpsNote(): void {
    if (this.fpsNote.length === 0 || this.fpsNoteShown) {
      return;
    }
    this.fpsNoteShown = true;
    showNotice(this.getUIContext(), '帧率已调整', this.fpsNote);
  }

  // 0x20 is the only base-version marker a client can read; whether the host's virtual mouse driver is
  // on cannot be probed (see doc_agent/moonlight-native.md §2.2), hence a one-time suggestion.
  private suggestVmouseOff(nativeTouchpadSupported: boolean): void {
    if (!nativeTouchpadSupported || this.vmouseHintShown) {
      return;
    }
    this.vmouseHintShown = true;
    SettingsStore.rememberVmouseHintShown();
    showNotice(this.getUIContext(), '触控板滚动建议',
      '主机若开启「虚拟鼠标驱动」，触控板滚动会被整成整齿。\n'
        + '建议在主机 GUI 关闭它（或把鼠标模式设为 SendInput），再到设置里开启「总是使用系统滑动组件」。');
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
    this.stopMetricsTimer();
    if (this.probeTimer !== -1) {
      clearTimeout(this.probeTimer);
      this.probeTimer = -1;
    }
  }

  private onPadProbe(active: boolean, p1x: number, p1y: number, p2x: number, p2y: number): void {
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
    const shouldHide: boolean = hidden
      && this.cursorMode === ML_CURSOR_HIDE_SYSTEM && this.state === ConnectionState.Connected;
    this.shell?.remoteCursor().setLocalHidden(shouldHide);
  }

  private mapPoint(x: number, y: number): number[] {
    return mapPicturePoint(x, y, this.componentWidth, this.componentHeight,
      this.streamWidth, this.streamHeight);
  }

  private onRevealZone(inBand: boolean): void {
    this.shell?.setPointerZone(inBand ? TOOLBAR_ZONE_TRIGGER : TOOLBAR_ZONE_NONE);
  }

  private handleMouse(event: MouseEvent): void {
    this.setLocalCursorHidden(true);
    this.shell?.routeMouse(event);
  }

  private handleAxis(event: AxisEvent): void {
    if (this.scrollEngine && !SessionInput.isPinchEvent(event)
      && ((event.axisVertical ?? 0) !== 0 || (event.axisHorizontal ?? 0) !== 0)) {
      // The engine's offsets carry no position, so remember the pointer for its wheel events.
      const point: number[] = this.mapPoint(event.x, event.y);
      this.enginePointerX = point[0];
      this.enginePointerY = point[1];
    }
    this.shell?.routeAxis(event);
  }

  private handleEngineScroll(dx: number, dy: number): void {
    const input: SessionInput | undefined = this.shell?.sessionInput();
    if (!(input?.engineAcceptsFrame() ?? false)) {
      return;
    }
    input?.handleEngineScroll(dx, dy, this.enginePointerX, this.enginePointerY);
  }

  private handleTouch(event: TouchEvent): void {
    this.shell?.routeTouch(event);
  }

  private handleKey(event: KeyEvent): void {
    this.shell?.routeKey(event);
  }

  private handleFocusAxis(event: FocusAxisEvent): void {
    this.shell?.routeFocusAxis(event);
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
    Column() {
      Row() {
        SessionTitle({
          name: this.hostLabel,
          detail: this.streamWidth + ' × ' + this.streamHeight + this.srIndicator
            + moonlightDecodeIndicator()
        })
        Blank()
        if (this.state === ConnectionState.Connected && this.clipboardAvailable) {
          ClipboardButtons({
            onCopy: (): void => {
              this.clipboard?.copyRemoteToLocal();
            },
            pasteCallback: this.pasteButtonCallback
          })
          Row().width(16)
        }
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
      if (this.state === ConnectionState.Connected && this.developerMode) {
        TelemetryRow({ fields: this.telemetryFields() })
        TelemetryRow({ fields: this.detailFields() })
      }
    }
    .width('100%')
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
        SessionPicture({
          xComponentId: 'moonlightSurface',
          pictureController: this.controller,
          contentWidth: this.streamWidth,
          contentHeight: this.streamHeight,
          revealBand: (this.isFullscreen && !this.showToolbar) ? this.revealBand : 0,
          revealBandWidth: this.revealWidthPercent,
          onPointerMouse: (event: MouseEvent) => {
            this.handleMouse(event);
          },
          onPointerTouch: (event: TouchEvent) => {
            this.handleTouch(event);
          },
          onPointerHover: (isHover: boolean) => {
            this.setLocalCursorHidden(isHover);
            if (isHover) {
              // The system may drop a custom cursor when the pointer re-enters,
              // and the host only sends a shape on change, so re-install it.
              this.shell?.remoteCursor().reapply();
            }
          },
          onPointerAxis: (event: AxisEvent) => {
            this.handleAxis(event);
          },
          engineScroll: this.scrollEngine,
          onEngineScroll: (dx: number, dy: number) => {
            this.handleEngineScroll(dx, dy);
          },
          onSurfaceKey: (event: KeyEvent) => {
            this.handleKey(event);
          },
          onSurfaceFocusAxis: (event: FocusAxisEvent) => {
            this.handleFocusAxis(event);
          },
          onSurfaceBlur: () => {
            // Keys/buttons held when focus leaves would never get their Up event.
            const input: SessionInput | undefined = this.shell?.sessionInput();
            input?.releaseAll();
          },
          onPictureArea: (width: number, height: number) => {
            this.componentWidth = width;
            this.componentHeight = height;
          },
          onRevealZone: (inBand: boolean) => {
            this.onRevealZone(inBand);
          }
        })

        if (this.developerMode) {
          Column() {
            PadProbeOverlay({
              p1x: this.probeP1X,
              p1y: this.probeP1Y,
              p2x: this.probeP2X,
              p2y: this.probeP2Y,
              onHoverChange: (hover: boolean): void => {
                this.probeNearPointer = hover;
              }
            })
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.End)
          .alignItems(HorizontalAlign.Start)
          .padding({ left: 12, bottom: 12 })
          .hitTestBehavior(HitTestMode.None)
          // Marks a live pinch only; yields to the pointer so it never sits under the cursor.
          .opacity(this.probeActive && !this.probeNearPointer ? 1 : 0)
        }

        if (this.isFullscreen && this.state === ConnectionState.Connected) {
          Stack() {
            ToolbarRevealHint({ fill: this.revealHintFill })
            ToolbarTouchBar({ fill: this.touchBarFill })
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
