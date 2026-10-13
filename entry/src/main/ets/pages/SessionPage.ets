import { common } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { matrix4 } from '@kit.ArkUI';
import { BusinessError, pasteboard, SuppressWarnings, SuppressWarningsType } from '@kit.BasicServicesKit';
import { fileIo, fileUri } from '@kit.CoreFileKit';
import { hidebug, hilog } from '@kit.PerformanceAnalysisKit';
import { RdpEventPayload } from 'libhmrdp.so';
import { RdpConnectOptions, RdpClipKind, RdpEvent, SR_BACKEND_FSR } from '../model/RdpModels';
import { PendingConnection, SessionRequests } from '../model/Connections';
import { BackgroundEvent, BackgroundEvents } from '../services/BackgroundProtocol';
import { ClipboardController, ClipboardControllerHost } from '../services/ClipboardController';
import { RdpClipboardBridge } from '../services/RdpClipboardBridge';
import { AudioPlayer } from '../services/AudioPlayer';
import { BackgroundWorker } from '../services/BackgroundWorker';
import { RdpNative } from '../services/RdpNative';
import { RemoteCursor, RemoteCursorUpdate } from '../services/RemoteCursor';
import { RdpInputSink, RdpSessionBackend } from '../services/RdpSessionBackend';
import { SessionBackend, SessionBackendRegistry } from '../services/SessionBackend';
import { SessionInput } from '../services/SessionInput';
import { SessionSignal } from '../model/SessionSignal';
import { RdpCounters } from '../services/RdpTelemetry';
import { DeviceCapabilities } from '../services/DeviceCapabilities';
import { ConnectionState, SessionManager } from '../services/SessionManager';
import { ForegroundReconnect, ForegroundReconnectHost } from '../services/ForegroundReconnect';
import { AppSettings, DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT, PINCH_MODE_ZOOM,
  SettingsStore, srRatioLabel } from '../services/SettingsStore';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { SessionShellController, SessionSurfaceController } from '../services/SessionShellController';
import { SessionShellHooks, SessionShellHost } from '../services/SessionViewDelegate';
import { showConfirm, showToast } from '../utils/UiUtil';
import { mapPicturePoint } from '../utils/PictureFit';
import { DibImage } from '../utils/ClipboardCodec';
import { FileDescriptorInput, FileTransferCodec } from '../utils/FileTransferCodec';
import { SessionPicture } from '../components/SessionPicture';
import { StatusCard } from '../components/StatusCard';
import { METRIC_BAD, METRIC_GOOD, METRIC_WARN, OutlineChip, TOOLBAR_ACCENT_CHIP_BG,
  TOOLBAR_BG, TOOLBAR_FG, TOOLBAR_FG_SECONDARY,
  TOOLBAR_PROGRESS, TOOLBAR_ROW_HEIGHT, TOOLBAR_TELEMETRY_HEIGHT,
  TOOLBAR_ZONE_BAR, TOOLBAR_ZONE_NONE, TOOLBAR_ZONE_TRIGGER, SessionTitle, ToolbarChip,
  ToolbarRevealHint, TelemetryField, TelemetryRow, metricColor,
  ClipboardButtons, ClipboardTransferStatus } from '../components/SessionChrome';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSession';

// axisPinch is a distance ratio relative to the gesture start; clamp it so a bad sample cannot explode or collapse the separation.
const MIN_PINCH_RATIO: number = 0.05;
const MAX_PINCH_RATIO: number = 20;
const MIN_VIEW_SCALE: number = 1;
const MAX_VIEW_SCALE: number = 8;
const VIEW_SNAP_SCALE: number = 1.08;
const VIEW_SNAP_MS: number = 180;
const METRICS_INTERVAL_MS: number = 1000;
const PAN_WHEEL_STEP: number = 64;

// Toolbar throughput below this is idle/keep-alive noise, so it is not shown.
const MIN_BANDWIDTH_BYTES: number = 1024;

// Fixed widths so a changing number never reflows the items after it.
const METRIC_WIDTH_NETWORK: number = 66;
const METRIC_WIDTH_LOCAL: number = 74;
const METRIC_WIDTH_FPS: number = 48;
const METRIC_WIDTH_AUDIO: number = 86;
const METRIC_WIDTH_CPU: number = 60;
const METRIC_WIDTH_GPU: number = 76;
const METRIC_GAP_TIGHT: number = 6;
const METRIC_GAP_GROUP: number = 8;

// Latency colour thresholds: network RTT warns late (dominated by distance); local per-frame work over WARN misses 60 fps, over BAD even 30 fps.
const NETWORK_WARN_MS: number = 100;
const NETWORK_BAD_MS: number = 200;
const LOCAL_WARN_MS: number = 16;
const LOCAL_BAD_MS: number = 33;

@Entry({ useSharedStorage: true })
@Component
struct SessionPage {
  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接...';
  @State showOverlay: boolean = true;
  @State showToolbar: boolean = true;
  @State revealHintFill: number = 0;
  @State isFullscreen: boolean = false;
  @LocalStorageLink('sessionKey') sessionKey: string = '';

