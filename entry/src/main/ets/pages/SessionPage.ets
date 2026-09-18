/*
 * HmRdp - remote session page: XComponent surface + input forwarding.
 */
import { common } from '@kit.AbilityKit';
import { util } from '@kit.ArkTS';
import { image } from '@kit.ImageKit';
import { pointer } from '@kit.InputKit';
import { window } from '@kit.ArkUI';
import { BusinessError, pasteboard } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PendingConnection, RdpConnectOptions, RdpConnectionState,
  RdpEvent, RdpPointerFlags, RdpTouchFlags, SessionRequests } from '../model/RdpModels';
import { RdpNative } from '../services/RdpNative';
import { DeviceCapabilities } from '../services/DeviceCapabilities';
import { SessionManager } from '../services/SessionManager';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
import { TouchpadWheelMapper, WheelChunk, WHEEL_DETENT, DEFAULT_PINCH_NOTCHES_PER_UNIT,
  MIN_SPEED_SCALE, MAX_SPEED_SCALE } from '../services/TouchpadWheel';
import { WindowController } from '../services/WindowController';
import { KeyMapper, RdpScanCode } from '../utils/KeyMapper';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSession';

// Synthetic touch pinch geometry. The initial finger separation is a percentage
// of the smaller desktop side (window-relative, never a fixed resolution); see
// the pinchTouchDistance setting.
const DEFAULT_PINCH_DISTANCE_PERCENT: number = 4;
const MIN_PINCH_DISTANCE_PERCENT: number = 1;
const MAX_PINCH_DISTANCE_PERCENT: number = 20;
const MIN_PINCH_HALF: number = 8;
// axisPinch is a distance ratio relative to the gesture start; clamp it so a
// bad sample can never explode or collapse the synthetic finger separation.
const MIN_PINCH_RATIO: number = 0.05;
const MAX_PINCH_RATIO: number = 20;
// The pinch (and its synthetic touch contacts / held Ctrl) lives from the first
// PINCH axis event until the OS sends the real AxisAction.END, i.e. the fingers
// are actually lifted. No timers are used, so a stationary hold keeps the
// gesture alive instead of lifting the contacts and producing a fake click.
// External RDPEI ids for the synthetic pinch contacts; real screen touches use
// touch.id + 1 (1..11), so these never collide.
const PINCH_TOUCH_ID_A: number = 20;
const PINCH_TOUCH_ID_B: number = 21;
// The synthetic pinch's first MOTION must not reach the native RDPEI layer
// before its DOWN frame has been flushed: FreeRDP coalesces touch frames at
// 50Hz by default (HmrdpSetTouchFrameInterval(20)), so a MOTION inside that
// window overwrites the unsent DOWN and the remote never engages the contacts.
// This margin decouples the app from the coalescing interval (with the
// high-rate option the interval is 0, so the delay is a no-op in practice) and
// is far below the time Windows needs to recognise a pinch.
const PINCH_TOUCH_DOWN_FLUSH_MS: number = 30;

// Toolbar throughput below this is idle/keep-alive noise, so it is not shown.
const MIN_BANDWIDTH_BYTES: number = 1024;

// Fixed widths for the toolbar telemetry so a changing number never reflows the
// items after it. Sized close to the common maximum so the unused space inside
// a box stays small and the gaps are governed by the margins below.
const METRIC_WIDTH_NETWORK: number = 66;
const METRIC_WIDTH_LOCAL: number = 74;
const METRIC_WIDTH_FPS: number = 66;
const METRIC_WIDTH_AUDIO: number = 86;
// Two gap sizes: the network/local pair sits one character apart, the rest
// share a wider gap.
const METRIC_GAP_TIGHT: number = 6;
const METRIC_GAP_GROUP: number = 8;

// Latency colour thresholds. The network RTT is allowed to be fairly high
// before it warns (it is dominated by distance), while the local per-frame work
// is held to a much tighter budget because it is entirely under our control:
// over WARN_MS the client cannot reach 60 fps and over BAD_MS it cannot reach
// 30 fps, whatever the network delivers.
const NETWORK_WARN_MS: number = 100;
const NETWORK_BAD_MS: number = 200;
const LOCAL_WARN_MS: number = 16;
const LOCAL_BAD_MS: number = 33;

// A TouchType.Cancel does not always mean the finger lifted (a move can be
// interrupted). The contact is held instead, and a following Down within this
// distance of its last position is treated as the same finger resuming. Only if
// no such event arrives within CANCEL_HOLD_MS is the contact really released.
const CANCEL_RESUME_RATIO: number = 0.02;
const CANCEL_RESUME_MIN_DISTANCE: number = 16;
const CANCEL_RESUME_MAX_DISTANCE: number = 64;
const CANCEL_HOLD_MS: number = 400;

class RdpSurfaceController extends XComponentController {
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
struct SessionPage {
  @State state: RdpConnectionState = RdpConnectionState.Connecting;
  @State statusText: string = '正在连接...';
  @State showOverlay: boolean = true;
  @State showToolbar: boolean = true;
  @State isFullscreen: boolean = false;
  // Identifies this window's launch request, injected by SessionAbility.
  @LocalStorageLink('sessionKey') sessionKey: string = '';

