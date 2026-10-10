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
import { AppSettings, PINCH_MODE_ZOOM,
  SettingsStore, srRatioLabel } from '../services/SettingsStore';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { SessionShellController, SessionSurfaceController } from '../services/SessionShellController';
import { SessionShellHooks, SessionShellHost } from '../services/SessionViewDelegate';
import { lengthToNumber, showConfirm, showToast } from '../utils/UiUtil';
import { DibImage } from '../utils/ClipboardCodec';
import { FileDescriptorInput, FileTransferCodec } from '../utils/FileTransferCodec';
import { StatusCard } from '../components/StatusCard';
import { METRIC_BAD, METRIC_GOOD, METRIC_WARN, MetricItem, OutlineChip, TOOLBAR_ACCENT_CHIP_BG,
  TOOLBAR_BG, TOOLBAR_FG, TOOLBAR_FG_SECONDARY,
  TOOLBAR_PROGRESS, TOOLBAR_ROW_HEIGHT, TOOLBAR_TELEMETRY_HEIGHT,
  TOOLBAR_ZONE_BAR, TOOLBAR_ZONE_NONE, TOOLBAR_ZONE_TRIGGER, SessionTitle, ToolbarChip,
  ToolbarRevealHint } from '../components/SessionChrome';

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

class ClipPick {
  kind: number = RdpClipKind.NONE;
  id: number = 0;
}

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
  private desktopWidth: number = 1920;
  private desktopHeight: number = 1080;
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
  private remoteClipboardText: string = '';
  private remoteClipboardHtml: string = '';
  private remoteClipboardPixelMap: image.PixelMap | undefined = undefined;
  private remoteFileCount: number = 0;
  private remoteFolderCount: number = 0;
  private pasteboardBusy: boolean = false;
  @State private transferActive: boolean = false;
  @State private transferWaiting: boolean = false;
  @State private transferPercent: number = 0;
  @State private transferText: string = '';
  @State private transferSpeed: string = '';
  @State private transferFileIndex: number = 0;
  @State private transferFileCount: number = 0;
  private transferDirection: number = 0;
  private transferBusy: boolean = false;
  private transferLastDone: number = 0;
  private transferLastAt: number = 0;
  private backgroundSink: (event: BackgroundEvent) => void =
    (event: BackgroundEvent): void => {
      this.onBackgroundEvent(event);
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
        this.submitClipboard();
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
      isPinned: (): boolean => this.transferActive || this.transferWaiting,
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
      this.resetClipboardNegotiation();
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
      this.resetClipboardNegotiation();
      this.stopMetricsTimer();
      return;
    }
    this.statusText = message.length > 0 ? message
      : (state === ConnectionState.Failed ? '连接失败' : '连接已断开');
    this.cursor.reset();
    this.stopAudio();
    this.resetClipboardNegotiation();
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
    } else if (event === RdpEvent.ClipboardText) {
      const text: string = payload.text ?? '';
      hilog.info(DOMAIN, TAG, 'remote clipboard text: %{public}d chars', text.length);
      this.remoteClipboardText = text;
      this.remoteClipboardHtml = '';
      this.remoteClipboardPixelMap = undefined;
      this.remoteFileCount = 0;
      this.remoteFolderCount = 0;
    } else if (event === RdpEvent.ClipboardHtml) {
      this.onRemoteClipboardHtml(payload);
    } else if (event === RdpEvent.ClipboardRtf) {
      this.onRemoteClipboardRtf(payload);
    } else if (event === RdpEvent.ClipboardImage) {
      hilog.info(DOMAIN, TAG, 'remote clipboard image (raw DIB)');
      this.remoteFileCount = 0;
      this.remoteFolderCount = 0;
      this.onClipboardImage(payload);
    } else if (event === RdpEvent.RemoteFileListRaw) {
      this.onRemoteFileListRaw(payload);
    } else if (event === RdpEvent.RemoteFileChunk) {
      this.onRemoteFileChunk(payload);
    } else if (event === RdpEvent.RemoteClipboardChanged) {
      this.onRemoteClipboardChanged();
    } else if (event === RdpEvent.FileTransferProgress) {
      this.onFileTransferProgress(payload);
    } else if (event === RdpEvent.FileTransferDone) {
      this.finishFileTransfer();
      this.toast('已发送文件到远端');
    } else if (event === RdpEvent.FileTransferFailed) {
      this.finishFileTransfer();
      const reason: string = payload.reason ?? '';
      this.toast(reason.length > 0 ? reason : '文件传输失败');
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
    } else if (event === RdpEvent.RemoteFormatList) {
      this.onRemoteFormatList(payload);
    } else if (event === RdpEvent.RemoteFormatResponse) {
      this.onRemoteFormatResponse();
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

  private thresholdColor(ms: number, warnMs: number, badMs: number): string {
    if (ms < 0) {
      return TOOLBAR_FG_SECONDARY;
    }
    if (ms <= warnMs) {
      return METRIC_GOOD;
    }
    if (ms <= badMs) {
      return METRIC_WARN;
    }
    return METRIC_BAD;
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

  private onRemoteClipboardHtml(payload: RdpEventPayload): void {
    const dataHandle: number = payload.dataHandle ?? 0;
    if (dataHandle <= 0) {
      return;
    }
    const cfHtml: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(dataHandle);
    if (cfHtml === undefined) {
      return;
    }
    BackgroundWorker.decodeCfHtml(cfHtml)
      .then((html: string) => {
        hilog.info(DOMAIN, TAG, 'remote clipboard html: %{public}d chars', html.length);
        this.setRemoteHtml(html);
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'decode cf_html failed: %{public}s', JSON.stringify(error));
      });
  }

  private onRemoteClipboardRtf(payload: RdpEventPayload): void {
    const dataHandle: number = payload.dataHandle ?? 0;
    if (dataHandle <= 0) {
      return;
    }
    const rtf: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(dataHandle);
    if (rtf === undefined) {
      return;
    }
    BackgroundWorker.decodeRtf(rtf)
      .then((html: string) => {
        hilog.info(DOMAIN, TAG, 'remote clipboard rtf->html: %{public}d chars', html.length);
        this.setRemoteHtml(html);
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'decode rtf failed: %{public}s', JSON.stringify(error));
      });
  }

  private setRemoteHtml(html: string): void {
    this.remoteClipboardHtml = html;
    this.remoteClipboardText = '';
    this.remoteClipboardPixelMap = undefined;
    this.remoteFileCount = 0;
    this.remoteFolderCount = 0;
  }

  private onRemoteFileListRaw(payload: RdpEventPayload): void {
    const dataHandle: number = payload.dataHandle ?? 0;
    if (dataHandle <= 0) {
      return;
    }
    const fgd: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(dataHandle);
    if (fgd === undefined) {
      return;
    }
    this.remoteClipboardText = '';
    this.remoteClipboardHtml = '';
    this.remoteClipboardPixelMap = undefined;
    BackgroundWorker.postFileList(this.native.handleId(), fgd);
  }

  private onRemoteClipboardChanged(): void {
    this.remoteFileCount = 0;
    this.remoteFolderCount = 0;
    BackgroundWorker.postClipboardChanged(this.native.handleId());
  }

  private onRemoteFormatList(payload: RdpEventPayload): void {
    const pick: ClipPick | undefined =
      SessionPage.pickRemoteFormat(payload.formatCandidates ?? '');
    if (pick === undefined) {
      return;
    }
    if (this.remoteFormatInFlight) {
      this.remoteFormatPending = true;
      this.remotePendingId = pick.id;
      this.remotePendingKind = pick.kind;
      return;
    }
    this.issueRemoteFormat(pick.id, pick.kind);
  }

  private issueRemoteFormat(id: number, kind: number): void {
    this.remoteFormatInFlight = this.native.requestClipboardFormat(id, kind);
  }

  private onRemoteFormatResponse(): void {
    this.remoteFormatInFlight = false;
    if (this.remoteFormatPending) {
      this.remoteFormatPending = false;
      this.issueRemoteFormat(this.remotePendingId, this.remotePendingKind);
    }
  }

  private static pickRemoteFormat(candidates: string): ClipPick | undefined {
    let best: ClipPick | undefined = undefined;
    let bestRank: number = 99;
    for (const entry of candidates.split(',')) {
      const bar: number = entry.indexOf(':');
      if (bar <= 0) {
        continue;
      }
      const kind: number = Number.parseInt(entry.substring(0, bar), 10);
      const id: number = Number.parseInt(entry.substring(bar + 1), 10);
      if (id <= 0) {
        continue;
      }
      const rank: number = SessionPage.clipKindRank(kind);
      if (rank < bestRank) {
        bestRank = rank;
        const pick: ClipPick = new ClipPick();
        pick.kind = kind;
        pick.id = id;
        best = pick;
      }
    }
    return best;
  }

  private static clipKindRank(kind: number): number {
    if (kind === RdpClipKind.FILES) {
      return 0;
    }
    if (kind === RdpClipKind.IMAGE) {
      return 1;
    }
    if (kind === RdpClipKind.HTML) {
      return 2;
    }
    if (kind === RdpClipKind.RTF) {
      return 3;
    }
    if (kind === RdpClipKind.TEXT) {
      return 4;
    }
    return 99;
  }

  private resetClipboardNegotiation(): void {
    this.remoteFormatInFlight = false;
    this.remoteFormatPending = false;
    this.remotePendingId = 0;
    this.remotePendingKind = RdpClipKind.NONE;
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

  private onClipboardImage(payload: RdpEventPayload): void {
    const imageHandle: number = payload.imageHandle ?? 0;
    if (imageHandle <= 0) {
      return;
    }
    const dib: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(imageHandle);
    if (dib === undefined) {
      return;
    }
    BackgroundWorker.decodeDib(dib)
      .then((parsed: DibImage | undefined) => {
        if (parsed === undefined || parsed.width <= 0 || parsed.height <= 0) {
          hilog.warn(DOMAIN, TAG, 'unsupported remote DIB');
          return;
        }
        const options: image.InitializationOptions = {
          editable: false,
          pixelFormat: image.PixelMapFormat.BGRA_8888,
          size: { width: parsed.width, height: parsed.height }
        };
        try {
          this.remoteClipboardPixelMap = image.createPixelMapSync(parsed.bgra, options);
          this.remoteClipboardText = '';
          this.remoteClipboardHtml = '';
        } catch (error) {
          hilog.warn(DOMAIN, TAG, 'create clipboard pixelmap failed: %{public}s',
            JSON.stringify(error));
        }
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'decode dib failed: %{public}s', JSON.stringify(error));
      });
  }

  // One transfer at a time: a second would interleave two state machines in one progress row.
  private fileTransferBlocking(): boolean {
    return this.transferBusy || this.transferActive || this.transferWaiting;
  }

  private rejectConcurrentTransfer(): void {
    this.toast('正在传输文件，请先取消或等待完成');
  }

  private submitClipboard(): void {
    if (this.fileTransferBlocking()) {
      this.rejectConcurrentTransfer();
      return;
    }
    const board: pasteboard.SystemPasteboard = pasteboard.getSystemPasteboard();
    // A URI record means the clipboard may hold files; the metadata check needs no permission.
    let hasFiles: boolean = false;
    try {
      hasFiles = board.hasDataType(pasteboard.MIMETYPE_TEXT_URI);
    } catch (error) {
      hasFiles = false;
    }
    if (hasFiles) {
      this.submitClipboardFiles();
      return;
    }
    this.submitPlainClipboard();
  }

  @SuppressWarnings({ rules: [SuppressWarningsType.PERMISSION] })
  private submitPlainClipboard(): void {
    pasteboard.getSystemPasteboard().getData()
      .then((data: pasteboard.PasteData) => {
        this.pushLocalPasteData(data);
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'read pasteboard failed: %{public}s', JSON.stringify(error));
        this.toast('读取本机剪贴板失败');
      });
  }

  // The system copies the clipboard files into our sandbox, so no READ_PASTEBOARD permission is needed.
  @SuppressWarnings({ rules: [SuppressWarningsType.PERMISSION] })
  private submitClipboardFiles(): void {
    const dir: string | undefined = this.makeSandboxDir('rdp_outbox');
    if (dir === undefined) {
      return;
    }
    this.transferBusy = true;
    const params: pasteboard.GetDataParams = {
      destUri: fileUri.getUriFromPath(dir),
      fileConflictOptions: pasteboard.FileConflictOptions.OVERWRITE,
      progressIndicator: pasteboard.ProgressIndicator.DEFAULT
    };
    pasteboard.getSystemPasteboard().getDataWithProgress(params)
      .then((data: pasteboard.PasteData) => {
        const paths: string[] = SessionPage.listFiles(dir);
        if (paths.length === 0) {
          this.transferBusy = false;
          if (SessionPage.clipboardUriIsFile(data)) {
            // getDataWithProgress cannot copy folders, so a file URI that produced nothing is a folder.
            this.toast('不支持文件夹传输');
            return;
          }
          this.submitPlainClipboard();
          return;
        }
        // cliprdr is pull-based: the button only advertises the files, so release the busy guard.
        this.transferBusy = false;
        const entries: FileDescriptorInput[] = [];
        for (const path of paths) {
          entries.push(FileDescriptorInput.of(SessionPage.baseName(path),
            SessionPage.fileSizeOf(path)));
        }
        this.native.pushClipboardFileOffer(
          FileTransferCodec.buildFileGroupDescriptor(entries), paths);
      })
      .catch((error: BusinessError) => {
        this.transferBusy = false;
        hilog.warn(DOMAIN, TAG, 'read clipboard files failed: %{public}s',
          JSON.stringify(error));
        this.toast('不支持文件夹传输');
      });
  }

  private static clipboardUriIsFile(data: pasteboard.PasteData): boolean {
    try {
      const uri: string = data.getPrimaryUri();
      if (uri.length === 0) {
        return false;
      }
      return !uri.startsWith('http://') && !uri.startsWith('https://');
    } catch (error) {
      return false;
    }
  }

  private onFileTransferProgress(payload: RdpEventPayload): void {
    const direction: number = payload.direction ?? 0;
    const done: number = payload.doneBytes ?? 0;
    const total: number = payload.totalBytes ?? 0;
    this.transferFileIndex = payload.fileIndex ?? 0;
    this.transferFileCount = payload.fileCount ?? 0;
    // An upload only advertises the list (native sends a 0% placeholder); keep the "waiting for the remote paste" hint until bytes move.
    if (direction === 1 && done === 0) {
      this.transferWaiting = true;
      this.refreshToolbarAutoHide();
      return;
    }
    this.transferWaiting = false;
    this.transferBusy = true;
    this.transferActive = true;
    this.transferDirection = direction;
    this.refreshToolbarAutoHide();
    this.transferPercent = total > 0 ? Math.min(100, Math.floor(done * 100 / total)) : 0;
    this.transferText = direction === 0 ? '正在接收文件…' : '正在发送文件…';
    const now: number = Date.now();
    if (this.transferLastAt > 0 && now > this.transferLastAt && done >= this.transferLastDone) {
      const bytesPerSecond: number =
        (done - this.transferLastDone) * 1000 / (now - this.transferLastAt);
      this.transferSpeed = bytesPerSecond > 0 ? this.formatRate(bytesPerSecond) : '';
    }
    this.transferLastDone = done;
    this.transferLastAt = now;
  }

  private writeDownloadedFiles(paths: string[]): void {
    if (paths.length === 0) {
      this.toast('远端文件为空');
      return;
    }
    try {
      const pasteData: pasteboard.PasteData = pasteboard.createData(
        pasteboard.MIMETYPE_TEXT_URI, fileUri.getUriFromPath(paths[0]));
      for (let i = 1; i < paths.length; i++) {
        pasteData.addRecord(pasteboard.MIMETYPE_TEXT_URI, fileUri.getUriFromPath(paths[i]));
      }
      const label: string = paths.length > 1 ? ` ${paths.length} 个文件` : '文件';
      this.writeLocalPasteData(pasteData, label);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'build file pasteboard failed: %{public}s',
        JSON.stringify(error));
      this.toast('写入剪贴板失败');
    }
  }

  private finishFileTransfer(): void {
    this.transferBusy = false;
    this.transferActive = false;
    this.transferWaiting = false;
    this.transferDirection = 0;
    this.transferPercent = 0;
    this.transferText = '';
    this.transferSpeed = '';
    this.transferFileIndex = 0;
    this.transferFileCount = 0;
    this.transferLastDone = 0;
    this.transferLastAt = 0;
    this.refreshToolbarAutoHide();
  }

  private cancelFileTransfer(): void {
    if (this.transferDirection === 1) {
      // Aborting an upload leaves the remote clipboard listing unreadable files, so warn first.
      showConfirm(this.getUIContext(), {
        title: '取消文件传输',
        message: '文件仍在发送。取消后远端粘贴会提示错误，已传部分也会作废。是否继续取消？',
        cancelLabel: '继续传输',
        confirmLabel: '取消传输',
        danger: true,
        onConfirm: () => {
          this.abortFileTransfer();
        }
      });
      return;
    }
    BackgroundWorker.cancelDownload(this.native.handleId());
  }

  private abortFileTransfer(): void {
    this.native.cancelFileTransfer();
    this.finishFileTransfer();
    this.toast('已取消文件传输');
  }

  private makeSandboxDir(name: string): string | undefined {
    let filesDir: string = '';
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx !== undefined) {
        filesDir = ctx.filesDir;
      }
    } catch (error) {
      filesDir = '';
    }
    if (filesDir.length === 0) {
      this.toast('无法访问应用目录');
      return undefined;
    }
    const stamp: string = `${Date.now()}`;
    const root: string = `${filesDir}/${name}`;
    const dir: string = `${root}/${stamp}`;
    try {
      fileIo.mkdirSync(dir, true);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'mkdir %{public}s failed: %{public}s', dir,
        JSON.stringify(error));
      this.toast('无法创建文件目录');
      return undefined;
    }
    SessionPage.purgeStaleTransferDirs(root, stamp);
    return dir;
  }

  // The clipboard only references the newest transfer directory, so the previous one is purged.
  private static purgeStaleTransferDirs(root: string, keep: string): void {
    let names: string[] = [];
    try {
      names = fileIo.listFileSync(root);
    } catch (error) {
      return;
    }
    for (const name of names) {
      if (name === keep) {
        continue;
      }
      const path: string = `${root}/${name}`;
      try {
        // Unlink contents first, in case rmdirSync does not remove a non-empty directory.
        for (const child of fileIo.listFileSync(path)) {
          try {
            fileIo.unlinkSync(`${path}/${child}`);
          } catch (error) {
          }
        }
        fileIo.rmdirSync(path);
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'purge %{public}s failed: %{public}s', path,
          JSON.stringify(error));
      }
    }
  }

  private static listFiles(dir: string): string[] {
    const paths: string[] = [];
    try {
      for (const name of fileIo.listFileSync(dir)) {
        paths.push(`${dir}/${name}`);
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'list %{public}s failed: %{public}s', dir,
        JSON.stringify(error));
    }
    return paths;
  }

  private static baseName(path: string): string {
    const slash: number = path.lastIndexOf('/');
    return slash < 0 ? path : path.substring(slash + 1);
  }

  private static fileSizeOf(path: string): number {
    try {
      return fileIo.statSync(path).size;
    } catch (error) {
      return 0;
    }
  }

  private pushLocalPasteData(data: pasteboard.PasteData): void {
    // Do NOT gate on getMimeTypes(): it may only list a record's default MIME, while HTML lives in a non-default Entry.
    let pixelMap: image.PixelMap | undefined = undefined;
    try {
      pixelMap = data.getPrimaryPixelMap();
    } catch (error) {
      pixelMap = undefined;
    }
    if (pixelMap !== undefined) {
      this.pushLocalImage(pixelMap);
      return;
    }
    SessionPage.findHtmlEntry(data)
      .then((html: string) => {
        if (html.length > 0) {
          // Encode + native push on the worker so a large document cannot block.
          BackgroundWorker.pushHtml(this.native.handleId(), html).then((ok: boolean) => {
            this.toast(ok ? '已放入远端剪贴板' : '提交本机富文本失败');
          });
          return;
        }
        this.pushLocalText(data);
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'read html entry failed: %{public}s', JSON.stringify(error));
        this.pushLocalText(data);
      });
  }

  private static async findHtmlEntry(data: pasteboard.PasteData): Promise<string> {
    const count: number = data.getRecordCount();
    for (let i = 0; i < count; i++) {
      try {
        const record: pasteboard.PasteDataRecord = data.getRecord(i);
        const value: pasteboard.ValueType =
          await record.getData(pasteboard.MIMETYPE_TEXT_HTML);
        if (typeof value === 'string' && value.length > 0) {
          return value;
        }
      } catch (error) {
      }
    }
    return '';
  }

  private pushLocalText(data: pasteboard.PasteData): void {
    let text: string = '';
    try {
      text = data.getPrimaryText();
    } catch (error) {
      text = '';
    }
    if (text.length === 0) {
      this.toast('本机剪贴板为空');
      return;
    }
    BackgroundWorker.pushText(this.native.handleId(), text).then((ok: boolean) => {
      // The clipboard is pull-based: advertise puts it on the remote; the bytes move only when the remote pastes.
      this.toast(ok ? '已放入远端剪贴板' : '提交本机文本失败');
    });
  }

  // The pixel read is a PixelMap API (UI thread); CF_DIB encode and native push run on the worker.
  private pushLocalImage(pixelMap: image.PixelMap): void {
    pixelMap.getImageInfo()
      .then((info: image.ImageInfo) => {
        const width: number = info.size.width;
        const height: number = info.size.height;
        if (width <= 0 || height <= 0) {
          this.toast('本机图片为空');
          return;
        }
        const buffer: ArrayBuffer = new ArrayBuffer(width * height * 4);
        pixelMap.readPixelsToBuffer(buffer)
          .then(() => {
            // `buffer` is transferred to the worker (zero-copy); it encodes CF_DIB and PNG.
            BackgroundWorker.pushImage(this.native.handleId(), buffer, width, height,
              info.pixelFormat).then((pushed: boolean) => {
                if (!pushed) {
                  hilog.warn(DOMAIN, TAG, 'unsupported clipboard pixel format or dead session: %{public}d',
                    info.pixelFormat);
                  this.toast('提交本机图片失败');
                  return;
                }
                this.toast('已放入远端剪贴板');
              });
          })
          .catch((error: BusinessError) => {
            hilog.warn(DOMAIN, TAG, 'read image pixels failed: %{public}s', JSON.stringify(error));
            this.toast('提交本机图片失败');
          });
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'get image info failed: %{public}s', JSON.stringify(error));
        this.toast('提交本机图片失败');
      });
  }

  private pullClipboard(): void {
    if (this.pasteboardBusy) {
      return;
    }
    if (this.fileTransferBlocking()) {
      this.rejectConcurrentTransfer();
      return;
    }
    if (this.remoteFolderCount > 0) {
      // Folders are unsupported, so a list with any directory is refused whole.
      this.toast('不支持文件夹传输');
      return;
    }
    if (this.remoteFileCount > 0) {
      this.startFileDownload();
      return;
    }
    const pixelMap: image.PixelMap | undefined = this.remoteClipboardPixelMap;
    if (pixelMap !== undefined) {
      this.writeLocalPasteData(
        pasteboard.createData(pasteboard.MIMETYPE_PIXELMAP, pixelMap), '图片');
      return;
    }
    if (this.remoteClipboardHtml.length > 0) {
      // Remote HTML is usually a bare fragment; wrap it into a well-formed document.
      const htmlDoc: string = SessionPage.ensureHtmlDocument(this.remoteClipboardHtml);
      const data: pasteboard.PasteData =
        pasteboard.createData(pasteboard.MIMETYPE_TEXT_HTML, htmlDoc);
      const plain: string = SessionPage.htmlToPlainText(htmlDoc);
      if (plain.length > 0) {
        // Same Record, extra Entry: one fragment in two formats, so targets do not treat them as separate items.
        try {
          const record: pasteboard.PasteDataRecord = data.getRecord(0);
          record.addEntry(pasteboard.MIMETYPE_TEXT_PLAIN, plain);
        } catch (error) {
          hilog.warn(DOMAIN, TAG, 'add plain entry failed: %{public}s', JSON.stringify(error));
        }
      }
      this.writeLocalPasteData(data, '富文本');
      return;
    }
    if (this.remoteClipboardText.length > 0) {
      this.writeLocalPasteData(
        pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.remoteClipboardText),
        '文本');
      return;
    }
    this.toast('远端剪贴板为空');
  }

  private startFileDownload(): void {
    if (this.fileTransferBlocking()) {
      this.rejectConcurrentTransfer();
      return;
    }
    const dir: string | undefined = this.makeSandboxDir('rdp_inbox');
    if (dir === undefined) {
      return;
    }
    this.transferBusy = true;
    this.transferActive = true;
    this.transferWaiting = false;
    this.transferDirection = 0;
    this.transferPercent = 0;
    this.transferText = '正在接收文件…';
    this.transferSpeed = '';
    this.transferFileIndex = 0;
    this.transferFileCount = this.remoteFileCount;
    this.transferLastDone = 0;
    this.transferLastAt = 0;
    this.refreshToolbarAutoHide();
    BackgroundWorker.startDownload(this.native.handleId(), dir);
  }

  private onRemoteFileChunk(payload: RdpEventPayload): void {
    BackgroundWorker.postFileChunk(this.native.handleId(),
      payload.listIndex ?? 0, payload.isSize ?? false, payload.sizeValue ?? 0,
      payload.failed ?? false, payload.dataHandle ?? 0);
  }

  private onBackgroundEvent(event: BackgroundEvent): void {
    if (event.handle !== this.native.handleId()) {
      return;
    }
    if (event.op === BackgroundEvents.FILE_LIST_SUMMARY) {
      this.remoteFileCount = event.fileCount ?? 0;
      this.remoteFolderCount = event.folderCount ?? 0;
      hilog.info(DOMAIN, TAG,
        'remote clipboard files: %{public}d folders=%{public}d total=%{public}d',
        this.remoteFileCount, this.remoteFolderCount, event.totalBytes ?? 0);
    } else if (event.op === BackgroundEvents.DOWNLOAD_PROGRESS) {
      this.applyDownloadProgress(event);
    } else if (event.op === BackgroundEvents.DOWNLOAD_DONE) {
      const paths: string[] = event.paths ?? [];
      this.finishFileTransfer();
      hilog.info(DOMAIN, TAG, 'download complete: %{public}d file(s)', paths.length);
      this.writeDownloadedFiles(paths);
    } else if (event.op === BackgroundEvents.DOWNLOAD_FAILED) {
      const reason: string = event.reason ?? '';
      this.finishFileTransfer();
      this.toast(reason.length > 0 ? reason : '文件传输失败');
    }
  }

  private applyDownloadProgress(event: BackgroundEvent): void {
    const done: number = event.doneBytes ?? 0;
    const total: number = event.totalBytes ?? 0;
    this.transferWaiting = false;
    this.transferBusy = true;
    this.transferActive = true;
    this.transferDirection = 0;
    this.transferFileIndex = event.fileIndex ?? 0;
    this.transferFileCount = event.fileCount ?? 0;
    this.transferPercent = total > 0 ? Math.min(100, Math.floor(done * 100 / total)) : 0;
    this.transferText = '正在接收文件…';
    const now: number = Date.now();
    if (this.transferLastAt > 0 && now > this.transferLastAt && done >= this.transferLastDone) {
      const bytesPerSecond: number =
        (done - this.transferLastDone) * 1000 / (now - this.transferLastAt);
      this.transferSpeed = bytesPerSecond > 0 ? this.formatRate(bytesPerSecond) : '';
    }
    this.transferLastDone = done;
    this.transferLastAt = now;
    this.refreshToolbarAutoHide();
  }

  private writeLocalPasteData(data: pasteboard.PasteData, label: string): void {
    this.pasteboardBusy = true;
    pasteboard.getSystemPasteboard().setData(data)
      .then(() => {
        this.pasteboardBusy = false;
        this.toast('已复制' + label + '到本机');
      })
      .catch((error: BusinessError) => {
        this.pasteboardBusy = false;
        hilog.warn(DOMAIN, TAG, 'write pasteboard failed: %{public}s', JSON.stringify(error));
        this.toast('写入本机剪贴板失败');
      });
  }

  // Skips <head>/<style>/<script>/comments so CSS never leaks into plain text; block tags become line breaks.
  private static htmlToPlainText(html: string): string {
    const lower: string = html.toLowerCase();
    let out: string = '';
    let i: number = 0;
    while (i < html.length) {
      const lt: number = html.indexOf('<', i);
      if (lt < 0) {
        out += html.substring(i);
        break;
      }
      out += html.substring(i, lt);
      const gt: number = html.indexOf('>', lt);
      if (gt < 0) {
        break;
      }
      const tag: string = lower.substring(lt + 1, gt).trim();
      let skipUntil: string = '';
      if (tag.startsWith('script')) {
        skipUntil = '</script';
      } else if (tag.startsWith('style')) {
        skipUntil = '</style';
      } else if (tag.startsWith('head')) {
        skipUntil = '</head';
      } else if (tag.startsWith('!--')) {
        skipUntil = '-->';
      }
      if (skipUntil.length > 0) {
        const close: number = lower.indexOf(skipUntil, gt + 1);
        if (close < 0) {
          break;
        }
        const closeGt: number = html.indexOf('>', close);
        i = closeGt < 0 ? html.length : closeGt + 1;
        continue;
      }
      if (tag.startsWith('br') || tag.startsWith('p') || tag.startsWith('/p') ||
        tag.startsWith('div') || tag.startsWith('/div') || tag.startsWith('li') ||
        tag.startsWith('tr') || tag.startsWith('h1') || tag.startsWith('h2') ||
        tag.startsWith('h3')) {
        out += '\n';
      }
      i = gt + 1;
    }
    return SessionPage.decodeHtmlEntities(out).trim();
  }

  private static ensureHtmlDocument(html: string): string {
    if (html.toLowerCase().indexOf('<html') >= 0) {
      return html;
    }
    return '<html><body>' + html + '</body></html>';
  }

  private static decodeHtmlEntities(text: string): string {
    return text.split('&nbsp;').join(' ')
      .split('&amp;').join('&')
      .split('&lt;').join('<')
      .split('&gt;').join('>')
      .split('&quot;').join('"')
      .split('&#39;').join('\'');
  }

  private toast(message: string): void {
    showToast(this.getUIContext(), message, 1500);
  }

  private setToolbarPointerZone(zone: number): void {
    this.shell?.setPointerZone(zone);
  }

  private refreshToolbarAutoHide(): void {
    this.shell?.refreshAutoHide();
  }

  private baseViewRect(): number[] {
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const desktopAspect: number = this.desktopWidth / this.desktopHeight;
    const compAspect: number = compWidth / compHeight;
    if (desktopAspect > compAspect) {
      const viewHeight: number = compWidth / desktopAspect;
      return [0, (compHeight - viewHeight) / 2, compWidth, viewHeight];
    }
    const viewWidth: number = compHeight * desktopAspect;
    return [(compWidth - viewWidth) / 2, 0, viewWidth, compHeight];
  }

  private mapPointer(x: number, y: number): number[] {
    const rect: number[] = this.baseViewRect();
    const viewX: number = rect[0];
    const viewY: number = rect[1];
    const viewWidth: number = rect[2];
    const viewHeight: number = rect[3];
    const normalizedX: number = (x - viewX) / viewWidth;
    const normalizedY: number = (y - viewY) / viewHeight;
    const clampedX: number = Math.min(1, Math.max(0, normalizedX));
    const clampedY: number = Math.min(1, Math.max(0, normalizedY));
    const remoteX: number = Math.round(clampedX * (this.desktopWidth - 1));
    const remoteY: number = Math.round(clampedY * (this.desktopHeight - 1));
    return [remoteX, remoteY];
  }

  private handleMouse(event: MouseEvent): void {
    this.setToolbarPointerZone(event.y < this.revealBand
      ? TOOLBAR_ZONE_TRIGGER : TOOLBAR_ZONE_NONE);
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
    const rect: number[] = this.baseViewRect();
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const cx: number = compWidth / 2;
    const cy: number = compHeight / 2;
    const unclampedX: number = this.viewPinchAnchorX +
      scale * (rect[0] - this.viewPinchAnchorX) + this.viewPinchPanX;
    const unclampedY: number = this.viewPinchAnchorY +
      scale * (rect[1] - this.viewPinchAnchorY) + this.viewPinchPanY;
    const left: number = this.clampViewEdge(unclampedX, compWidth, rect[2] * scale);
    const top: number = this.clampViewEdge(unclampedY, compHeight, rect[3] * scale);
    const panX: number = this.viewPinchPanX + (left - unclampedX);
    const panY: number = this.viewPinchPanY + (top - unclampedY);
    this.viewScale = scale;
    this.viewTransX = (1 - scale) * (this.viewPinchAnchorX - cx) + panX;
    this.viewTransY = (1 - scale) * (this.viewPinchAnchorY - cy) + panY;
    this.commitViewMatrix();
  }

  private panView(dx: number, dy: number): void {
    const rect: number[] = this.baseViewRect();
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const cx: number = compWidth / 2;
    const cy: number = compHeight / 2;
    const scale: number = this.viewScale;
    const width: number = rect[2] * scale;
    const height: number = rect[3] * scale;
    const left: number = this.clampViewEdge(
      cx + scale * (rect[0] - cx) + this.viewTransX + dx, compWidth, width);
    const top: number = this.clampViewEdge(
      cy + scale * (rect[1] - cy) + this.viewTransY + dy, compHeight, height);
    this.viewTransX = left - cx - scale * (rect[0] - cx);
    this.viewTransY = top - cy - scale * (rect[1] - cy);
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
      if (this.transferWaiting) {
        Row() {
          LoadingProgress()
            .width(14)
            .height(14)
            .color(TOOLBAR_FG)
          Text('在远端粘贴开始传输')
            .fontSize(12)
            .fontColor(TOOLBAR_FG)
            .margin({ left: 6 })
            .maxLines(1)
          OutlineChip({
            label: '取消',
            onTap: () => {
              this.abortFileTransfer();
            }
          })
            .margin({ left: 8 })
        }
        .margin({ right: 12 })
      } else if (this.transferActive) {
        Row() {
          Text(this.transferText)
            .fontSize(12)
            .fontColor(TOOLBAR_FG)
            .maxLines(1)
          if (this.transferFileCount > 1 && this.transferFileIndex > 0) {
            Text(`${this.transferFileIndex}/${this.transferFileCount}`)
              .fontSize(12)
              .fontColor(TOOLBAR_FG)
              .margin({ left: 6 })
              .maxLines(1)
          }
          Text(`${this.transferPercent}%`)
            .fontSize(12)
            .fontColor(TOOLBAR_FG)
            .margin({ left: 6 })
            .maxLines(1)
          if (this.transferSpeed.length > 0) {
            Text(this.transferSpeed)
              .fontSize(12)
              .fontColor(TOOLBAR_FG)
              .margin({ left: 6 })
              .maxLines(1)
          }
          Progress({ value: this.transferPercent, total: 100, type: ProgressType.Linear })
            .width(140)
            .color(TOOLBAR_PROGRESS)
            .margin({ left: 8 })
          OutlineChip({
            label: '取消',
            onTap: () => {
              this.cancelFileTransfer();
            }
          })
            .margin({ left: 8 })
        }
        .margin({ right: 12 })
      }
      ToolbarChip({
        label: '复制',
        chipBg: TOOLBAR_ACCENT_CHIP_BG,
        icon: $r('sys.media.ohos_ic_public_copy'),
        hPad: 14,
        onTap: () => {
          this.pullClipboard();
        }
      })
        .bindTips('从远端复制到本机剪贴板', this.tipsOptions)
      // Clicking grants a one-time clipboard read without the READ_PASTEBOARD permission.
      Row() {
        PasteButton({
          icon: PasteIconStyle.LINES,
          text: PasteDescription.PASTE,
          buttonType: ButtonType.Normal
        })
          .iconSize(16)
          .iconColor(TOOLBAR_FG)
          .fontSize(13)
          .fontColor(TOOLBAR_FG)
          .backgroundColor(TOOLBAR_ACCENT_CHIP_BG)
          .borderRadius(8)
          .padding({ left: 14, right: 14, top: 7, bottom: 7 })
          .onClick(this.pasteButtonCallback)
      }
      .margin({ left: 8 })
      .bindTips('把本机剪贴板发送到远端', this.tipsOptions)
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
    Row() {
      MetricItem({
        text: this.rttLabel(),
        metricWidth: METRIC_WIDTH_NETWORK,
        fgColor: this.thresholdColor(this.metricsRtt, NETWORK_WARN_MS, NETWORK_BAD_MS)
      })
      MetricItem({
        text: this.localLabel(),
        metricWidth: METRIC_WIDTH_LOCAL,
        fgColor: this.thresholdColor(this.localMs(), LOCAL_WARN_MS, LOCAL_BAD_MS),
        leftGap: METRIC_GAP_TIGHT,
        tip: this.localTips()
      })
      MetricItem({
        text: this.fpsLabel(),
        metricWidth: METRIC_WIDTH_FPS,
        leftGap: METRIC_GAP_GROUP
      })
      MetricItem({
        text: this.cpuLabel(),
        metricWidth: METRIC_WIDTH_CPU,
        leftGap: METRIC_GAP_GROUP
      })
      MetricItem({
        text: this.gpuLabel(),
        metricWidth: METRIC_WIDTH_GPU,
        leftGap: METRIC_GAP_GROUP,
        tip: this.gpuTips()
      })
      if (this.hasBandwidth()) {
        MetricItem({
          text: this.bandwidthLabel(),
          leftGap: METRIC_GAP_GROUP
        })
      }
      if (this.metricsAudioRateHz > 0) {
        MetricItem({
          text: this.audioLabel(),
          metricWidth: METRIC_WIDTH_AUDIO,
          fgColor: this.audioColor(),
          leftGap: METRIC_GAP_GROUP
        })
      }
    }
    .width('100%')
    .height(TOOLBAR_TELEMETRY_HEIGHT)
    .padding({ left: 16, right: 16 })
  }

  build() {
    Column() {
      if (!this.isFullscreen && !this.showOverlay) {
        this.sessionToolbar()
      }
      Stack({ alignContent: Alignment.TopStart }) {
        // The picture lives in its own layer so direct pinch can scale it locally; the Stack clips it.
        Column() {
          XComponent({
            id: 'rdpSurface',
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
              // The system may drop a custom cursor when the pointer leaves and re-enters, so re-install the last shape.
              if (isHover) {
                this.cursor.reapply();
              }
            })
            .onBlur(() => {
              this.releaseAllInputs();
            })
            .onTouch((event: TouchEvent) => {
              this.handleTouch(event);
            })
            .onAxisEvent((event: AxisEvent) => {
              this.handleAxis(event);
            })
            .onKeyEvent((event: KeyEvent) => {
              this.handleKey(event);
            })
            .onAreaChange((oldArea: Area, newArea: Area) => {
              const width: number = lengthToNumber(newArea.width);
              const height: number = lengthToNumber(newArea.height);
              if (this.componentWidth !== width || this.componentHeight !== height) {
                this.resetViewZoom();
              }
              this.componentWidth = width;
              this.componentHeight = height;
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
