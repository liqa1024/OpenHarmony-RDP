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
import { moonlightDecodeTokens, moonlightVideoStatus } from '../utils/MoonlightCodec';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { ML_CURSOR_HIDE_HOST, ML_CURSOR_HIDE_SYSTEM, PINCH_MODE_NATIVE, PINCH_MODE_TOUCH,
  PINCH_MODE_WHEEL, PINCH_MODE_ZOOM, AppSettings, DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT,
  DEFAULT_TOOLBAR_WIDTH_PERCENT, SettingsStore } from '../services/SettingsStore';
import { mapPicturePoint } from '../utils/PictureFit';
import { joinSessionTags, srTag } from '../utils/SessionTags';
import { gpuFrameLabel, gpuSplitLabel, usToMs } from '../utils/TelemetryFormat';
import { SessionPicture } from '../components/SessionPicture';
import { StatusCard } from '../components/StatusCard';
import { PadProbeOverlay } from '../components/PadProbeOverlay';
import { MoonlightCounters, MoonlightTelemetryAggregator } from '../services/MoonlightTelemetry';
import { DeviceCapabilities } from '../services/DeviceCapabilities';
import { OVERLOAD_HOST, OVERLOAD_LOCAL, OVERLOAD_NONE, OverloadWatch }
  from '../services/OverloadWatch';
import { AudioPlayer } from '../services/AudioPlayer';
import { ClipboardController, ClipboardControllerHost } from '../services/ClipboardController';
import { MoonlightClipboardBridge } from '../services/MoonlightClipboardBridge';
import { showNotice, showToast } from '../utils/UiUtil';
import { TelemetryField, ToolbarRevealHint, ToolbarTouchBar,
  formatLatency } from '../components/SessionChrome';
import { SessionToolbar } from '../components/SessionToolbar';
import { TOOLBAR_ROW_HEIGHT, TOOLBAR_TELEMETRY_HEIGHT, TOOLBAR_ZONE_NONE,
  TOOLBAR_ZONE_TRIGGER } from '../services/ToolbarContract';

const DOMAIN: number = 0xD004;
const TAG: string = 'HmRdpMoonlightPage';

const CONNECT_OVERLAY_DELAY_MS: number = 1000;
const PINCH_PROBE_PUBLISH_MS: number = 50;