  private native: RdpNative = new RdpNative();
  private options: RdpConnectOptions = new RdpConnectOptions();
  private connectionId: string = '';
  // True when the manager already connected before this window was opened.
  private preconnected: boolean = false;
  private closing: boolean = false;
  private controller: RdpSurfaceController = new RdpSurfaceController();
  private componentWidth: number = 0;
  private componentHeight: number = 0;
  private desktopWidth: number = 1920;
  private desktopHeight: number = 1080;
  // Live session telemetry pushed once per second by the native side. @State so
  // the toolbar refreshes on every sample.
  @State metricsRtt: number = -1;
  @State metricsRx: number = 0;
  @State metricsTx: number = 0;
  @State metricsFps: number = 0;
  // Client work per frame in microseconds (ZGX decompress + RDPGFX parse +
  // decode + gdi compose + present); 0 = no frame ended in the window.
  @State metricsLocalUs: number = 0;
  // The same per-frame figure split by phase (microseconds) plus its
  // normalization: GFX commands and raw bytes per frame, and the duty cycle in
  // 0.1% units. Shown in the 本机 tooltip.
  @State metricsZgxUs: number = 0;
  @State metricsDecodeUs: number = 0;
  @State metricsComposeUs: number = 0;
  @State metricsPresentUs: number = 0;
  @State metricsBytesPerFrame: number = 0;
  @State metricsDutyPermille: number = 0;
  @State metricsCmdsPerFrame: number = 0;
  // Audio sample rate (Hz) and recent glitch rate in basis points (0.01%);
  // -1 when audio is not playing.
  @State metricsAudioRateHz: number = -1;
  @State metricsAudioLossBp: number = -1;
  private surfaceId: string = '';
  private surfaceWidth: number = 0;
  private surfaceHeight: number = 0;
  private hoverTimer: number = -1;
  private hideTimer: number = -1;
  private topTrigger: number = 6;
  private toolbarHeight: number = 48;
  private hoverDelay: number = 350;
  private hideDelay: number = 250;
  // Active touch contacts, keyed by the ArkUI touch id. The value is the last
  // remote point sent for that contact (used to drop no-op moves).
  private touchPoints: Map<number, number[]> = new Map<number, number[]>();
  // ArkUI touch id -> remote RDPEI contact id, kept separate so a contact can be
  // resumed after a Cancel even if ArkUI renumbers the finger.
  private touchContactIds: Map<number, number> = new Map<number, number>();
  // Contacts whose Cancel arrived but that may still be down (see CANCEL_*).
  private pendingCancelIds: number[] = [];
  private cancelTimers: Map<number, number> = new Map<number, number>();
  // Touchpad scroll: ArkUI reports raw finger displacement (vp); the mapper
  // converts it into high-resolution RDP wheel chunks.
  private touchpadWheel: TouchpadWheelMapper = new TouchpadWheelMapper();
  private wheelChunks: WheelChunk[] = [];
  // Touchpad pinch state: axisPinch is the ratio of the current two-finger
  // distance to the distance when the pinch started (starts at 1).
  private pinchScale: number = 0;
  private pinchAccum: number = 0;
  private pinchActive: boolean = false;
  private pinchCtrlDown: boolean = false;
  private pinchZoomSpeed: number = 1;
  private readonly ctrlScancode: number = 0x1D;
  // Pinch translation: false = Ctrl+wheel (native RDP behaviour), true = a
  // synthetic native two-finger touch pinch around the pointer.
  private pinchAsTouch: boolean = false;
  private pinchTouchDistance: number = DEFAULT_PINCH_DISTANCE_PERCENT;
  private touchPinchActive: boolean = false;
  private touchPinchAnchor: number[] = [0, 0];
  private touchPinchHalf: number = MIN_PINCH_HALF;
  private touchPinchLastHalf: number = MIN_PINCH_HALF;
  // Earliest time a MOTION may be forwarded after the synthetic DOWN (see
  // PINCH_TOUCH_DOWN_FLUSH_MS); 0 while no touch pinch is active.
  private touchPinchMotionAt: number = 0;
  // Orientation of the synthetic contact pair (degrees, clockwise on screen).
  // ArkUI cannot report the real finger angle, so the pair is tilted by this
  // configured amount; the unit vector is cached when a pinch starts.
  private touchPinchAngle: number = 30;
  private touchPinchUnitX: number = 1;
  private touchPinchUnitY: number = 0;
  private sessionWindow: window.Window | undefined = undefined;
  // Clipboard redirection is manual, driven by the toolbar. The remote
  // clipboard is cached when the server offers it; "复制" pulls it to the local
  // clipboard and "粘贴" (system paste control, no READ_PASTEBOARD) pushes the
  // local clipboard to the remote.
  private clipboardEnabled: boolean = true;
  // Latest rich payload pulled from the remote. Only one is populated at a time
  // (native requests the richest supported format per remote copy), so "复制"
  // can pick by presence alone.
  private remoteClipboardText: string = '';
  private remoteClipboardHtml: string = '';
  private remoteClipboardPixelMap: image.PixelMap | undefined = undefined;
  private pasteboardBusy: boolean = false;
  // Remote cursor shape, installed as the HarmonyOS system cursor. The server
  // sends the full bitmap + hot spot; we keep the last PixelMap so it can be
  // re-installed when the window re-acquires the pointer. `cursorHidden` tracks
  // the server's SYSPTR_NULL (e.g. a full-screen video hides the cursor).
  private cursorPixelMap: image.PixelMap | undefined = undefined;
  private cursorHotX: number = 0;
  private cursorHotY: number = 0;
  private cursorHidden: boolean = false;
  // Set once a cursor API call fails, so we stop retrying on every update.
  private cursorSupported: boolean = true;
  // Global "使用RDP光标" setting; when off the session leaves the system cursor
  // alone and the native side ignores RDP pointer updates entirely.
  private useRdpCursor: boolean = true;

  // The system paste control grants a one-time clipboard read without the
  // READ_PASTEBOARD permission; on success we push the text to the remote.
  private pasteButtonCallback: PasteButtonCallback =
    (event: ClickEvent, result: PasteButtonOnClickResult): void => {
      if (result === PasteButtonOnClickResult.SUCCESS) {
        this.submitClipboard();
        return;
      }
      hilog.warn(DOMAIN, TAG, 'paste authorization failed');
      this.toast('未获得剪贴板授权');
    };
  // KeyCode the user remapped to Win/Meta for the remote (0 = disabled). Lets
  // Win combinations work even though the shell keeps the real Win key.
  private winKeySubstitute: number = 0;