  private native: RdpNative = new RdpNative();
  private options: RdpConnectOptions = new RdpConnectOptions();
  private connectionId: string = '';
  // No-op without the material capability, where the flat background shows.
  private toolbarMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.FLOATING));
  // A system without the material capability ignores this field and keeps the default tooltip.
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
  private controller: SessionSurfaceController = new SessionSurfaceController();
  private shell: SessionShellController | undefined = undefined;
  private componentWidth: number = 0;
  private componentHeight: number = 0;
  @State desktopWidth: number = 1920;
  @State desktopHeight: number = 1080;
  @State metricsRtt: number = -1;
  @State metricsRx: number = 0;
  @State metricsTx: number = 0;
  @State metricsFps: number = 0;
  @State metricsZgxUs: number = 0;
  @State metricsDecodeUs: number = 0;
  @State metricsComposeUs: number = 0;
  @State metricsPresentUs: number = 0;
  @State metricsPresentWaitUs: number = 0;
  @State metricsBytesPerFrame: number = 0;
  @State metricsDutyPermille: number = 0;
  @State metricsCmdsPerFrame: number = 0;
  // The client's own GPU work; no public API exposes the GPU's total load to an app.
  @State metricsGpuCopyUs: number = 0;
  @State metricsGpuSrUs: number = 0;
  @State metricsGpuBlitUs: number = 0;
  @State metricsCpuPercent: number = -1;
  @State developerMode: boolean = false;
  @State metricsAudioRateHz: number = -1;
  @State metricsAudioLossBp: number = -1;
  private surfaceId: string = '';
  private surfaceWidth: number = 0;
  private surfaceHeight: number = 0;
  // While false, surface sizes the system reports are held until it is back, and the server's output is suppressed.
  private windowVisible: boolean = true;
  private pendingSurfaceResize: boolean = false;
  private metricsTimer: number = -1;
  private revealBand: number = 6;
  private revealWidthPercent: number = DEFAULT_TOOLBAR_TRIGGER_WIDTH_PERCENT;
  private toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  private hoverDelay: number = 350;
  private hideDelay: number = 250;
  private sink: RdpInputSink | undefined = undefined;
  private input: SessionInput | undefined = undefined;
  // Direct-zoom mode keeps the client-side view transform in this page.
  private pinchDirect: boolean = false;
  private directZoomActive: boolean = false;
  @State private viewMatrix: Matrix4Transit = matrix4.identity();
  private viewScale: number = 1;
  private viewTransX: number = 0;
  private viewTransY: number = 0;
  private viewPinchAnchorX: number = 0;
  private viewPinchAnchorY: number = 0;
  private viewPinchPanX: number = 0;
  private viewPinchPanY: number = 0;
  private viewPinchBaseScale: number = 1;
  private filesDir: string = '';
  private rdpClipboard: RdpClipboardBridge | undefined = undefined;
  private clipboard: ClipboardController | undefined = undefined;
  @State private transferActive: boolean = false;
  @State private transferWaiting: boolean = false;
  @State private transferPercent: number = 0;
  @State private transferText: string = '';
  @State private transferSpeed: string = '';
  @State private transferFileIndex: number = 0;
  @State private transferFileCount: number = 0;
  private backgroundSink: (event: BackgroundEvent) => void =
    (event: BackgroundEvent): void => {
      this.rdpClipboard?.onBackgroundEvent(event);
    };
  private audioHandle: number = -1;
  private lastRemoteLocks: number = 0;
  private lastRemoteLocksValid: boolean = false;
  private lastReassertMs: number = 0;
  // Only one data request may be in flight; a newer list is pulled once the current one settles (remote responses carry no format id).
  private remoteFormatInFlight: boolean = false;
  private remoteFormatPending: boolean = false;
  private remotePendingId: number = 0;
  private remotePendingKind: number = RdpClipKind.NONE;
  private cursor: RemoteCursor = new RemoteCursor();
  private foreground: ForegroundReconnect | undefined = undefined;

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
    this.resolveRequest();
    const settings: AppSettings = SettingsStore.load();
    this.developerMode = settings.developerMode;
    // Developer mode adds the telemetry line, so the toolbar is taller.
    this.toolbarHeight = this.developerMode
      ? TOOLBAR_ROW_HEIGHT + TOOLBAR_TELEMETRY_HEIGHT
      : TOOLBAR_ROW_HEIGHT;
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.revealWidthPercent = settings.toolbarTriggerWidthPercent;
    this.pinchDirect = settings.pinchMode === PINCH_MODE_ZOOM;
    RdpNative.applyTouchHighRate(settings.touchHighRate);
    // Never ask for Vulkan presentation on a device that cannot do it (covers a config imported from another device).
    RdpNative.applyHardwareAccel(settings.hardwareAccel
      && DeviceCapabilities.hardwareAccel().supported);
    let filesDir: string = '';
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx !== undefined) {
        filesDir = ctx.filesDir;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'filesDir unavailable: %{public}s', JSON.stringify(error));
    }
    RdpNative.applyGfxCapture(settings.gfxCapture, filesDir);
    hilog.info(DOMAIN, TAG, 'gfx capture=%{public}d dir=%{public}s', settings.gfxCapture ? 1 : 0,
      filesDir);
    this.filesDir = filesDir;
    this.rdpClipboard = new RdpClipboardBridge(this.native, filesDir);
    this.clipboard = new ClipboardController(this.rdpClipboard, this.makeClipboardHost());
    const foregroundHost: ForegroundReconnectHost = {
      context: (): common.Context | undefined => this.getUIContext().getHostContext(),
      state: (): ConnectionState => this.state,
      reconnect: (): void => {
        this.shell?.retry();
      }
    };
    this.foreground = new ForegroundReconnect(foregroundHost, settings.reconnectOnForeground);
    RdpNative.applyRdpCursor(settings.useRdpCursor);
    // With 超分辨率 the server renders the session (pre-upscale) size, not the configured output size; the Resize event corrects a mismatch.
    this.desktopWidth = SettingsStore.srSessionSize(this.options.width,
      this.options.srEnabled ? this.options.srRatioPercent : 0);
    this.desktopHeight = SettingsStore.srSessionSize(this.options.height,
      this.options.srEnabled ? this.options.srRatioPercent : 0);
    const host: SessionShellHost = {
      ui: (): UIContext => this.getUIContext(),
      setState: (state: ConnectionState): void => {
        this.state = state;
      },
      setStatusText: (text: string): void => {
        this.statusText = text;
      },
      setOverlayVisible: (visible: boolean): void => {
        this.showOverlay = visible;
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
      overlayDelayMs: (): number => 0,
      isPinned: (): boolean => this.clipboard?.isTransferBlocking() ?? false,
      makeInput: (backend: SessionBackend | undefined, load: AppSettings): SessionInput | undefined =>
        this.makeInput(load),
      configureCursor: (cursor: RemoteCursor, load: AppSettings): void => {
        cursor.setEnabled(load.useRdpCursor);
      },
      onStateChanged: (state: ConnectionState, message: string): void => {
        this.onStateChanged(state, message);
      },
      onSignal: (signal: SessionSignal): void => {
        this.onSessionSignal(signal);
      },
      onSurfaceCreated: (surfaceId: string, width: number, height: number): void => {
        this.surfaceId = surfaceId;
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        this.native.bindSurface(surfaceId, width, height);
      },
      onSurfaceChanged: (width: number, height: number): void => {
        // Hidden: hold the size and re-measure once the window is visible.
        if (!this.windowVisible) {
          this.pendingSurfaceResize = true;
          return;
        }
        this.applySurfaceSize(width, height);
      },
      onSurfaceDestroyed: (): void => {
        this.native.unbindSurface();
      },
      onWindowVisible: (visible: boolean): void => {
        this.windowVisible = visible;
        if (visible) {
          this.flushPendingSurfaceSize();
        }
        this.native.setSuppressed(!visible);
      },
      consumesAxis: (event: AxisEvent): boolean => false,
      onReleaseInputs: (): void => {
        this.endDirectZoom();
      },
      onBeforeRetry: (): void => {
        if (this.surfaceId.length > 0) {
          this.native.bindSurface(this.surfaceId, this.surfaceWidth, this.surfaceHeight);
        }
      },
      onDisconnectFallback: (): void => {
        this.native.stopSession();
        this.native.clearListener();
      },
      onRelease: (): void => {
        this.releaseProtocol();
      }
    };
    const shell: SessionShellController =
      new SessionShellController(this.sessionKey, host, hooks, this.controller, this.cursor);
    this.shell = shell;
    shell.attach(settings.shortcutPassthrough);
  }

  aboutToDisappear(): void {
    this.shell?.release();
  }

  onPageShow(): void {
    this.foreground?.onPageShow();
  }

  onPageHide(): void {
    this.foreground?.onPageHide();
  }

  private resolveRequest(): void {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    if (request !== undefined) {
      this.options = request.options;
      this.connectionId = request.connectionId;
    }
    const backend: SessionBackend | undefined = SessionBackendRegistry.find(this.sessionKey);
    if (backend instanceof RdpSessionBackend) {
      this.native = backend.native;
    }
    BackgroundWorker.registerSession(this.backgroundSink);
  }

  private onStateChanged(state: ConnectionState, message: string): void {
    if (state === ConnectionState.Connected) {
      this.statusText = '已连接';
      this.native.setSuppressed(!this.windowVisible);
      this.audioHandle = this.native.handleId();
      AudioPlayer.start(this.audioHandle);
      this.rdpClipboard?.reset();
      this.lastRemoteLocksValid = false;
      this.lastReassertMs = 0;
      this.startMetricsTimer();
      return;
    }
    if (state === ConnectionState.Connecting) {
      this.statusText = '正在连接...';
      return;
    }
    if (state === ConnectionState.Disconnecting) {
      this.statusText = '正在断开...';
      this.stopAudio();
      this.rdpClipboard?.reset();
      this.stopMetricsTimer();
      return;
    }
    this.statusText = message.length > 0 ? message
      : (state === ConnectionState.Failed ? '连接失败' : '连接已断开');
    this.cursor.reset();
    this.stopAudio();
    this.rdpClipboard?.reset();
    this.stopMetricsTimer();
  }

  private makeInput(settings: AppSettings): SessionInput {
    const sink: RdpInputSink = new RdpInputSink(this.native);
    this.sink = sink;
    const input: SessionInput = new SessionInput(sink);
    this.input = input;
    input.setWinKeySubstitute(settings.winKeySubstitute);
    input.setPointerSpace(this.desktopWidth, this.desktopHeight);
    input.setPointMapper((x: number, y: number): number[] => this.mapPointer(x, y));
    input.setTouchEnabled(true);
    input.setTouchpadScrollSpeed(settings.touchpadScrollSpeed);
    input.setTouchpadMiddleDoubleTap(settings.touchpadMiddleDoubleTap);
    input.setPinchMode(settings.pinchMode);
    input.setPinchZoomSpeed(settings.pinchZoomSpeed);
    input.setPinchTouchDistance(settings.pinchTouchDistance);
    input.setPinchTouchAngle(settings.pinchTouchAngle);
    input.setPinchCursorHandler((active: boolean, zoomIn: boolean): void => {
      this.cursor.setPinchOverride(active, zoomIn);
    });
    return input;
  }

  private releaseProtocol(): void {
    BackgroundWorker.unregisterSession(this.backgroundSink);
    this.stopAudio();
    this.stopMetricsTimer();
    this.foreground?.release();
    this.foreground = undefined;
    this.endDirectZoom();
    this.options.password = '';
    this.options.gatewayPassword = '';
  }

  private stopAudio(): void {
    if (this.audioHandle > 0) {
      AudioPlayer.stop(this.audioHandle);
      this.audioHandle = -1;
    }
  }

  private startMetricsTimer(): void {
    if (this.metricsTimer !== -1) {
      return;
    }
    this.metricsTimer = setInterval(() => {
      this.refreshCounters();
    }, METRICS_INTERVAL_MS);
  }

  private stopMetricsTimer(): void {
    if (this.metricsTimer === -1) {
      return;
    }
    clearInterval(this.metricsTimer);
    this.metricsTimer = -1;
  }

  private refreshCounters(): void {
    const counters: RdpCounters | undefined = this.native.readCounters();
    if (counters !== undefined) {
      this.applyCounters(counters);
    }
  }

  private onSessionSignal(signal: SessionSignal): void {
    const payload: RdpEventPayload = signal.payload as RdpEventPayload;
    this.onRdpEvent(signal.eventCode as RdpEvent, payload);
  }

  private onRdpEvent(event: RdpEvent, payload: RdpEventPayload): void {
    if (this.rdpClipboard?.handleEvent(event, payload) === true) {
      return;
    }
    if (event === RdpEvent.FirstFrame) {
      this.showOverlay = false;
      this.showToolbar = false;
    } else if (event === RdpEvent.Resize) {
      const width: number = payload.width ?? 0;
      const height: number = payload.height ?? 0;
      if (width > 0 && height > 0) {
        this.desktopWidth = width;
        this.desktopHeight = height;
        this.input?.setPointerSpace(width, height);
        this.resetViewZoom();
      }
    } else if (event === RdpEvent.CursorShape) {
      this.onCursorShape(payload);
    } else if (event === RdpEvent.CursorDefault) {
      this.cursor.applyDefault();
    } else if (event === RdpEvent.CursorHidden) {
      const update: RemoteCursorUpdate = new RemoteCursorUpdate();
      update.hasShape = false;
      update.visible = false;
      this.cursor.applyUpdate(update);
    } else if (event === RdpEvent.KeyIntercept) {
      this.handleInterceptedKey(payload);
    } else if (event === RdpEvent.AudioFormat) {
      AudioPlayer.onFormat(this.native.handleId(), payload.audioRate ?? 0,
        payload.audioChannels ?? 0);
    } else if (event === RdpEvent.RemoteLockState) {
      this.onRemoteLockState(payload.lockFlags ?? 0);
    }
  }

  private applyCounters(counters: RdpCounters): void {
    this.metricsRtt = counters.rttMs;
    this.metricsRx = counters.rxBytesPerSec;
    this.metricsTx = counters.txBytesPerSec;
    this.metricsFps = counters.fps;
    this.metricsAudioRateHz = counters.audioRateHz;
    this.metricsAudioLossBp = counters.audioLossBp;
    this.metricsZgxUs = counters.zgxParseUs;
    this.metricsDecodeUs = counters.decodeUs;
    this.metricsComposeUs = counters.composeUs;
    this.metricsPresentUs = counters.presentUs;
    this.metricsBytesPerFrame = counters.bytesPerFrame;
    this.metricsDutyPermille = counters.dutyPermille;
    this.metricsCmdsPerFrame = counters.commandsPerFrame;
    this.metricsPresentWaitUs = counters.presentWaitUs;
    this.metricsGpuCopyUs = counters.gpuCopyUs;
    this.metricsGpuSrUs = counters.gpuSrUs;
    this.metricsGpuBlitUs = counters.gpuBlitUs;
    this.sampleCpuUsage();
  }

  // hidebug is comparatively expensive and meant for debugging, so it is called once per metrics window.
  private sampleCpuUsage(): void {
    try {
      const usage: number = hidebug.getCpuUsage();
      this.metricsCpuPercent = usage >= 0 ? Math.round(usage * 100) : -1;
    } catch (error) {
      this.metricsCpuPercent = -1;
    }
  }

  private rttLabel(): string {
    return this.metricsRtt < 0 ? '网络 --' : '网络 ' + this.metricsRtt + 'ms';
  }

  private localUs(): number {
    return this.metricsZgxUs + this.metricsDecodeUs + this.metricsComposeUs +
      this.metricsPresentUs;
  }

  private localMs(): number {
    const us: number = this.localUs();
    return us > 0 ? us / 1000 : -1;
  }

  private localLabel(): string {
    const ms: number = this.localMs();
    return ms < 0 ? '本机 --' : '本机 ' + Math.round(ms) + 'ms';
  }

  private usToMs(us: number): number {
    return Math.round(us / 1000);
  }

  private usToMsTenth(us: number): string {
    return (us / 1000).toFixed(1);
  }

  private cpuLabel(): string {
    return this.metricsCpuPercent < 0 ? 'CPU --' : 'CPU ' + this.metricsCpuPercent + '%';
  }

  private gpuLabel(): string {
    const total: number = this.metricsGpuCopyUs + this.metricsGpuSrUs + this.metricsGpuBlitUs;
    return total > 0 ? 'GPU ' + this.usToMsTenth(total) + 'ms' : 'GPU --';
  }

  private gpuTips(): string {
    const total: number = this.metricsGpuCopyUs + this.metricsGpuSrUs + this.metricsGpuBlitUs;
    if (total <= 0) {
      return '本机 GPU 上屏工时（脏区上传 / 超分辨率 / letterbox）\n'
        + '当前上屏后端不提供 GPU 时间戳，只在使用 Vulkan 硬件加速时可见';
    }
    return '本机 GPU 上屏工时/帧：上传 ' + this.usToMsTenth(this.metricsGpuCopyUs) + 'ms'
      + ' · 超分辨率 ' + this.usToMsTenth(this.metricsGpuSrUs) + 'ms'
      + ' · letterbox ' + this.usToMsTenth(this.metricsGpuBlitUs) + 'ms'
      + '\n这是本机自己的 GPU 工作量，不是 GPU 整机占用率（平台未开放该接口）';
  }

  private localTips(): string {
    return '解压+解析 ' + this.usToMs(this.metricsZgxUs) + 'ms' +
      ' / 解码 ' + this.usToMs(this.metricsDecodeUs) + 'ms' +
      ' / 合成 ' + this.usToMs(this.metricsComposeUs) + 'ms' +
      ' / 上屏 ' + this.usToMs(this.metricsPresentUs) + 'ms' +
      (this.metricsPresentWaitUs > 0
        ? '（等显示 ' + this.usToMs(this.metricsPresentWaitUs) + 'ms，不计入本机）'
        : '') +
      '\n载荷 ' + Math.round(this.metricsBytesPerFrame / 1024) + 'KB/帧' +
      '（' + this.metricsCmdsPerFrame + ' 条命令）' +
      '，本机工时占帧周期 ' + Math.round(this.metricsDutyPermille / 10) + '%';
  }

  private fpsLabel(): string {
    return 'FPS ' + this.metricsFps;
  }

  private audioLabel(): string {
    return '音频 ' + Math.round(this.metricsAudioRateHz / 1000) + 'k ' +
      (this.metricsAudioLossBp / 100).toFixed(2) + '%';
  }

  private audioColor(): string {
    if (this.metricsAudioLossBp < 0) {
      return TOOLBAR_FG_SECONDARY;
    }
    if (this.metricsAudioLossBp === 0) {
      return METRIC_GOOD;
    }
    if (this.metricsAudioLossBp <= 50) {
      return METRIC_WARN;
    }
    return METRIC_BAD;
  }

  private formatRate(bytesPerSec: number): string {
    if (bytesPerSec >= 1024 * 1024) {
      return (bytesPerSec / (1024 * 1024)).toFixed(1) + 'MB/s';
    }
    if (bytesPerSec >= 1024) {
      return Math.round(bytesPerSec / 1024) + 'KB/s';
    }
    return bytesPerSec + 'B/s';
  }

  private hasBandwidth(): boolean {
    return this.metricsRx >= MIN_BANDWIDTH_BYTES || this.metricsTx >= MIN_BANDWIDTH_BYTES;
  }

  private bandwidthLabel(): string {
    const parts: string[] = [];
    if (this.metricsRx >= MIN_BANDWIDTH_BYTES) {
      parts.push('↓' + this.formatRate(this.metricsRx));
    }
    if (this.metricsTx >= MIN_BANDWIDTH_BYTES) {
      parts.push('↑' + this.formatRate(this.metricsTx));
    }
    return parts.join('  ');
  }

  private onCursorShape(payload: RdpEventPayload): void {
    const update: RemoteCursorUpdate = new RemoteCursorUpdate();
    update.hasShape = true;
    // RDP has no standalone "show" event; a shape or the default arriving *is* the show.
    update.visible = true;
    update.shapeKey = payload.shapeHash ?? 0;
    update.magnification = (payload.magnificationMilli ?? 1000) / 1000;
    update.width = payload.width ?? 0;
    update.height = payload.height ?? 0;
    update.hotX = payload.hotX ?? 0;
    update.hotY = payload.hotY ?? 0;
    update.imageHandle = payload.imageHandle ?? 0;
    this.cursor.applyUpdate(update);
  }

  private onRemoteLockState(lockFlags: number): void {
    if (this.lastRemoteLocksValid && this.lastRemoteLocks === lockFlags) {
      return;
    }
    this.lastRemoteLocks = lockFlags;
    this.lastRemoteLocksValid = true;
    const sink: RdpInputSink | undefined = this.sink;
    if (sink === undefined || !sink.hasLockState() || lockFlags === sink.lastLockFlags()) {
      return;
    }
    const now: number = Date.now();
    if (now - this.lastReassertMs < 1000) {
      return;
    }
    this.lastReassertMs = now;
    this.native.reassertLockState(sink.lastLockFlags());
  }

  private toast(message: string): void {
    showToast(this.getUIContext(), message, 1500);
  }

  private makeClipboardHost(): ClipboardControllerHost {
    const host: ClipboardControllerHost = {
      toast: (message: string): void => {
        this.toast(message);
      },
      confirmCancelUpload: (onConfirm: () => void): void => {
        showConfirm(this.getUIContext(), {
          title: '取消文件传输',
          message: '文件仍在发送。取消后远端粘贴会提示错误，已传部分也会作废。是否继续取消？',
          cancelLabel: '继续传输',
          confirmLabel: '取消传输',
          danger: true,
          onConfirm: (): void => {
            onConfirm();
          }
        });
      },
      setTransferProgress: (direction: number, percent: number, text: string,
        speedBytesPerSecond: number, fileIndex: number, fileCount: number): void => {
        this.transferActive = true;
        this.transferWaiting = false;
        this.transferPercent = percent;
        this.transferText = text;
        this.transferSpeed = speedBytesPerSecond > 0 ? this.formatRate(speedBytesPerSecond) : '';
        this.transferFileIndex = fileIndex;
        this.transferFileCount = fileCount;
        this.refreshToolbarAutoHide();
      },
      setTransferWaiting: (direction: number, fileIndex: number, fileCount: number): void => {
        this.transferActive = false;
        this.transferWaiting = true;
        this.transferFileIndex = fileIndex;
        this.transferFileCount = fileCount;
        this.refreshToolbarAutoHide();
      },
      clearTransfer: (): void => {
        this.transferActive = false;
        this.transferWaiting = false;
        this.transferPercent = 0;
        this.transferText = '';
        this.transferSpeed = '';
        this.transferFileIndex = 0;
        this.transferFileCount = 0;
        this.refreshToolbarAutoHide();
      },
      onRemoteChanged: (): void => {
      }
    };
    return host;
  }

  private setToolbarPointerZone(zone: number): void {
    this.shell?.setPointerZone(zone);
  }

  private refreshToolbarAutoHide(): void {
    this.shell?.refreshAutoHide();
  }

  private onRevealZone(inBand: boolean): void {
    this.setToolbarPointerZone(inBand ? TOOLBAR_ZONE_TRIGGER : TOOLBAR_ZONE_NONE);
  }

  private mapPointer(x: number, y: number): number[] {
    return mapPicturePoint(x, y, this.componentWidth, this.componentHeight,
      this.desktopWidth, this.desktopHeight);
  }

  private handleMouse(event: MouseEvent): void {
    this.shell?.routeMouse(event);
  }

  private handleTouch(event: TouchEvent): void {
    this.shell?.routeTouch(event);
  }

  private handleAxis(event: AxisEvent): void {
    // Direct zoom is a client-view transform that stays in this page. It only owns the pinch
    // (view zoom) and, while zoomed, the pan; every other axis event falls through to the
    // shared input pipeline (touchpad scroll / wheel).
    if (this.pinchDirect) {
      const isPinch: boolean = SessionInput.isPinchEvent(event);
      if (isPinch || this.directZoomActive) {
        if (event.action === AxisAction.END) {
          this.endDirectZoom();
          return;
        }
        if (isPinch) {
          const scale: number = SessionInput.pinchScaleValue(event);
          if (!this.directZoomActive) {
            this.directZoomActive = true;
            this.beginViewPinch(event, scale);
          } else if (scale > 0) {
            this.updateViewPinch(scale);
          }
        }
        return;
      }
      const scrolling: boolean = event.action === AxisAction.BEGIN
        || event.action === AxisAction.UPDATE;
      if (this.viewScale > MIN_VIEW_SCALE && scrolling) {
        const vertical: number = event.axisVertical ?? 0;
        const horizontal: number = event.axisHorizontal ?? 0;
        if (this.input?.isTouchpad(event.deviceId, event.sourceTool) ?? false) {
          this.panView(-horizontal, -vertical);
        } else if (vertical !== 0) {
          this.panView(0, -vertical * PAN_WHEEL_STEP);
        }
        return;
      }
    }
    this.shell?.routeAxis(event);
  }

  private endDirectZoom(): void {
    this.directZoomActive = false;
    this.snapViewZoom();
  }

  private beginViewPinch(event: AxisEvent, scale: number): void {
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const cx: number = compWidth / 2;
    const cy: number = compHeight / 2;
    this.viewPinchAnchorX = Math.max(0, Math.min(compWidth, event.x));
    this.viewPinchAnchorY = Math.max(0, Math.min(compHeight, event.y));
    this.viewPinchBaseScale = this.viewScale;
    this.viewPinchPanX = cx + this.viewScale * (this.viewPinchAnchorX - cx) + this.viewTransX
      - this.viewPinchAnchorX;
    this.viewPinchPanY = cy + this.viewScale * (this.viewPinchAnchorY - cy) + this.viewTransY
      - this.viewPinchAnchorY;
    this.updateViewPinch(scale > 0 ? scale : 1);
  }

  private updateViewPinch(scale: number): void {
    const ratio: number = Math.max(MIN_PINCH_RATIO, Math.min(MAX_PINCH_RATIO,
      scale > 0 ? scale : 1));
    const clamped: number = Math.max(MIN_VIEW_SCALE, Math.min(MAX_VIEW_SCALE,
      this.viewPinchBaseScale * ratio));
    this.applyViewZoom(clamped);
  }

  private applyViewZoom(scale: number): void {
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const cx: number = compWidth / 2;
    const cy: number = compHeight / 2;
    const unclampedX: number = (1 - scale) * this.viewPinchAnchorX + this.viewPinchPanX;
    const unclampedY: number = (1 - scale) * this.viewPinchAnchorY + this.viewPinchPanY;
    const left: number = this.clampViewEdge(unclampedX, compWidth, compWidth * scale);
    const top: number = this.clampViewEdge(unclampedY, compHeight, compHeight * scale);
    const panX: number = this.viewPinchPanX + (left - unclampedX);
    const panY: number = this.viewPinchPanY + (top - unclampedY);
    this.viewScale = scale;
    this.viewTransX = (1 - scale) * (this.viewPinchAnchorX - cx) + panX;
    this.viewTransY = (1 - scale) * (this.viewPinchAnchorY - cy) + panY;
    this.commitViewMatrix();
  }

  private panView(dx: number, dy: number): void {
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const cx: number = compWidth / 2;
    const cy: number = compHeight / 2;
    const scale: number = this.viewScale;
    const left: number = this.clampViewEdge(
      (1 - scale) * cx + this.viewTransX + dx, compWidth, compWidth * scale);
    const top: number = this.clampViewEdge(
      (1 - scale) * cy + this.viewTransY + dy, compHeight, compHeight * scale);
    this.viewTransX = left - (1 - scale) * cx;
    this.viewTransY = top - (1 - scale) * cy;
    this.commitViewMatrix();
  }

  private clampViewEdge(position: number, viewport: number, content: number): number {
    if (content >= viewport) {
      return Math.max(viewport - content, Math.min(0, position));
    }
    return (viewport - content) / 2;
  }

  private commitViewMatrix(): void {
    const pxPerVp: number = this.getUIContext().vp2px(1);
    this.viewMatrix = matrix4.init([
      this.viewScale, 0, 0, 0,
      0, this.viewScale, 0, 0,
      0, 0, 1, 0,
      this.viewTransX * pxPerVp, this.viewTransY * pxPerVp, 0, 1
    ]);
  }

  private resetViewZoom(): void {
    if (this.viewScale === MIN_VIEW_SCALE && this.viewTransX === 0 && this.viewTransY === 0) {
      return;
    }
    this.viewScale = MIN_VIEW_SCALE;
    this.viewTransX = 0;
    this.viewTransY = 0;
    this.viewMatrix = matrix4.identity();
  }

  private snapViewZoom(): void {
    if (!this.pinchDirect || this.viewScale <= MIN_VIEW_SCALE
      || this.viewScale >= VIEW_SNAP_SCALE) {
      return;
    }
    this.getUIContext().animateTo({ duration: VIEW_SNAP_MS, curve: Curve.EaseOut }, () => {
      this.viewScale = MIN_VIEW_SCALE;
      this.viewTransX = 0;
      this.viewTransY = 0;
      this.viewMatrix = matrix4.identity();
    });
  }

  private handleInterceptedKey(payload: RdpEventPayload): void {
    const keyCode: number = payload.keyCode ?? -1;
    if (keyCode >= 0) {
      this.input?.handleInterceptedKey(keyCode, payload.down ?? false);
    }
  }

  private handleKey(event: KeyEvent): void {
    this.shell?.routeKey(event);
  }

  private releaseAllInputs(): void {
    this.input?.releaseAll();
    this.endDirectZoom();
  }

  private applySurfaceSize(width: number, height: number): void {
    if (width <= 0 || height <= 0 || this.surfaceId.length === 0) {
      return;
    }
    if (width === this.surfaceWidth && height === this.surfaceHeight) {
      return;
    }
    this.surfaceWidth = width;
    this.surfaceHeight = height;
    this.native.resizeSurface(width, height);
  }

  private flushPendingSurfaceSize(): void {
    if (!this.pendingSurfaceResize) {
      return;
    }
    this.pendingSurfaceResize = false;
    const rect: SurfaceRect = this.controller.getXComponentSurfaceRect();
    this.applySurfaceSize(rect.surfaceWidth, rect.surfaceHeight);
  }

  private srIndicator(): string {
    if (!this.options.srEnabled || this.options.srRatioPercent <= 100) {
      return '';
    }
    const backend: string = this.options.srBackend === SR_BACKEND_FSR ? 'FSR' : 'XEngine';
    return ' (' + backend + ' ' + srRatioLabel(this.options.srRatioPercent) + ')';
  }

  @Builder
  sessionToolbarMainRow() {
    Row() {
      SessionTitle({
        name: this.options.displayName(),
        detail: this.desktopWidth + ' × ' + this.desktopHeight + this.srIndicator()
      })
      Blank()
      if (this.transferWaiting || this.transferActive) {
        ClipboardTransferStatus({
          waiting: this.transferWaiting,
          percent: this.transferPercent,
          text: this.transferText,
          speed: this.transferSpeed,
          fileIndex: this.transferFileIndex,
          fileCount: this.transferFileCount,
          onCancel: (): void => {
            this.clipboard?.cancelTransfer();
          }
        })
      }
      ClipboardButtons({
        onCopy: (): void => {
          this.clipboard?.copyRemoteToLocal();
        },
        pasteCallback: this.pasteButtonCallback
      })
      Row().width(16)
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
        chipBg: $r('app.color.danger'),
        onTap: () => {
          this.shell?.disconnect();
        }
      })
    }
    .width('100%')
    .height(TOOLBAR_ROW_HEIGHT)
    .padding({ left: 16, right: 16 })
  }

  @Builder
  sessionToolbar() {
    Column() {
      this.sessionToolbarMainRow()
      if (this.state === ConnectionState.Connected && this.developerMode) {
        this.toolbarTelemetry()
      }
    }
    .width('100%')
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : TOOLBAR_BG)
    .attributeModifier(this.toolbarMaterial)
    // In fullscreen the strip covers the surface, so only its own hover reports the pointer.
    .onHover((isHover: boolean) => {
      this.setToolbarPointerZone(isHover ? TOOLBAR_ZONE_BAR : TOOLBAR_ZONE_NONE);
    })
    .transition(TransitionEffect.translate({ y: -this.toolbarHeight })
      .animation({ duration: 180, curve: Curve.EaseOut }))
  }

  // Developer-only telemetry line. The two fields that can disappear follow in reverse order of how often they show, so a changing field never shifts a stable one.
  @Builder
  toolbarTelemetry() {
    TelemetryRow({ fields: this.telemetryFields() })
  }

  private telemetryFields(): TelemetryField[] {
    const fields: TelemetryField[] = [];
    fields.push(new TelemetryField(this.rttLabel(), METRIC_WIDTH_NETWORK,
      metricColor(this.metricsRtt, NETWORK_WARN_MS, NETWORK_BAD_MS), '', 0));
    fields.push(new TelemetryField(this.localLabel(), METRIC_WIDTH_LOCAL,
      metricColor(this.localMs(), LOCAL_WARN_MS, LOCAL_BAD_MS), this.localTips(),
      METRIC_GAP_TIGHT));
    fields.push(new TelemetryField(this.fpsLabel(), METRIC_WIDTH_FPS,
      TOOLBAR_FG_SECONDARY, '', METRIC_GAP_GROUP));
    fields.push(new TelemetryField(this.cpuLabel(), METRIC_WIDTH_CPU,
      TOOLBAR_FG_SECONDARY, '', METRIC_GAP_GROUP));
    fields.push(new TelemetryField(this.gpuLabel(), METRIC_WIDTH_GPU,
      TOOLBAR_FG_SECONDARY, this.gpuTips(), METRIC_GAP_GROUP));
    if (this.hasBandwidth()) {
      fields.push(new TelemetryField(this.bandwidthLabel(), 0,
        TOOLBAR_FG_SECONDARY, '', METRIC_GAP_GROUP));
    }
    if (this.metricsAudioRateHz > 0) {
      fields.push(new TelemetryField(this.audioLabel(), METRIC_WIDTH_AUDIO,
        this.audioColor(), '', METRIC_GAP_GROUP));
    }
    return fields;
  }

  build() {
    Column() {
      if (!this.isFullscreen && !this.showOverlay) {
        this.sessionToolbar()
      }
      Stack({ alignContent: Alignment.TopStart }) {
        // The picture lives in its own layer so direct pinch can scale it locally; the Stack clips it.
        Column() {
          SessionPicture({
            xComponentId: 'rdpSurface',
            pictureController: this.controller,
            contentWidth: this.desktopWidth,
            contentHeight: this.desktopHeight,
            revealBand: (this.isFullscreen && !this.showOverlay && !this.showToolbar)
              ? this.revealBand : 0,
            revealBandWidth: this.revealWidthPercent,
            onPointerMouse: (event: MouseEvent) => {
              this.handleMouse(event);
            },
            onPointerHover: (isHover: boolean) => {
              // The system may drop a custom cursor when the pointer leaves and re-enters, so re-install the last shape.
              if (isHover) {
                this.cursor.reapply();
              }
            },
            onSurfaceBlur: () => {
              this.releaseAllInputs();
            },
            onPointerTouch: (event: TouchEvent) => {
              this.handleTouch(event);
            },
            onPointerAxis: (event: AxisEvent) => {
              this.handleAxis(event);
            },
            onSurfaceKey: (event: KeyEvent) => {
              this.handleKey(event);
            },
            onPictureArea: (width: number, height: number) => {
              if (this.componentWidth !== width || this.componentHeight !== height) {
                this.resetViewZoom();
              }
              this.componentWidth = width;
              this.componentHeight = height;
            },
            onRevealZone: (inBand: boolean) => {
              this.onRevealZone(inBand);
            }
          })
        }
        .width('100%')
        .height('100%')
        .transform(this.viewMatrix)

        if (this.isFullscreen && !this.showOverlay) {
          Stack() {
            ToolbarRevealHint({ fill: this.revealHintFill })
          }
          .width('100%')
          .height('100%')
          .alignContent(Alignment.Top)
          .hitTestBehavior(HitTestMode.None)
        }

        if (this.isFullscreen && this.showToolbar && !this.showOverlay) {
          this.sessionToolbar()
        }

      if (this.showOverlay) {
        Column() {
          if (this.state === ConnectionState.Connecting) {
            StatusCard({
              title: '正在连接',
              subtitle: this.options.targetLabel(),
              primary: '取消',
              loading: true,
              onPrimary: () => {
                this.shell?.disconnect();
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
              icon: $r('app.media.ic_remote_desktop'),
              onPrimary: () => {
                this.shell?.retry();
              },
              onSecondary: () => {
                this.shell?.disconnect();
              }
            })
          } else if (this.state === ConnectionState.Idle) {
            StatusCard({
              title: '连接已断开',
              subtitle: this.statusText,
              primary: '重试',
              secondary: '返回',
              icon: $r('app.media.ic_remote_desktop'),
              onPrimary: () => {
                this.shell?.retry();
              },
              onSecondary: () => {
                this.shell?.disconnect();
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