const METRICS_INTERVAL_MS: number = 1000;
const METRIC_WIDTH_NETWORK: number = 66;
const METRIC_WIDTH_HOST: number = 62;
const METRIC_WIDTH_LOCAL: number = 66;
const METRIC_WIDTH_GPU: number = 76;
const METRIC_WIDTH_FPS: number = 62;
const METRIC_WIDTH_FPS_WIDE: number = 78;
const METRIC_WIDTH_BITRATE: number = 132;
const METRIC_WIDTH_DETAIL: number = 82;
const METRIC_GAP_TIGHT: number = 6;
const METRIC_GAP_GROUP: number = 8;
const NETWORK_WARN_MS: number = 100;
const NETWORK_BAD_MS: number = 200;
const LOCAL_FALLBACK_FPS: number = 60;
const LOCAL_BAD_PERIODS: number = 3;
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
  private streamWidth: number = 1920;
  private streamHeight: number = 1080;
  private srToken: string = '';
  private cursorMode: number = ML_CURSOR_HIDE_SYSTEM;
  private relativeMouse: boolean = false;
  private lowLatencyAudio: boolean = true;
  private hostLabel: string = '';
  private targetLabel: string = '';
  private mlPinchMode: number = PINCH_MODE_WHEEL;
  private developerMode: boolean = false;
  private probeTimer: number = -1;
  private probeLiveActive: boolean = false;
  private probeLive: number[] = [0.5, 0.5, 0.5, 0.5];
  private revealBand: number = 6;
  private revealWidthPercent: number = DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT;
  private scrollEngine: boolean = true;
  private touchpadAlwaysEngine: boolean = false;
  private nativeTouchpadSupported: boolean = false;
  private vmouseHintShown: boolean = false;
  private enginePointerX: number = 0;
  private enginePointerY: number = 0;
  private toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  private toolbarWidthPercent: number = DEFAULT_TOOLBAR_WIDTH_PERCENT;
  private hoverDelay: number = 700;
  private hideDelay: number = 250;

  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接…';
  @State connectOverlayVisible: boolean = false;
  @State showToolbar: boolean = true;
  @State toolbarPinned: boolean = false;
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
  @State metricsGpuCopyUs: number = 0;
  @State metricsGpuSrUs: number = 0;
  @State metricsGpuBlitUs: number = 0;
  @State metricsGpuFrameUs: number = 0;
  @State metricsPresentWaitUs: number = 0;
  @State metricsFps: number = 0;
  @State metricsBytesPerSec: number = 0;
  @State metricsPending: number = 0;
  @State metricsDroppedPerSec: number = 0;
  @State metricsLossBp: number = -1;
  private fpsNote: string = '';
  private fpsNoteShown: boolean = false;
  private videoWarningShown: boolean = false;
  private targetFps: number = 0;
  private targetBitrateKbps: number = 0;
  private metricsTimer: number = -1;
  private metricsLastAt: number = 0;
  private telemetry: MoonlightTelemetryAggregator = new MoonlightTelemetryAggregator();
  private overload: OverloadWatch = new OverloadWatch();
  private mlClipboard: MoonlightClipboardBridge | undefined = undefined;
  private clipboard: ClipboardController | undefined = undefined;
  @State clipboardAvailable: boolean = false;

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
    DeviceCapabilities.applyHardwareAccel();
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
        if (config.streamWidth > 0 && config.streamHeight > 0) {
          this.streamWidth = config.streamWidth;
          this.streamHeight = config.streamHeight;
        }
        this.targetFps = config.fps;
        this.targetBitrateKbps = config.bitrateKbps;
        if (config.streamWidth > 0 && config.srRatioPercent > 100) {
          this.srToken = srTag(config.srBackend, config.srRatioPercent);
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
    this.toolbarWidthPercent = settings.toolbarWidthPercent;
    this.cursorMode = settings.mlCursorMode;
    this.relativeMouse = settings.mlRelativeMouse && settings.mlCursorMode === ML_CURSOR_HIDE_SYSTEM;
    this.lowLatencyAudio = settings.mlLowLatencyAudio;
    this.developerMode = settings.developerMode;
    this.scrollEngine = !settings.scrollEngineFallback;
    this.touchpadAlwaysEngine = settings.mlTouchpadScrollAlwaysEngine;
    this.vmouseHintShown = settings.mlVmouseHintShown;
    this.toolbarHeight = this.developerMode
      ? TOOLBAR_ROW_HEIGHT + TOOLBAR_TELEMETRY_HEIGHT * 2
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
      isPinned: (): boolean => this.toolbarPinned,
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
        this.surfaceWidth = width;
        this.surfaceHeight = height;
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
    this.stopAudio();
    this.stopMetricsTimer();
    this.clipboardAvailable = false;
    this.shell?.remoteCursor().endSession();
    if (state === ConnectionState.Idle || state === ConnectionState.Failed) {
      this.refreshHostState();
    }
  }

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
    this.metricsGpuCopyUs = counters.gpuCopyUs;
    this.metricsGpuSrUs = counters.gpuSrUs;
    this.metricsGpuBlitUs = counters.gpuBlitUs;
    this.metricsGpuFrameUs = counters.gpuFrameUs;
    this.metricsPresentWaitUs = counters.presentWaitUs;
    this.metricsFps = counters.fps;
    this.metricsBytesPerSec = counters.bytesPerSec;
    this.metricsPending = counters.pendingVideoFrames;
    this.metricsDroppedPerSec = counters.droppedPerSec;
    this.metricsLossBp = counters.lossBp;
    this.reportVideoWarning();
    this.reportOverload();
  }

  private reportVideoWarning(): void {
    const warning: string = moonlightVideoStatus();
    const videoError: boolean = warning.indexOf('video-present') >= 0;
    const srIssue: boolean = warning.indexOf('sr-unavailable') >= 0
      || warning.indexOf('sr-inactive') >= 0;
    if (!videoError && !srIssue) {
      this.videoWarningShown = false;
      return;
    }
    if (this.videoWarningShown) {
      return;
    }
    if (videoError) {
      this.videoWarningShown = true;
      this.toast('超分视频导入失败，请关闭超分后重连', OVERLOAD_TOAST_MS);
    } else {
      this.videoWarningShown = true;
      this.toast('本机未能启用超分，已按原分辨率显示', OVERLOAD_TOAST_MS);
    }
  }

  private reportOverload(): void {
    const localMs: number = this.localBottleneckMs();
    const overload: number = this.overload.sample(localMs, this.metricsHostMs,
      this.localBadMs());
    if (overload === OVERLOAD_NONE) {
      return;
    }
    if (overload === OVERLOAD_LOCAL) {
      hilog.warn(DOMAIN, TAG, 'local decode/present overloaded: %{public}fms/frame pending=%{public}d drop=%{public}d/s',
        localMs, this.metricsPending, this.metricsDroppedPerSec);
      this.toast('本机处理跟不上，建议降低分辨率或调整超分', OVERLOAD_TOAST_MS);
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

  private localBottleneckMs(): number {
    const gpuMs: number = this.metricsGpuFrameUs / 1000;
    return gpuMs > this.metricsDecodeMs ? gpuMs : this.metricsDecodeMs;
  }

  private gpuLabel(): string {
    return gpuFrameLabel(this.metricsGpuFrameUs);
  }

  private localFrameFps(): number {
    return this.targetFps > 0 ? this.targetFps : LOCAL_FALLBACK_FPS;
  }

  private localBadMs(): number {
    return LOCAL_BAD_PERIODS * 1000 / this.localFrameFps();
  }

  private titleDetail(): string {
    const tokens: string[] = [];
    if (this.srToken.length > 0) {
      tokens.push(this.srToken);
    }
    const codec: string[] = moonlightDecodeTokens();
    for (let i = 0; i < codec.length; i++) {
      tokens.push(codec[i]);
    }
    return this.streamWidth + ' × ' + this.streamHeight + joinSessionTags(tokens);
  }

  private fpsLabel(): string {
    return this.targetFps > 0
      ? 'FPS ' + this.metricsFps + '/' + this.targetFps
      : 'FPS ' + this.metricsFps;
  }

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
    fields.push(new TelemetryField(this.rttLabel(), METRIC_WIDTH_NETWORK, 0));
    fields.push(new TelemetryField(this.hostEncodeLabel(), METRIC_WIDTH_HOST, METRIC_GAP_TIGHT));
    fields.push(new TelemetryField(this.localLabel(), METRIC_WIDTH_LOCAL, METRIC_GAP_GROUP));
    fields.push(new TelemetryField(this.gpuLabel(), METRIC_WIDTH_GPU, METRIC_GAP_TIGHT));
    fields.push(new TelemetryField(this.fpsLabel(), this.fpsWidth(), METRIC_GAP_GROUP));
    fields.push(new TelemetryField(this.bitrateLabel(), METRIC_WIDTH_BITRATE, METRIC_GAP_GROUP));
    const dropped: number = this.sink?.inputDropped() ?? 0;
    if (dropped > 0) {
      fields.push(new TelemetryField('输入 ' + dropped, 0, METRIC_GAP_GROUP));
    }
    return fields;
  }

  private detailFields(): TelemetryField[] {
    const jitter: string = this.metricsJitter < 0 ? '--' : this.metricsJitter + 'ms';
    const loss: string = this.metricsLossBp < 0 ? '--'
      : (this.metricsLossBp / 100).toFixed(2) + '%';
    const fields: TelemetryField[] = [];
    fields.push(new TelemetryField('抖动 ' + jitter, METRIC_WIDTH_DETAIL, 0));
    fields.push(new TelemetryField('丢包 ' + loss, METRIC_WIDTH_DETAIL, METRIC_GAP_TIGHT));
    fields.push(new TelemetryField('在途 ' + this.metricsPending + ' 帧', METRIC_WIDTH_DETAIL,
      METRIC_GAP_GROUP));
    fields.push(new TelemetryField('丢 ' + this.metricsDroppedPerSec + ' 帧/s', METRIC_WIDTH_DETAIL,
      METRIC_GAP_TIGHT));
    if (this.metricsGpuFrameUs > 0) {
      fields.push(new TelemetryField(
        gpuSplitLabel(this.metricsGpuCopyUs, this.metricsGpuSrUs, this.metricsGpuBlitUs, '转换'),
        0, METRIC_GAP_GROUP));
      fields.push(new TelemetryField('等显示 ' + usToMs(this.metricsPresentWaitUs) + 'ms',
        METRIC_WIDTH_DETAIL, METRIC_GAP_TIGHT));
    }
    return fields;
  }

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

  private showFpsNote(): void {
    if (this.fpsNote.length === 0 || this.fpsNoteShown) {
      return;
    }
    this.fpsNoteShown = true;
    showNotice(this.getUIContext(), '帧率已调整', this.fpsNote);
  }

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
    if (backend === undefined) {
      return;
    }
    if (!backend.bindSurface(surfaceId, width, height)) {
      hilog.error(DOMAIN, TAG, 'bind surface failed');
      return;
    }
    if (!this.started) {
      this.started = true;
      SessionManager.beginStream(this.connectionId);
    }
  }

  private onRelease(): void {
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

  private toggleToolbarPin(): void {
    this.toolbarPinned = !this.toolbarPinned;
    this.shell?.refreshAutoHide();
  }

  @Builder
  sessionToolbar() {
    SessionToolbar({
      titleName: this.hostLabel,
      titleDetail: this.titleDetail(),
      isFullscreen: this.isFullscreen,
      pinned: this.toolbarPinned,
      toolbarWidthPercent: this.toolbarWidthPercent,
      toolbarHeight: this.toolbarHeight,
      showTelemetry: this.state === ConnectionState.Connected && this.developerMode,
      telemetryFields: this.telemetryFields(),
      detailFields: this.detailFields(),
      showTransfer: false,
      showClipboard: this.state === ConnectionState.Connected && this.clipboardAvailable,
      showCloseApp: true,
      disconnectTip: '仅断开与主机的连接，主机上的应用保持运行',
      pasteCallback: this.pasteButtonCallback,
      onTogglePin: () => {
        this.toggleToolbarPin();
      },
      onCopy: () => {
        this.clipboard?.copyRemoteToLocal();
      },
      onToggleFullscreen: () => {
        this.shell?.toggleFullscreen();
      },
      onMinimize: () => {
        this.shell?.minimize();
      },
      onDisconnect: () => {
        this.disconnect();
      },
      onCloseApp: () => {
        this.closeAppAndDisconnect();
      },
      onPointerZone: (zone: number) => {
        this.shell?.setPointerZone(zone);
      }
    })
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
          
        }
      }
      .width('100%')
      .layoutWeight(1)
      .clip(true)
    }
    .width('100%')
    .height('100%')
    
  }
}