  aboutToAppear(): void {
    this.resolveRequest();
    const settings: AppSettings = SettingsStore.load();
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.touchpadWheel.setSpeed(settings.touchpadScrollSpeed);
    this.pinchZoomSpeed =
      Math.max(MIN_SPEED_SCALE, Math.min(MAX_SPEED_SCALE, settings.pinchZoomSpeed));
    this.pinchAsTouch = settings.pinchAsTouch;
    this.touchPinchAngle = settings.pinchTouchAngle;
    this.pinchTouchDistance = settings.pinchTouchDistance;
    RdpNative.applyTouchHighRate(settings.touchHighRate);
    // Never ask for Vulkan presentation on a device that cannot do it (the
    // settings page already forces the stored value off; this covers a config
    // imported from another device).
    RdpNative.applyHardwareAccel(settings.hardwareAccel
      && DeviceCapabilities.hardwareAccel().supported);
    // Process-global decode worker count (0 = automatic). The native decoder
    // module currently pins it to 1 while the parallel path is reworked, so the
    // stored value is forwarded but not in effect (doc_agent/cpu-accel-plan.md §2).
    RdpNative.applyDecodeThreads(settings.decodeThreads);
    let filesDir: string = '';
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx !== undefined) {
        filesDir = ctx.filesDir;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'filesDir unavailable: %{public}s', JSON.stringify(error));
    }
    RdpNative.applyRfxDump(settings.rfxDump, filesDir);
    hilog.info(DOMAIN, TAG, 'rfx dump=%{public}d dir=%{public}s', settings.rfxDump ? 1 : 0,
      filesDir);
    this.useRdpCursor = settings.useRdpCursor;
    RdpNative.applyRdpCursor(settings.useRdpCursor);
    this.winKeySubstitute = settings.winKeySubstitute;
    this.clipboardEnabled = this.options.enableClipboard;
    this.desktopWidth = this.options.width;
    this.desktopHeight = this.options.height;
    this.loadSessionWindow();
    this.controller.setCallbacks(
      (surfaceId: string, width: number, height: number) => {
        this.surfaceId = surfaceId;
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        this.native.bindSurface(surfaceId, width, height);
      },
      (surfaceId: string, width: number, height: number) => {
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        this.native.resizeSurface(width, height);
      },
      (surfaceId: string) => {
        this.native.unbindSurface();
      }
    );
    if (this.preconnected) {
      // The manager already connected and owns the event listener; this window
      // only has to bind the surface and render the live session.
      this.state = RdpConnectionState.Connected;
      this.statusText = '已连接';
      this.showOverlay = false;
      this.showToolbar = false;
    } else {
      this.native.setListener((event: RdpEvent, data: string) => {
        this.onRdpEvent(event, data);
      });
      this.startSession();
    }
  }

  aboutToDisappear(): void {
    if (this.hoverTimer !== -1) {
      clearTimeout(this.hoverTimer);
      this.hoverTimer = -1;
    }
    if (this.hideTimer !== -1) {
      clearTimeout(this.hideTimer);
      this.hideTimer = -1;
    }
    this.releaseAllTouches();
    this.endPinch();
    this.restoreCursor();
    this.native.stopSession();
    this.options.password = '';
    this.options.gatewayPassword = '';
  }

  /**
   * Resolves this window's launch request by its session key and registers the
   * native session instance so the ability can tear down exactly this one.
   */
  private resolveRequest(): void {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    if (request !== undefined) {
      this.options = request.options;
      this.connectionId = request.connectionId;
    }
    const existing: RdpNative | undefined = RdpNative.findByKey(this.sessionKey);
    if (existing !== undefined) {
      this.native = existing;
    }
    this.preconnected = this.connectionId.length > 0 &&
      SessionManager.isConnected(this.connectionId);
    RdpNative.register(this.sessionKey, this.native);
    SessionManager.attachWindow(this.sessionKey,
      (event: RdpEvent, data: string) => {
        this.onRdpEvent(event, data);
      },
      () => {
        this.terminateWindow();
      });
  }

  private startSession(): void {
    this.state = RdpConnectionState.Connecting;
    this.statusText = '正在连接...';
    this.showOverlay = true;
    this.showToolbar = true;
    const handle: number = this.native.startSession(this.options);
    if (handle < 0) {
      this.state = RdpConnectionState.Failed;
      this.statusText = '会话创建失败';
      this.showOverlay = true;
    }
  }

  private retry(): void {
    if (this.connectionId.length > 0) {
      this.state = RdpConnectionState.Connecting;
      this.statusText = '正在连接...';
      this.showOverlay = true;
      this.showToolbar = true;
      SessionManager.retry(this.connectionId);
    } else {
      this.startSession();
    }
    // Re-bind the surface: stopping a session destroyed its native window.
    if (this.surfaceId.length > 0) {
      this.native.bindSurface(this.surfaceId, this.surfaceWidth, this.surfaceHeight);
    }
  }

  private onRdpEvent(event: RdpEvent, data: string): void {
    if (event === RdpEvent.Connected) {
      this.state = RdpConnectionState.Connected;
      this.statusText = '已连接';
      this.showToolbar = false;
    } else if (event === RdpEvent.FirstFrame) {
      this.showOverlay = false;
      this.showToolbar = false;
    } else if (event === RdpEvent.Disconnected) {
      this.state = RdpConnectionState.Closed;
      this.statusText = data.length > 0 ? SessionManager.describeError(data) : '连接已断开';
      this.showOverlay = true;
      this.showToolbar = true;
      this.resetCursor();
    } else if (event === RdpEvent.Error) {
      this.state = RdpConnectionState.Failed;
      this.statusText = data.length > 0 ? SessionManager.describeError(data) : '连接失败';
      this.showOverlay = true;
      this.showToolbar = true;
      this.resetCursor();
    } else if (event === RdpEvent.Resize) {
      const parts: string[] = data.split('x');
      if (parts.length === 2) {
        const width: number = Number.parseInt(parts[0], 10);
        const height: number = Number.parseInt(parts[1], 10);
        if (!Number.isNaN(width) && !Number.isNaN(height) && width > 0 && height > 0) {
          this.desktopWidth = width;
          this.desktopHeight = height;
        }
      }
    } else if (event === RdpEvent.ClipboardText) {
      hilog.info(DOMAIN, TAG, 'remote clipboard text: %{public}d chars', data.length);
      this.remoteClipboardText = data;
      this.remoteClipboardHtml = '';
      this.remoteClipboardPixelMap = undefined;
    } else if (event === RdpEvent.ClipboardHtml) {
      hilog.info(DOMAIN, TAG, 'remote clipboard html: %{public}d chars', data.length);
      this.remoteClipboardHtml = data;
      this.remoteClipboardText = '';
      this.remoteClipboardPixelMap = undefined;
    } else if (event === RdpEvent.ClipboardImage) {
      hilog.info(DOMAIN, TAG, 'remote clipboard image: %{public}d chars', data.length);
      this.onClipboardImage(data);
    } else if (event === RdpEvent.CursorShape) {
      this.onCursorShape(data);
    } else if (event === RdpEvent.CursorDefault) {
      this.cursorHidden = false;
      this.cursorPixelMap = undefined;
      this.applyCursor();
    } else if (event === RdpEvent.CursorHidden) {
      this.cursorHidden = true;
      this.applyCursor();
    } else if (event === RdpEvent.Metrics) {
      this.onMetrics(data);
    }
  }

  /**
   * Parses "<rttMs>|<rxBps>|<txBps>|<fps>|<localUs>|<audioRateHz>|<audioLossBp>|
   * <zgxParseUs>|<decodeUs>|<composeUs>|<presentUs>|<bytesPerFrame>|<dutyPermille>|
   * <cmdsPerFrame>|<syncUs>". rtt/rx/tx/fps/localUs/audio drive the toolbar row;
   * the rest are the per-frame breakdown of localUs shown in the 本机 tooltip
   * (see RdpEvent.Metrics).
   */
  private onMetrics(data: string): void {
    const parts: string[] = data.split('|');
    if (parts.length < 7) {
      return;
    }
    const rtt: number = Number.parseInt(parts[0], 10);
    const rx: number = Number.parseInt(parts[1], 10);
    const tx: number = Number.parseInt(parts[2], 10);
    const fps: number = Number.parseInt(parts[3], 10);
    const localUs: number = Number.parseInt(parts[4], 10);
    const audioRateHz: number = Number.parseInt(parts[5], 10);
    const audioLossBp: number = Number.parseInt(parts[6], 10);
    if (!Number.isNaN(rtt)) {
      this.metricsRtt = rtt;
    }
    if (!Number.isNaN(rx)) {
      this.metricsRx = rx;
    }
    if (!Number.isNaN(tx)) {
      this.metricsTx = tx;
    }
    if (!Number.isNaN(fps)) {
      this.metricsFps = fps;
    }
    if (!Number.isNaN(localUs)) {
      this.metricsLocalUs = localUs;
    }
    if (!Number.isNaN(audioRateHz)) {
      this.metricsAudioRateHz = audioRateHz;
    }
    if (!Number.isNaN(audioLossBp)) {
      this.metricsAudioLossBp = audioLossBp;
    }
    if (parts.length >= 13) {
      const zgxUs: number = Number.parseInt(parts[7], 10);
      const decodeUs: number = Number.parseInt(parts[8], 10);
      const composeUs: number = Number.parseInt(parts[9], 10);
      const presentUs: number = Number.parseInt(parts[10], 10);
      const bytesPerFrame: number = Number.parseInt(parts[11], 10);
      const dutyPermille: number = Number.parseInt(parts[12], 10);
      const cmdsPerFrame: number = parts.length >= 14 ? Number.parseInt(parts[13], 10) : NaN;
      if (!Number.isNaN(zgxUs)) {
        this.metricsZgxUs = zgxUs;
      }
      if (!Number.isNaN(decodeUs)) {
        this.metricsDecodeUs = decodeUs;
      }
      if (!Number.isNaN(composeUs)) {
        this.metricsComposeUs = composeUs;
      }
      if (!Number.isNaN(presentUs)) {
        this.metricsPresentUs = presentUs;
      }
      if (!Number.isNaN(bytesPerFrame)) {
        this.metricsBytesPerFrame = bytesPerFrame;
      }
      if (!Number.isNaN(dutyPermille)) {
        this.metricsDutyPermille = dutyPermille;
      }
      if (!Number.isNaN(cmdsPerFrame)) {
        this.metricsCmdsPerFrame = cmdsPerFrame;
      }
    }
  }

  /** "网络 28ms", or "网络 --" until the server reports network stats. */
  private rttLabel(): string {
    return this.metricsRtt < 0 ? '网络 --' : '网络 ' + this.metricsRtt + 'ms';
  }

  /** Green / amber / red vs. the given warn/bad thresholds (ms). */
  private thresholdColor(ms: number, warnMs: number, badMs: number): string {
    if (ms < 0) {
      return '#B8BCC4';
    }
    if (ms <= warnMs) {
      return '#4ADE80';
    }
    if (ms <= badMs) {
      return '#FBBF24';
    }
    return '#F87171';
  }

  /**
   * Client work per frame in ms (ZGX decompress + parse + decode + compose +
   * present), or -1 when no frame ended in the window. Reported to the ones
   * place like the other latency figures so the row stays comparable.
   */
  private localMs(): number {
    return this.metricsLocalUs > 0 ? this.metricsLocalUs / 1000 : -1;
  }

  private localLabel(): string {
    const ms: number = this.localMs();
    return ms < 0 ? '本机 --' : '本机 ' + Math.round(ms) + 'ms';
  }

  private usToMs(us: number): number {
    return Math.round(us / 1000);
  }

  /** "本机" tooltip: the per-frame breakdown, every figure to the ones place. */
  private localTips(): string {
    return '解压+解析 ' + this.usToMs(this.metricsZgxUs) + 'ms' +
      ' / 解码 ' + this.usToMs(this.metricsDecodeUs) + 'ms' +
      ' / 合成 ' + this.usToMs(this.metricsComposeUs) + 'ms' +
      ' / 上屏 ' + this.usToMs(this.metricsPresentUs) + 'ms' +
      '\n载荷 ' + Math.round(this.metricsBytesPerFrame / 1024) + 'KB/帧' +
      '（' + this.metricsCmdsPerFrame + ' 条命令）' +
      '，本机工时占帧周期 ' + Math.round(this.metricsDutyPermille / 10) + '%';
  }

  /** "FPS <app>": the client's own present rate. */
  private fpsLabel(): string {
    return 'FPS ' + this.metricsFps;
  }

  /** Audio sample rate and recent glitch rate, e.g. "音频 48k 0.05%". */
  private audioLabel(): string {
    return '音频 ' + Math.round(this.metricsAudioRateHz / 1000) + 'k ' +
      (this.metricsAudioLossBp / 100).toFixed(2) + '%';
  }

  /** Any recent glitch is worth flagging: 0 green, <=0.5% amber, else red. */
  private audioColor(): string {
    if (this.metricsAudioLossBp < 0) {
      return '#B8BCC4';
    }
    if (this.metricsAudioLossBp === 0) {
      return '#4ADE80';
    }
    if (this.metricsAudioLossBp <= 50) {
      return '#FBBF24';
    }
    return '#F87171';
  }

  /** Compact byte/s label for the toolbar. */
  private formatRate(bytesPerSec: number): string {
    if (bytesPerSec >= 1024 * 1024) {
      return (bytesPerSec / (1024 * 1024)).toFixed(1) + 'MB/s';
    }
    if (bytesPerSec >= 1024) {
      return Math.round(bytesPerSec / 1024) + 'KB/s';
    }
    return bytesPerSec + 'B/s';
  }

  /** True when either direction carries meaningful (non-idle) traffic. */
  private hasBandwidth(): boolean {
    return this.metricsRx >= MIN_BANDWIDTH_BYTES || this.metricsTx >= MIN_BANDWIDTH_BYTES;
  }

  /** Per-direction throughput label, dropping directions below the threshold. */
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

  /** Decodes a remote cursor bitmap and installs it as the system cursor. */
  private onCursorShape(data: string): void {
    if (!this.cursorSupported || !this.useRdpCursor) {
      return;
    }
    const separator: number = data.indexOf('|');
    if (separator <= 0) {
      return;
    }
    const header: string[] = data.substring(0, separator).split(',');
    if (header.length !== 4) {
      return;
    }
    const width: number = Number.parseInt(header[0], 10);
    const height: number = Number.parseInt(header[1], 10);
    const hotX: number = Number.parseInt(header[2], 10);
    const hotY: number = Number.parseInt(header[3], 10);
    if (Number.isNaN(width) || Number.isNaN(height) || width <= 0 || height <= 0) {
      return;
    }
    // Native already downscales oversized cursors to <=256; this is only a
    // safety net should an out-of-range size ever reach the UI.
    if (width > 256 || height > 256) {
      this.cursorHidden = false;
      this.cursorPixelMap = undefined;
      this.applyCursor();
      return;
    }
    let bytes: Uint8Array;
    try {
      bytes = new util.Base64Helper().decodeSync(data.substring(separator + 1));
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'decode cursor bitmap failed: %{public}s', JSON.stringify(error));
      return;
    }
    const options: image.InitializationOptions = {
      editable: false,
      pixelFormat: image.PixelMapFormat.BGRA_8888,
      size: { width: width, height: height }
    };
    try {
      this.cursorPixelMap = image.createPixelMapSync(bytes.buffer, options);
      this.cursorHotX = Math.max(0, Math.min(hotX, width));
      this.cursorHotY = Math.max(0, Math.min(hotY, height));
      this.cursorHidden = false;
      this.applyCursor();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'create cursor pixelmap failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Decodes a remote clipboard image ("<w>,<h>|<base64 BGRA>") into a PixelMap. */
  private onClipboardImage(data: string): void {
    const separator: number = data.indexOf('|');
    if (separator <= 0) {
      return;
    }
    const header: string[] = data.substring(0, separator).split(',');
    if (header.length !== 2) {
      return;
    }
    const width: number = Number.parseInt(header[0], 10);
    const height: number = Number.parseInt(header[1], 10);
    if (Number.isNaN(width) || Number.isNaN(height) || width <= 0 || height <= 0) {
      return;
    }
    let bytes: Uint8Array;
    try {
      bytes = new util.Base64Helper().decodeSync(data.substring(separator + 1));
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'decode clipboard image failed: %{public}s', JSON.stringify(error));
      return;
    }
    const options: image.InitializationOptions = {
      editable: false,
      pixelFormat: image.PixelMapFormat.BGRA_8888,
      size: { width: width, height: height }
    };
    try {
      this.remoteClipboardPixelMap = image.createPixelMapSync(bytes.buffer, options);
      this.remoteClipboardText = '';
      this.remoteClipboardHtml = '';
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'create clipboard pixelmap failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Pushes the current cursor state onto this window's system cursor. */
  private applyCursor(): void {
    if (!this.cursorSupported || !this.useRdpCursor) {
      return;
    }
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    try {
      const windowId: number = win.getWindowProperties().id;
      if (windowId < 0) {
        return;
      }
      if (this.cursorHidden) {
        pointer.setPointerVisibleSync(false);
        return;
      }
      pointer.setPointerVisibleSync(true);
      const pixelMap: image.PixelMap | undefined = this.cursorPixelMap;
      if (pixelMap !== undefined) {
        pointer.setCustomCursorSync(windowId, pixelMap, this.cursorHotX, this.cursorHotY);
      } else {
        pointer.setPointerStyleSync(windowId, pointer.PointerStyle.DEFAULT);
      }
    } catch (error) {
      // Capability probing: on a device without a mouse the call throws, so
      // stop trying and leave the system cursor alone.
      this.cursorSupported = false;
      hilog.warn(DOMAIN, TAG, 'set cursor failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Restores the default arrow after the session stops driving the cursor. */
  private resetCursor(): void {
    this.cursorHidden = false;
    this.cursorPixelMap = undefined;
    this.applyCursor();
  }

  /** Restores cursor visibility; setPointerVisible is process-wide. */
  private restoreCursor(): void {
    this.cursorPixelMap = undefined;
    try {
      pointer.setPointerVisibleSync(true);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'restore cursor failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Local clipboard -> remote, invoked by the toolbar's system paste button. */
  private submitClipboard(): void {
    pasteboard.getSystemPasteboard().getData()
      .then((data: pasteboard.PasteData) => {
        this.pushLocalPasteData(data);
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'read pasteboard failed: %{public}s', JSON.stringify(error));
        this.toast('读取本机剪贴板失败');
      });
  }

  /** Dispatches the local clipboard content by MIME, richest type first. */
  private pushLocalPasteData(data: pasteboard.PasteData): void {
    // Do NOT gate on getMimeTypes(): it may only list a record's default MIME,
    // while the HTML lives in a non-default Entry of the same record.
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
          this.native.pushClipboardHtml(html);
          this.toast('已发送富文本到远端');
          return;
        }
        this.pushLocalText(data);
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'read html entry failed: %{public}s', JSON.stringify(error));
        this.pushLocalText(data);
      });
  }

  /** Scans every record for a HTML entry (the record's default MIME may differ). */
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
        // No record / no HTML entry here; try the next one.
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
    this.native.setClipboard(text);
    this.toast('已发送文本到远端');
  }

  /** Local image -> remote. Reads the pixels and lets native build a CF_DIB. */
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
            this.native.pushClipboardImage(width, height, info.pixelFormat, buffer);
            this.toast('已发送图片到远端');
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

  /** Remote clipboard -> local, invoked by the toolbar's pull button. */
  private pullClipboard(): void {
    if (this.pasteboardBusy) {
      return;
    }
    const pixelMap: image.PixelMap | undefined = this.remoteClipboardPixelMap;
    if (pixelMap !== undefined) {
      this.writeLocalPasteData(
        pasteboard.createData(pasteboard.MIMETYPE_PIXELMAP, pixelMap), '图片');
      return;
    }
    if (this.remoteClipboardHtml.length > 0) {
      // Remote HTML is usually a bare fragment; wrap it so the local pasteboard
      // always holds a well-formed document.
      const htmlDoc: string = SessionPage.ensureHtmlDocument(this.remoteClipboardHtml);
      const data: pasteboard.PasteData =
        pasteboard.createData(pasteboard.MIMETYPE_TEXT_HTML, htmlDoc);
      const plain: string = SessionPage.htmlToPlainText(htmlDoc);
      if (plain.length > 0) {
        // Same Record, extra Entry: one content fragment in two formats (the
        // clipboard model requires this instead of two separate Records, or
        // targets treat them as two independent items and pick plain text).
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

  /**
   * Rough HTML -> plain text fallback so a text target still pastes something.
   * Skips <head>/<style>/<script>/comments so CSS text never leaks into the
   * plain-text entry, and turns block tags into line breaks.
   */
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

  /** Wraps a bare HTML fragment into a minimal document when needed. */
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
    try {
      this.getUIContext().getPromptAction().showToast({ message: message, duration: 1500 });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'show toast failed: %{public}s', JSON.stringify(error));
    }
  }

  private updateToolbar(y: number): void {
    // The toolbar only auto-hides in fullscreen; in windowed mode it is always
    // visible and laid out above the remote image.
    if (!this.isFullscreen || this.state !== RdpConnectionState.Connected) {
      return;
    }
    if (y < this.topTrigger) {
      if (this.hideTimer !== -1) {
        clearTimeout(this.hideTimer);
        this.hideTimer = -1;
      }
      if (!this.showToolbar && this.hoverTimer === -1) {
        this.hoverTimer = setTimeout(() => {
          this.hoverTimer = -1;
          this.showToolbar = true;
        }, this.hoverDelay);
      }
    } else if (y >= this.toolbarHeight) {
      if (this.hoverTimer !== -1) {
        clearTimeout(this.hoverTimer);
        this.hoverTimer = -1;
      }
      if (this.showToolbar && this.hideTimer === -1) {
        this.hideTimer = setTimeout(() => {
          this.hideTimer = -1;
          this.showToolbar = false;
        }, this.hideDelay);
      }
    }
  }

  private lengthToNumber(value: Length): number {
    if (typeof value === 'number') {
      return value as number;
    }
    return 0;
  }

  private mapPointer(x: number, y: number): number[] {
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const desktopAspect: number = this.desktopWidth / this.desktopHeight;
    const compAspect: number = compWidth / compHeight;
    let viewX: number = 0;
    let viewY: number = 0;
    let viewWidth: number = compWidth;
    let viewHeight: number = compHeight;
    if (desktopAspect > compAspect) {
      viewHeight = compWidth / desktopAspect;
      viewY = (compHeight - viewHeight) / 2;
    } else {
      viewWidth = compHeight * desktopAspect;
      viewX = (compWidth - viewWidth) / 2;
    }
    const normalizedX: number = (x - viewX) / viewWidth;
    const normalizedY: number = (y - viewY) / viewHeight;
    const clampedX: number = Math.min(1, Math.max(0, normalizedX));
    const clampedY: number = Math.min(1, Math.max(0, normalizedY));
    const remoteX: number = Math.round(clampedX * (this.desktopWidth - 1));
    const remoteY: number = Math.round(clampedY * (this.desktopHeight - 1));
    return [remoteX, remoteY];
  }

  private handleMouse(event: MouseEvent): void {
    // A touchscreen can also emit compatibility mouse events; those are owned by
    // handleTouch. Forward only real mouse / touchpad mouse input here so the
    // remote never sees two input streams for one finger (which shows up as
    // stray clicks during a drag).
    if (event.source === SourceType.TouchScreen) {
      return;
    }
    this.updateToolbar(event.y);
    const point: number[] = this.mapPointer(event.x, event.y);
    const remoteX: number = point[0];
    const remoteY: number = point[1];
    let button: number = 0;
    if (event.button === MouseButton.Left) {
      button = RdpPointerFlags.BUTTON1;
    } else if (event.button === MouseButton.Right) {
      button = RdpPointerFlags.BUTTON2;
    } else if (event.button === MouseButton.Middle) {
      button = RdpPointerFlags.BUTTON3;
    }
    if (event.action === MouseAction.Press) {
      this.native.mouse(RdpPointerFlags.MOVE, remoteX, remoteY);
      this.native.mouse(button | RdpPointerFlags.DOWN, remoteX, remoteY);
    } else if (event.action === MouseAction.Release) {
      this.native.mouse(button, remoteX, remoteY);
    } else if (event.action === MouseAction.Move) {
      this.native.mouse(RdpPointerFlags.MOVE, remoteX, remoteY);
    }
  }

  private handleTouch(event: TouchEvent): void {
    // Only forward real touchscreen input; system-converted mouse touches are
    // handled by handleMouse so mouse stays mouse and touch stays touch.
    if (event.source !== SourceType.TouchScreen) {
      return;
    }
    const changed: TouchObject[] = event.changedTouches;
    for (let i = 0; i < changed.length; i++) {
      const touch: TouchObject = changed[i];
      const point: number[] = this.mapPointer(touch.x, touch.y);
      const type: TouchType = touch.type;
      const pressure: number = this.mapPressure(touch.pressure);
      const pressureFlag: number = pressure > 0 ? RdpTouchFlags.HAS_PRESSURE : 0;
      if (type === TouchType.Down) {
        this.onTouchDown(touch.id, point, pressure, pressureFlag);
      } else if (type === TouchType.Move) {
        this.onTouchMove(touch.id, point, pressure, pressureFlag);
      } else if (type === TouchType.Up) {
        this.onTouchUp(touch.id, point, pressure, pressureFlag);
      } else if (type === TouchType.Cancel) {
        this.onTouchCancel(touch.id, point);
      }
    }
  }

  private onTouchDown(id: number, point: number[], pressure: number, pressureFlag: number): void {
    // A Down for a contact that is still tracked is a retransmission: keep it.
    if (this.touchContactIds.has(id)) {
      return;
    }
    // A held Cancel resumed near its last position means the same finger is
    // still down: keep the remote contact and just move it, instead of lifting
    // and re-pressing (which the remote sees as a click).
    const resumed: number = this.findResumableCancel(point);
    if (resumed >= 0) {
      const rdpId: number | undefined = this.touchContactIds.get(resumed);
      const resumedId: number = rdpId !== undefined ? rdpId : resumed + 1;
      this.clearCancelTimer(resumed);
      this.removePendingCancel(resumed);
      this.touchPoints.delete(resumed);
      this.touchContactIds.delete(resumed);
      this.touchPoints.set(id, point);
      this.touchContactIds.set(id, resumedId);
      this.native.touch(RdpTouchFlags.MOTION | pressureFlag, resumedId, pressure,
        point[0], point[1]);
      return;
    }
    // A genuinely new finger: any held Cancel is now a real release.
    this.finalizeAllCancels();
    const rdpId: number = id + 1;
    this.touchPoints.set(id, point);
    this.touchContactIds.set(id, rdpId);
    this.native.touch(RdpTouchFlags.DOWN | pressureFlag, rdpId, pressure, point[0], point[1]);
  }

  private onTouchMove(id: number, point: number[], pressure: number, pressureFlag: number): void {
    const rdpId: number | undefined = this.touchContactIds.get(id);
    if (rdpId === undefined) {
      return;
    }
    // A Move for a held Cancel means the finger resumed without a fresh Down.
    if (this.isPendingCancel(id)) {
      this.clearCancelTimer(id);
      this.removePendingCancel(id);
    }
    const last: number[] | undefined = this.touchPoints.get(id);
    if (last !== undefined && last[0] === point[0] && last[1] === point[1]) {
      return;
    }
    this.touchPoints.set(id, point);
    this.native.touch(RdpTouchFlags.MOTION | pressureFlag, rdpId, pressure, point[0], point[1]);
  }

  private onTouchUp(id: number, point: number[], pressure: number, pressureFlag: number): void {
    const rdpId: number | undefined = this.touchContactIds.get(id);
    if (rdpId === undefined) {
      return;
    }
    this.clearCancelTimer(id);
    this.removePendingCancel(id);
    this.touchPoints.delete(id);
    this.touchContactIds.delete(id);
    this.native.touch(RdpTouchFlags.UP | pressureFlag, rdpId, pressure, point[0], point[1]);
  }

  private onTouchCancel(id: number, point: number[]): void {
    if (!this.touchContactIds.has(id) || this.isPendingCancel(id)) {
      return;
    }
    // Hold the contact: the finger is often still down. CANCEL_HOLD_MS is only
    // a safety release for a Cancel that never resumes.
    this.touchPoints.set(id, point);
    this.pendingCancelIds.push(id);
    const timer: number = setTimeout(() => {
      this.cancelTimers.delete(id);
      this.finalizeCancel(id);
    }, CANCEL_HOLD_MS);
    this.cancelTimers.set(id, timer);
  }

  /** Nearest held Cancel within the resume distance, or -1. */
  private findResumableCancel(point: number[]): number {
    const minDim: number = Math.min(this.desktopWidth, this.desktopHeight);
    const limit: number = Math.max(CANCEL_RESUME_MIN_DISTANCE,
      Math.min(CANCEL_RESUME_MAX_DISTANCE, Math.round(minDim * CANCEL_RESUME_RATIO)));
    const limitSq: number = limit * limit;
    let best: number = -1;
    let bestDist: number = limitSq;
    for (let i = 0; i < this.pendingCancelIds.length; i++) {
      const id: number = this.pendingCancelIds[i];
      const last: number[] | undefined = this.touchPoints.get(id);
      if (last === undefined) {
        continue;
      }
      const dx: number = last[0] - point[0];
      const dy: number = last[1] - point[1];
      const dist: number = dx * dx + dy * dy;
      if (dist <= bestDist) {
        bestDist = dist;
        best = id;
      }
    }
    return best;
  }

  private isPendingCancel(id: number): boolean {
    return this.pendingCancelIds.indexOf(id) >= 0;
  }

  private removePendingCancel(id: number): void {
    const index: number = this.pendingCancelIds.indexOf(id);
    if (index >= 0) {
      this.pendingCancelIds.splice(index, 1);
    }
  }

  private clearCancelTimer(id: number): void {
    const timer: number | undefined = this.cancelTimers.get(id);
    if (timer !== undefined) {
      clearTimeout(timer);
      this.cancelTimers.delete(id);
    }
  }

  private finalizeCancel(id: number): void {
    this.clearCancelTimer(id);
    this.removePendingCancel(id);
    const rdpId: number | undefined = this.touchContactIds.get(id);
    const point: number[] | undefined = this.touchPoints.get(id);
    this.touchPoints.delete(id);
    this.touchContactIds.delete(id);
    if (rdpId !== undefined && point !== undefined) {
      this.native.touch(RdpTouchFlags.UP, rdpId, 0, point[0], point[1]);
    }
  }

  private finalizeAllCancels(): void {
    // Copy first: finalizeCancel mutates the list.
    const ids: number[] = this.pendingCancelIds.slice();
    for (let i = 0; i < ids.length; i++) {
      this.finalizeCancel(ids[i]);
    }
  }

  /**
   * ArkUI reports contact pressure in [0, 65535); RDPEI expects [0, 1024].
   * Zero means the device did not report pressure, so it is not forwarded.
   */
  private mapPressure(pressure: number | undefined): number {
    if (pressure === undefined || pressure <= 0) {
      return 0;
    }
    return Math.max(1, Math.min(1024, Math.round(pressure / 65535 * 1024)));
  }

  private releaseAllTouches(): void {
    this.finalizeAllCancels();
    this.touchContactIds.forEach((rdpId: number, id: number) => {
      const point: number[] | undefined = this.touchPoints.get(id);
      if (point !== undefined) {
        this.native.touch(RdpTouchFlags.UP, rdpId, 0, point[0], point[1]);
      }
    });
    this.touchPoints.clear();
    this.touchContactIds.clear();
  }

  private handleAxis(event: AxisEvent): void {
    const isPinch: boolean = this.isPinchEvent(event);
    if (isPinch || this.pinchActive) {
      // The gesture ends only on the real AxisAction.END (fingers lifted).
      if (event.action === AxisAction.END) {
        this.endPinch();
        return;
      }
      if (isPinch) {
        this.updatePinch(event);
      }
      // Scroll axes produced by the same two fingers are suppressed while the
      // pinch is active, so zoom never mixes with scroll.
      return;
    }
    if (event.action !== AxisAction.BEGIN && event.action !== AxisAction.UPDATE) {
      return;
    }
    if (event.action === AxisAction.BEGIN) {
      this.touchpadWheel.reset();
    }
    const vertical: number = event.axisVertical ?? 0;
    const horizontal: number = event.axisHorizontal ?? 0;
    if (vertical === 0 && horizontal === 0) {
      return;
    }
    const point: number[] = this.mapPointer(event.x, event.y);
    if (event.sourceTool === SourceTool.TOUCHPAD) {
      this.handleTouchpadScroll(vertical, horizontal, point);
    } else if (vertical !== 0) {
      // Mouse wheel: forward/up is negative on the vertical axis.
      this.sendWheel(vertical < 0, point);
    }
  }

  private isPinchEvent(event: AxisEvent): boolean {
    if ((event.axisPinch ?? 0) > 0) {
      return true;
    }
    try {
      return event.hasAxis(AxisType.PINCH_AXIS);
    } catch (error) {
      return false;
    }
  }

  private pinchScaleValue(event: AxisEvent): number {
    const direct: number = event.axisPinch ?? 0;
    if (direct > 0) {
      return direct;
    }
    try {
      return event.getPinchAxisScaleValue();
    } catch (error) {
      return 0;
    }
  }

  private updatePinch(event: AxisEvent): void {
    const scale: number = this.pinchScaleValue(event);
    // Enter pinch mode the moment a pinch is detected (no threshold/gating).
    if (!this.pinchActive) {
      this.pinchActive = true;
      this.pinchAccum = 0;
      if (this.pinchAsTouch) {
        this.beginTouchPinch(event, scale);
      } else {
        // Ctrl goes down immediately and the current distance is the baseline.
        this.pinchScale = scale > 0 ? scale : 0;
        this.setPinchCtrl(true);
      }
      return;
    }
    if (scale <= 0) {
      return;
    }
    if (this.pinchAsTouch) {
      this.updateTouchPinch(scale);
      // Skip the MOTIONs that would otherwise let FreeRDP's 50Hz RDPEI
      // coalescing overwrite the unsent DOWN; the distance is still tracked
      // above, so the first forwarded MOTION carries the current gesture.
      if (Date.now() >= this.touchPinchMotionAt) {
        this.sendTouchPinchContacts(RdpTouchFlags.MOTION);
      }
      return;
    }
    if (this.pinchScale <= 0) {
      // The first event carried no usable distance; establish the baseline now.
      this.pinchScale = scale;
      return;
    }
    // Linear distance mapping. axisPinch is the ratio of the current distance to
    // the initial one, so (scale - previous) is directly proportional to the
    // distance the fingers moved. The step is derived from the configured zoom
    // speed: one notch per 1 / (baseNotchesPerUnit * speed) of that ratio, so a
    // larger speed emits more notches for the same pinch (faster zoom).
    this.pinchAccum += scale - this.pinchScale;
    this.pinchScale = scale;
    const notchesPerUnit: number = DEFAULT_PINCH_NOTCHES_PER_UNIT * this.pinchZoomSpeed;
    const step: number = notchesPerUnit > 0 ? 1 / notchesPerUnit : 0;
    if (step <= 0) {
      return;
    }
    const point: number[] = this.mapPointer(event.x, event.y);
    while (this.pinchAccum >= step) {
      this.pinchAccum -= step;
      this.sendWheel(true, point);
    }
    while (this.pinchAccum <= -step) {
      this.pinchAccum += step;
      this.sendWheel(false, point);
    }
  }

  /**
   * Touch-pinch mode: place two virtual contacts symmetrically around the
   * pointer and move them apart/together with the gesture ratio, so the remote
   * sees a real two-finger pinch at the cursor (better in touch-aware apps).
   */
  private beginTouchPinch(event: AxisEvent, scale: number): void {
    const minDim: number = Math.min(this.desktopWidth, this.desktopHeight);
    const percent: number = Math.max(MIN_PINCH_DISTANCE_PERCENT,
      Math.min(MAX_PINCH_DISTANCE_PERCENT, this.pinchTouchDistance));
    this.touchPinchAnchor = this.mapPointer(event.x, event.y);
    this.touchPinchHalf = Math.max(MIN_PINCH_HALF, Math.round(minDim * percent / 100));
    this.touchPinchLastHalf = this.touchPinchHalf;
    // The pair is tilted by the configured angle (positive = clockwise on
    // screen). ArkUI reports no finger angle, so this stands in for the hand.
    const angle: number = Math.max(-180, Math.min(180, this.touchPinchAngle));
    const radians: number = angle * Math.PI / 180;
    this.touchPinchUnitX = Math.cos(radians);
    this.touchPinchUnitY = Math.sin(radians);
    this.touchPinchActive = true;
    // axisPinch is already relative to the gesture start (1.0); apply the
    // initial sample so a late first event does not drop the first movement.
    this.updateTouchPinch(scale);
    this.sendTouchPinchContacts(RdpTouchFlags.DOWN);
    // Let the DOWN frame flush before the first MOTION can overwrite it.
    this.touchPinchMotionAt = Date.now() + PINCH_TOUCH_DOWN_FLUSH_MS;
  }

  /**
   * Maps the gesture distance ratio straight onto the synthetic finger
   * separation (a true 1:1 pinch; the zoom speed setting does not apply here),
   * so the remote scale matches the gesture exactly.
   */
  private updateTouchPinch(scale: number): void {
    const ratio: number = Math.max(MIN_PINCH_RATIO, Math.min(MAX_PINCH_RATIO, scale));
    this.touchPinchLastHalf =
      Math.max(MIN_PINCH_HALF, Math.round(this.touchPinchHalf * ratio));
  }

  private sendTouchPinchContacts(flags: number): void {
    const point: number[] = this.touchPinchAnchor;
    const half: number = this.touchPinchLastHalf;
    const dx: number = half * this.touchPinchUnitX;
    const dy: number = half * this.touchPinchUnitY;
    // Keep both contacts inside the remote desktop.
    const maxX: number = Math.max(0, this.desktopWidth - 1);
    const maxY: number = Math.max(0, this.desktopHeight - 1);
    const x1: number = Math.max(0, Math.min(maxX, Math.round(point[0] + dx)));
    const y1: number = Math.max(0, Math.min(maxY, Math.round(point[1] + dy)));
    const x2: number = Math.max(0, Math.min(maxX, Math.round(point[0] - dx)));
    const y2: number = Math.max(0, Math.min(maxY, Math.round(point[1] - dy)));
    this.native.touch(flags, PINCH_TOUCH_ID_A, 0, x1, y1);
    this.native.touch(flags, PINCH_TOUCH_ID_B, 0, x2, y2);
  }

  private endTouchPinch(): void {
    if (!this.touchPinchActive) {
      return;
    }
    this.touchPinchActive = false;
    this.sendTouchPinchContacts(RdpTouchFlags.UP);
  }

  private setPinchCtrl(down: boolean): void {
    if (this.pinchCtrlDown === down) {
      return;
    }
    this.pinchCtrlDown = down;
    this.native.key(this.ctrlScancode, down, false);
  }

  private endPinch(): void {
    this.endTouchPinch();
    this.pinchActive = false;
    this.pinchScale = 0;
    this.pinchAccum = 0;
    this.touchPinchMotionAt = 0;
    // Release unconditionally: the final event may not carry the pinch axis, so
    // pinchActive can already be false while Ctrl is still logically held.
    this.setPinchCtrl(false);
  }

  private handleTouchpadScroll(vertical: number, horizontal: number, point: number[]): void {
    // The mapper turns the raw displacement into one or more high-resolution
    // RDP wheel chunks (9-bit wheel field, 120 == one detent).
    this.wheelChunks.length = 0;
    this.touchpadWheel.map(vertical, horizontal, this.wheelChunks);
    for (let i = 0; i < this.wheelChunks.length; i++) {
      this.sendWheelChunk(this.wheelChunks[i], point);
    }
  }

  private sendWheelChunk(chunk: WheelChunk, point: number[]): void {
    // Negative rotation is 9-bit two's complement with the sign bit OR'd in:
    // magnitude m -> WHEEL_NEGATIVE | (0x100 - m). A detent of 120 encodes to
    // 0x88, identical to what the RDP/FreeRDP clients emit.
    let flags: number = chunk.horizontal ? RdpPointerFlags.HWHEEL : RdpPointerFlags.WHEEL;
    flags |= chunk.negative
      ? RdpPointerFlags.WHEEL_NEGATIVE | (0x100 - chunk.rotation)
      : chunk.rotation;
    this.native.mouse(flags, point[0], point[1]);
  }

  /** A real mouse wheel already reports notches, so forward one full detent. */
  private sendWheel(up: boolean, point: number[]): void {
    const chunk: WheelChunk = new WheelChunk();
    chunk.horizontal = false;
    chunk.negative = !up;
    chunk.rotation = WHEEL_DETENT;
    this.sendWheelChunk(chunk, point);
  }

  private handleKey(event: KeyEvent): void {
    const down: boolean = event.type === KeyType.Down;
    // The real Win key is owned by the system, so it is never forwarded;
    // Win for the remote is produced by the configurable substitute key.
    if (event.keyCode === 2076 || event.keyCode === 2077) {
      return;
    }
    if (this.winKeySubstitute !== 0 && event.keyCode === this.winKeySubstitute) {
      this.native.winKey(down);
      return;
    }
    const scan: RdpScanCode | undefined = KeyMapper.map(event.keyCode);
    if (scan !== undefined) {
      this.native.key(scan.scancode, down, scan.extended);
      return;
    }
    const text: string = event.keyText;
    if (text.length === 1) {
      const code: number = text.charCodeAt(0);
      if (code >= 0x20) {
        this.native.unicode(code, down);
      }
    }
  }

  private loadSessionWindow(): void {
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    window.getLastWindow(context).then((win: window.Window) => {
      this.sessionWindow = win;
      this.isFullscreen = win.getWindowStatus() === window.WindowStatusType.FULL_SCREEN;
      win.on('windowStatusChange', (status: window.WindowStatusType) => {
        this.isFullscreen = status === window.WindowStatusType.FULL_SCREEN;
      });
    }).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'getLastWindow failed: %{public}s', JSON.stringify(error));
    });
  }

  private minimizeWindow(): void {
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    win.minimize().catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'minimize failed: %{public}s', JSON.stringify(error));
    });
  }

  private toggleFullscreen(): void {
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    if (this.isFullscreen) {
      win.recover().catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'recover failed: %{public}s', JSON.stringify(error));
      });
      return;
    }
    win.maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER)
      .catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'maximize failed: %{public}s', JSON.stringify(error));
      });
  }

  private disconnect(): void {
    if (this.connectionId.length > 0) {
      // The manager stops the session and invokes our close callback, which
      // terminates this window; the list row goes back to idle.
      SessionManager.disconnect(this.connectionId);
      return;
    }
    this.native.stopSession();
    this.native.clearListener();
    this.terminateWindow();
  }

  private terminateWindow(): void {
    if (this.closing) {
      return;
    }
    this.closing = true;
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      return;
    }
    const abilityContext: common.UIAbilityContext = context as common.UIAbilityContext;
    // Single-window mode: this session hid the main window, so the controller
    // relaunches the main window first and terminates this one afterwards.
    if (WindowController.closeSession(abilityContext)) {
      return;
    }
    abilityContext.terminateSelf().catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'terminate session failed: %{public}s', JSON.stringify(error));
    });
  }

  @Builder
  statusCard(title: string, subtitle: string, primary: string, secondary: string) {
    Column() {
      if (this.state === RdpConnectionState.Connecting) {
        LoadingProgress().width(44).height(44).color($r('app.color.accent'))
      } else {
        Image($r('app.media.app_icon')).width(52).height(52).borderRadius(14)
      }
      Text(title)
        .fontSize(17)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .margin({ top: 18 })
      Text(subtitle)
        .fontSize(13)
        .fontColor($r('app.color.text_secondary'))
        .margin({ top: 8 })
        .textAlign(TextAlign.Center)
        .constraintSize({ maxWidth: 320 })
      Row() {
        Button(primary)
          .height(42)
          .fontSize(14)
          .backgroundColor($r('app.color.accent'))
          .borderRadius(10)
          .onClick(() => {
            if (this.state === RdpConnectionState.Connecting) {
              this.disconnect();
            } else {
              this.retry();
            }
          })
        Button(secondary)
          .height(42)
          .fontSize(14)
          .fontColor($r('app.color.text_primary'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(10)
          .margin({ left: 12 })
          .onClick(() => {
            this.disconnect();
          })
      }
      .margin({ top: 24 })
    }
    .width(360)
    .padding({ top: 32, bottom: 28, left: 28, right: 28 })
    .backgroundColor($r('app.color.card_bg'))
    .borderRadius(20)
    .shadow({ radius: 30, color: '#33000000', offsetX: 0, offsetY: 10 })
    .alignItems(HorizontalAlign.Center)
  }

  @Builder
  sessionToolbar() {
    Row() {
      Text(this.options.host)
        .fontSize(14)
        .fontColor('#FFFFFF')
      Text('  ' + this.desktopWidth + ' × ' + this.desktopHeight)
        .fontSize(12)
        .fontColor('#B8BCC4')
      if (this.state === RdpConnectionState.Connected) {
        Row() {
          Text(this.rttLabel())
            .fontSize(12)
            .fontColor(this.thresholdColor(this.metricsRtt, NETWORK_WARN_MS, NETWORK_BAD_MS))
            .width(METRIC_WIDTH_NETWORK)
            .maxLines(1)
          Text(this.localLabel())
            .fontSize(12)
            .fontColor(this.thresholdColor(this.localMs(), LOCAL_WARN_MS, LOCAL_BAD_MS))
            .width(METRIC_WIDTH_LOCAL)
            .margin({ left: METRIC_GAP_TIGHT })
            .maxLines(1)
            .bindTips(this.localTips())
          Text(this.fpsLabel())
            .fontSize(12)
            .fontColor('#B8BCC4')
            .width(METRIC_WIDTH_FPS)
            .margin({ left: METRIC_GAP_GROUP })
            .maxLines(1)
          if (this.hasBandwidth()) {
            Text(this.bandwidthLabel())
              .fontSize(12)
              .fontColor('#B8BCC4')
              .margin({ left: METRIC_GAP_GROUP })
          }
          if (this.metricsAudioRateHz > 0) {
            Text(this.audioLabel())
              .fontSize(12)
              .fontColor(this.audioColor())
              .width(METRIC_WIDTH_AUDIO)
              .margin({ left: METRIC_GAP_GROUP })
              .maxLines(1)
          }
        }
        .margin({ left: 16 })
      }
      Blank()
      if (this.clipboardEnabled) {
        // Clipboard actions sit left of the window actions, tinted
        // differently and separated from them by a fixed gap.
        Row() {
          Image($r('sys.media.ohos_ic_public_copy'))
            .width(16)
            .height(16)
            .fillColor('#FFFFFF')
          Text('复制')
            .fontSize(13)
            .fontColor('#FFFFFF')
            .margin({ left: 4 })
        }
        .padding({ left: 14, right: 14, top: 7, bottom: 7 })
        .backgroundColor('#4D3B82F6')
        .borderRadius(8)
        .hoverEffect(HoverEffect.Highlight)
        .onClick(() => {
          this.pullClipboard();
        })
        .bindTips('从远端复制到本机剪贴板')
        // Local clipboard -> remote. Security control: clicking it grants a
        // one-time clipboard read without the READ_PASTEBOARD permission.
        Row() {
          PasteButton({
            icon: PasteIconStyle.LINES,
            text: PasteDescription.PASTE,
            buttonType: ButtonType.Normal
          })
            .iconSize(16)
            .iconColor('#FFFFFF')
            .fontSize(13)
            .fontColor('#FFFFFF')
            .backgroundColor('#4D3B82F6')
            .borderRadius(8)
            .padding({ left: 14, right: 14, top: 7, bottom: 7 })
            .onClick(this.pasteButtonCallback)
        }
        .margin({ left: 8 })
        .bindTips('把本机剪贴板发送到远端')
        Row().width(16)
      }
      Text(this.isFullscreen ? '退出全屏' : '全屏')
        .fontSize(13)
        .fontColor('#FFFFFF')
        .padding({ left: 16, right: 16, top: 7, bottom: 7 })
        .backgroundColor('#33FFFFFF')
        .borderRadius(8)
        .margin({ right: 8 })
        .hoverEffect(HoverEffect.Highlight)
        .onClick(() => {
          this.toggleFullscreen();
        })
      Text('最小化')
        .fontSize(13)
        .fontColor('#FFFFFF')
        .padding({ left: 16, right: 16, top: 7, bottom: 7 })
        .backgroundColor('#33FFFFFF')
        .borderRadius(8)
        .margin({ right: 8 })
        .hoverEffect(HoverEffect.Highlight)
        .onClick(() => {
          this.minimizeWindow();
        })
      Text('断开')
        .fontSize(13)
        .fontColor('#FFFFFF')
        .padding({ left: 16, right: 16, top: 7, bottom: 7 })
        .backgroundColor($r('app.color.danger'))
        .borderRadius(8)
        .hoverEffect(HoverEffect.Highlight)
        .onClick(() => {
          this.disconnect();
        })
    }
    .width('100%')
    .height(48)
    .padding({ left: 16, right: 16 })
    .backgroundColor('#CC1A1A1A')
    .transition(TransitionEffect.translate({ y: -48 })
      .animation({ duration: 180, curve: Curve.EaseOut }))
  }

  build() {
    Column() {
      // Windowed mode: the toolbar is always visible and laid out above the
      // remote image, so the picture starts below it instead of under it.
      if (!this.isFullscreen && !this.showOverlay) {
        this.sessionToolbar()
      }
      Stack({ alignContent: Alignment.TopStart }) {
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
          // The system may drop a custom cursor when the pointer leaves and
          // re-enters the window, so re-install the last known shape.
          if (isHover) {
            this.applyCursor();
          }
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
          this.componentWidth = this.lengthToNumber(newArea.width);
          this.componentHeight = this.lengthToNumber(newArea.height);
        })

        // Fullscreen mode: the toolbar floats over the image and auto-hides.
        if (this.isFullscreen && this.showToolbar && !this.showOverlay) {
          this.sessionToolbar()
        }

      if (this.showOverlay) {
        Column() {
          if (this.state === RdpConnectionState.Connecting) {
            this.statusCard('正在连接', this.options.host + ':' + this.options.port, '取消', '')
          } else if (this.state === RdpConnectionState.Failed) {
            this.statusCard('连接失败', this.statusText, '重试', '返回')
          } else if (this.state === RdpConnectionState.Closed) {
            this.statusCard('连接已断开', this.statusText, '重试', '返回')
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
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
