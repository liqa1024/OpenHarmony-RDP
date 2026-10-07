/*
 * HmRdp - remote session page: XComponent surface + input forwarding.
 */
import { common, ApplicationStateChangeCallback } from '@kit.AbilityKit';
import { image } from '@kit.ImageKit';
import { inputDevice, pointer } from '@kit.InputKit';
import { matrix4, window } from '@kit.ArkUI';
import { BusinessError, pasteboard, SuppressWarnings, SuppressWarningsType } from '@kit.BasicServicesKit';
import { fileIo, fileUri } from '@kit.CoreFileKit';
import { hidebug, hilog } from '@kit.PerformanceAnalysisKit';
import { RdpEventPayload } from 'libhmrdp.so';
import { PendingConnection, RdpConnectOptions,
  RdpClipKind, RdpEvent, RdpLockFlags, RdpPointerFlags, RdpTouchFlags, SessionRequests,
  SR_BACKEND_FSR } from '../model/RdpModels';
import { BackgroundEvent, BackgroundEvents } from '../services/BackgroundProtocol';
import { AudioPlayer } from '../services/AudioPlayer';
import { BackgroundWorker } from '../services/BackgroundWorker';
import { RdpNative } from '../services/RdpNative';
import { RdpSessionBackend, SessionBackend, SessionBackendRegistry } from '../services/SessionBackend';
import { SessionSignal } from '../model/SessionSignal';
import { RdpCounters } from '../services/SessionTelemetry';
import { DeviceCapabilities } from '../services/DeviceCapabilities';
import { ConnectionState, SessionManager } from '../services/SessionManager';
import { AppSettings, DEFAULT_PINCH_TOUCH_DISTANCE, PINCH_MODE_TOUCH, PINCH_MODE_ZOOM,
  SettingsStore, srRatioLabel } from '../services/SettingsStore';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { TouchpadWheelMapper, WheelChunk, WHEEL_DETENT, DEFAULT_PINCH_NOTCHES_PER_UNIT,
  MIN_SPEED_SCALE, MAX_SPEED_SCALE } from '../services/TouchpadWheel';
import { WindowController } from '../services/WindowController';
import { KeyMapper, RdpScanCode } from '../utils/KeyMapper';
import { DibImage } from '../utils/ClipboardCodec';
import { FileDescriptorInput, FileTransferCodec } from '../utils/FileTransferCodec';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSession';

// Synthetic touch pinch geometry. The initial finger separation is a percentage
// of the smaller desktop side (window-relative, never a fixed resolution); the
// value itself is settings.pinchTouchDistance, so only the clamp is local.
const MIN_PINCH_DISTANCE_PERCENT: number = 1;
const MAX_PINCH_DISTANCE_PERCENT: number = 20;
const MIN_PINCH_HALF: number = 8;
// axisPinch is a distance ratio relative to the gesture start; clamp it so a
// bad sample can never explode or collapse the synthetic finger separation.
const MIN_PINCH_RATIO: number = 0.05;
const MAX_PINCH_RATIO: number = 20;
// Ceiling of the direct (local) picture zoom; 1.0 is the fitted view.
const MIN_VIEW_SCALE: number = 1;
const MAX_VIEW_SCALE: number = 8;
// A pinch released below this scale snaps back to exactly 100% (VIEW_SNAP_MS).
const VIEW_SNAP_SCALE: number = 1.08;
const VIEW_SNAP_MS: number = 180;
// Telemetry pull cadence; one sample per second, matching the native window.
const METRICS_INTERVAL_MS: number = 1000;
// Pan per mouse wheel notch (vp); the touchpad scroll pans 1:1 instead.
const PAN_WHEEL_STEP: number = 64;
// External RDPEI ids for the synthetic pinch contacts; real screen touches use
// touch.id + 1 (1..11), so these never collide.
const PINCH_TOUCH_ID_A: number = 20;
const PINCH_TOUCH_ID_B: number = 21;
// The synthetic pinch's first MOTION must not reach the native RDPEI layer
// before its DOWN frame has been flushed: FreeRDP coalesces touch frames at
// 50Hz by default (HmrdpSetTouchFrameInterval(20)), so a MOTION inside that
// window overwrites the unsent DOWN and the remote never engages the contacts.
// This margin decouples the app from the coalescing interval (a touch pinch uses
// the high-rate option, whose interval is below this margin) and is far below the
// time Windows needs to recognise a pinch.
const PINCH_TOUCH_DOWN_FLUSH_MS: number = 30;
// Direction dead zone for the pinch locator cursor: the gesture ratio must move
// this far past 1.0 before the open/grab hand flips, so a jitter around the start
// does not make the cursor flicker between the two.
const PINCH_CURSOR_HYSTERESIS: number = 0.02;

// Toolbar throughput below this is idle/keep-alive noise, so it is not shown.
const MIN_BANDWIDTH_BYTES: number = 1024;

// Fixed widths for the toolbar telemetry so a changing number never reflows the
// items after it. Sized close to the common maximum so the unused space inside
// a box stays small and the gaps are governed by the margins below.
const METRIC_WIDTH_NETWORK: number = 66;
const METRIC_WIDTH_LOCAL: number = 74;
const METRIC_WIDTH_FPS: number = 48;
const METRIC_WIDTH_AUDIO: number = 86;
// Process CPU usage (%) and this client's own GPU time per frame (ms).
const METRIC_WIDTH_CPU: number = 60;
const METRIC_WIDTH_GPU: number = 76;
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

// The toolbar floats over the remote desktop image, whose colours are unknown,
// so its palette is deliberately *fixed dark* instead of following the app
// theme: light chrome would be unreadable over an arbitrary remote screen, and
// the bar reads as one consistent piece of chrome in both system light and dark
// mode. The material behind it is pinned dark to match (see SystemMaterial).
const TOOLBAR_BG: string = '#CC1A1A1A';
const TOOLBAR_FG: string = '#FFFFFF';
const TOOLBAR_FG_SECONDARY: string = '#B8BCC4';
const TOOLBAR_CHIP_BG: string = '#33FFFFFF';
const TOOLBAR_ACCENT_CHIP_BG: string = '#4D3B82F6';
// Toolbar geometry: the main row is always there; developer mode appends the
// telemetry line, so the auto-hide boundary has to grow with it.
const TOOLBAR_ROW_HEIGHT: number = 48;
const TOOLBAR_TELEMETRY_HEIGHT: number = 24;
// Outline for secondary (a step below the filled chips) toolbar actions.
const TOOLBAR_CHIP_BORDER: string = '#59FFFFFF';
// File-transfer progress fill: brighter than the chip tints so the bar reads at
// a glance on the dark toolbar.
const TOOLBAR_PROGRESS: string = '#5B9CF8';
// Pointer zones for the fullscreen auto-hide (SessionPage.setToolbarPointerZone).
const TOOLBAR_ZONE_NONE: number = 0;
const TOOLBAR_ZONE_TRIGGER: number = 1;
const TOOLBAR_ZONE_BAR: number = 2;
// Reveal hint: a hairline at the top edge whose fill grows over the whole dwell,
// so a hidden strip is announced before it drops in.
const REVEAL_HINT_WIDTH: number = 96;
const REVEAL_HINT_HEIGHT: number = 3;
const REVEAL_HINT_FADE_MS: number = 140;
// Telemetry threshold colours, tuned against the fixed dark bar.
const METRIC_GOOD: string = '#4ADE80';
const METRIC_WARN: string = '#FBBF24';
const METRIC_BAD: string = '#F87171';

// Grace period before the "came back to the foreground" reconnect check. While
// the app is away the system freezes and then aborts its sockets, but the native
// read loop only notices that once it runs again, i.e. just after the window is
// back. One delayed check therefore covers both orders: a session already
// reported down while we were away is still down by the time it runs.
const FOREGROUND_RECONNECT_GRACE_MS: number = 400;

// A TouchType.Cancel does not always mean the finger lifted (a move can be
// interrupted). The contact is held instead, and a following Down within this
// distance of its last position is treated as the same finger resuming. Only if
// no such event arrives within CANCEL_HOLD_MS is the contact really released.
const CANCEL_RESUME_RATIO: number = 0.02;
const CANCEL_RESUME_MIN_DISTANCE: number = 16;
const CANCEL_RESUME_MAX_DISTANCE: number = 64;
const CANCEL_HOLD_MS: number = 400;

// Upper bound on how often a pointer move is forwarded (125Hz). It is the pacing
// of the input the remote receives, not a display rate: mouse and touchpad events
// arrive far faster than any pointer needs to move, and each forwarded move is a
// separate input PDU.
const MOUSE_MOVE_MIN_INTERVAL_MS: number = 8;

// 触控板双指双击 -> 中键的判定窗口：单次双指轻触要等这么久没有第二次才按右键发出。
const TOUCHPAD_DOUBLE_TAP_MS: number = 300;



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

/**
 * Routes the process-level foreground transitions to the page. Only an actual
 * round trip matters: the reconnect decision is made on the way back, never on
 * the way out.
 */
class ForegroundStateCallback extends ApplicationStateChangeCallback {
  private onAway: () => void;
  private onReturn: () => void;

  constructor(onAway: () => void, onReturn: () => void) {
    super();
    this.onAway = onAway;
    this.onReturn = onReturn;
  }

  onApplicationForeground(): void {
    this.onReturn();
  }

  onApplicationBackground(): void {
    this.onAway();
  }
}

/** One clipboard candidate (kind + the server's format id) the page may pull. */
class ClipPick {
  kind: number = RdpClipKind.NONE;
  id: number = 0;
}

@Entry({ useSharedStorage: true })
@Component
struct SessionPage {
  // The manager is the authority; this only mirrors what it publishes.
  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接...';
  @State showOverlay: boolean = true;
  @State showToolbar: boolean = true;
  // Fill of the top-edge reveal hint (0..1): shows the dwell, never decides it.
  @State revealHintFill: number = 0;
  @State isFullscreen: boolean = false;
  // Identifies this window's launch request, injected by SessionAbility.
  @LocalStorageLink('sessionKey') sessionKey: string = '';

  private native: RdpNative = new RdpNative();
  private options: RdpConnectOptions = new RdpConnectOptions();
  private connectionId: string = '';
  // Immersive system material for the toolbar and the connection status card.
  // Both are no-ops on a system without the material capability, in which case
  // the flat background below is what shows.
  private toolbarMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.FLOATING));
  private statusCardMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.CARD));
  // Tooltip surfaces take the material through their options object; a system
  // without the capability ignores the field and keeps the default tooltip.
  private tipsOptions: TipsOptions =
    { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };
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
  // Per-frame work phase by phase, in microseconds: ZGX decompress + RDPGFX parse,
  // decode, gdi compose, present. The toolbar's "本机" is their sum (see localUs()):
  // the native side sends no pre-summed total, and blocked time (the GPU `sync`)
  // is a separate field that is deliberately not summed in.
  @State metricsZgxUs: number = 0;
  @State metricsDecodeUs: number = 0;
  @State metricsComposeUs: number = 0;
  @State metricsPresentUs: number = 0;
  // Of `presentUs`, the part the presenter spent blocked on the display
  // (swapchain acquire / buffer swap). Blocked, so it is not in the 本机 sum; it
  // is only shown as a hint in the tooltip.
  @State metricsPresentWaitUs: number = 0;
  @State metricsBytesPerFrame: number = 0;
  @State metricsDutyPermille: number = 0;
  @State metricsCmdsPerFrame: number = 0;
  // GPU time of the presented frame in microseconds, split into the dirty-rect
  // upload, the 超分辨率 upscale and the clear + letterbox quad. Zero on a backend
  // that cannot measure it (the GLES fallback). This is the client's *own* GPU work,
  // not the GPU's total load - no public API exposes that to an app.
  @State metricsGpuCopyUs: number = 0;
  @State metricsGpuSrUs: number = 0;
  @State metricsGpuBlitUs: number = 0;
  // Process CPU usage in percent, or -1 when the device cannot report it
  // (hidebug). Sampled once per telemetry window - see sampleCpuUsage().
  @State metricsCpuPercent: number = -1;
  // Developer options (SettingsStore.developerMode): the toolbar's performance
  // telemetry is a dev tool, so it is only shown while this is on.
  @State developerMode: boolean = false;
  // Audio sample rate (Hz) and recent glitch rate in basis points (0.01%);
  // -1 when audio is not playing.
  @State metricsAudioRateHz: number = -1;
  @State metricsAudioLossBp: number = -1;
  private surfaceId: string = '';
  private surfaceWidth: number = 0;
  private surfaceHeight: number = 0;
  // windowVisible = windowOnScreen && !windowMinimized (see
  // doc_agent/session-and-input.md §1.1). While it is false, surface sizes the
  // system reports are held until it is back, and the server's output is suppressed.
  private windowOnScreen: boolean = true;
  private windowMinimized: boolean = false;
  private windowVisible: boolean = true;
  private pendingSurfaceResize: boolean = false;
  // Fullscreen auto-hide: the pointer zone, the reveal dwell and the hide delay
  // (see refreshToolbarAutoHide). -1 = timer not running.
  private pointerZone: number = TOOLBAR_ZONE_NONE;
  private revealTimer: number = -1;
  private hideTimer: number = -1;
  // Pulls the native telemetry window once per second while the session is
  // connected (see refreshCounters). -1 = timer not running.
  private metricsTimer: number = -1;
  // Cleared in aboutToDisappear so a hover-exit during teardown arms nothing.
  private toolbarAutoHideArmed: boolean = true;
  private moveTimer: number = -1;
  // The strip is `toolbarHeight` tall; the band that reveals it is much shorter,
  // so a pass-by along the top edge does not.
  private revealBand: number = 6;
  private toolbarHeight: number = TOOLBAR_ROW_HEIGHT;
  private hoverDelay: number = 350;
  private hideDelay: number = 250;
  // Active touch contacts, keyed by the ArkUI touch id. The value is the last
  // remote point sent for that contact, so no-op moves can be dropped.
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
  // Latest pointer position waiting to be sent, and when a MOVE was last sent
  // (see MOUSE_MOVE_MIN_INTERVAL_MS). -1 means nothing is pending.
  private pendingMoveX: number = -1;
  private pendingMoveY: number = -1;
  private lastMoveSentAt: number = 0;
  // Input the remote still sees as held, so a lost Up (focus left the window)
  // can be flushed. Keys are keyed by remote identity, not by ArkUI key code.
  private heldScanKeys: Map<string, RdpScanCode> = new Map<string, RdpScanCode>();
  private heldUnicodeKeys: Set<number> = new Set<number>();
  private heldButtons: Set<number> = new Set<number>();
  private lastPointerX: number = 0;
  private lastPointerY: number = 0;
  // 触控板中键手势: a touchpad two-finger tap is a right click, but it may be the
  // first half of a double tap that means middle, so while the setting is on the
  // single click is held back for TOUCHPAD_DOUBLE_TAP_MS.
  private touchpadMiddleDoubleTap: boolean = false;
  private touchpadClickPending: boolean = false;
  private touchpadClickTimer: number = -1;
  private touchpadClickX: number = 0;
  private touchpadClickY: number = 0;
  // The second tap of a double was consumed, so its Release is swallowed too.
  private touchpadMiddleSwallow: boolean = false;
  // deviceId -> whether that input device is a touchpad (see isTouchpadDevice).
  private touchpadDevices: Map<number, boolean> = new Map<number, boolean>();
  private deviceChangeListener: (data: inputDevice.DeviceListener) => void =
    (data: inputDevice.DeviceListener) => {
      this.touchpadDevices.clear();
    };
  // Touchpad pinch state: axisPinch is the ratio of the current two-finger
  // distance to the distance when the pinch started (starts at 1).
  private pinchScale: number = 0;
  private pinchAccum: number = 0;
  // The gesture (and its synthetic contacts / held Ctrl) lasts from the first PINCH
  // axis event until the OS sends the real AxisAction.END, i.e. the fingers are
  // actually lifted. No timers: a stationary hold keeps it alive instead of lifting
  // the contacts into a fake click.
  private pinchActive: boolean = false;
  private pinchCtrlDown: boolean = false;
  private pinchZoomSpeed: number = 1;
  private readonly ctrlScancode: number = 0x1D;
  // How a touchpad two-finger pinch is handled, as the two flags the gesture
  // branches on: a synthetic native two-finger touch, or a local picture zoom.
  // (Ctrl+wheel, the default, is neither.)
  private pinchAsTouch: boolean = false;
  private pinchDirect: boolean = false;
  private pinchTouchDistance: number = DEFAULT_PINCH_TOUCH_DISTANCE;
  private touchPinchActive: boolean = false;
  // While a synthetic-touch pinch is running the OS hides the mouse pointer (a
  // touch is in progress), which would leave the user with no aim point. With the
  // RDP cursor on, the system cursor is swapped to a native hand for the duration
  // so the pinch anchor stays visible: open hand while spreading (zoom in), grab
  // hand while closing (zoom out).
  private touchPinchCursor: boolean = false;
  private touchPinchZoomIn: boolean = true;
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
  // Direct (local) picture zoom: the transform on the XComponent wrapper plus the
  // state behind it (1.0 = fitted view). Kept as numbers so a later pinch can
  // anchor on where the picture currently is (see the direct-zoom methods).
  @State private viewMatrix: Matrix4Transit = matrix4.identity();
  private viewScale: number = 1;
  private viewTransX: number = 0;
  private viewTransY: number = 0;
  // Pinch-start snapshot: the anchor under the cursor and the screen offset that
  // keeps it there; only the scale changes during the gesture.
  private viewPinchAnchorX: number = 0;
  private viewPinchAnchorY: number = 0;
  private viewPinchPanX: number = 0;
  private viewPinchPanY: number = 0;
  private viewPinchBaseScale: number = 1;
  private sessionWindow: window.Window | undefined = undefined;
  // Clipboard redirection is manual, driven by the toolbar. The remote
  // clipboard is cached when the server offers it; "复制" pulls it to the local
  // clipboard and "粘贴" (system paste control, no READ_PASTEBOARD) pushes the
  // local clipboard to the remote.
  // Latest rich payload pulled from the remote. Only one is populated at a time
  // (native requests the richest supported format per remote copy), so "复制"
  // can pick by presence alone.
  private remoteClipboardText: string = '';
  private remoteClipboardHtml: string = '';
  private remoteClipboardPixelMap: image.PixelMap | undefined = undefined;
  // Remote clipboard file list (names/sizes only; the bytes are pulled on
  // demand when the user presses 复制). `remoteFolderCount` counts advertised
  // directories, which the file clipboard cannot stream.
  private remoteFileCount: number = 0;
  private remoteFolderCount: number = 0;
  private pasteboardBusy: boolean = false;
  // File transfer progress shown in the toolbar: 0 = remote -> local,
  // 1 = local -> remote.
  @State private transferActive: boolean = false;
  // An upload is advertised but the remote has not pasted yet: show the hint in
  // place of the progress row instead of a toast.
  @State private transferWaiting: boolean = false;
  @State private transferPercent: number = 0;
  @State private transferText: string = '';
  @State private transferSpeed: string = '';
  // Current file position for a multi-file transfer ("2/5").
  @State private transferFileIndex: number = 0;
  @State private transferFileCount: number = 0;
  private transferDirection: number = 0;
  private transferBusy: boolean = false;
  private transferLastDone: number = 0;
  private transferLastAt: number = 0;
  // The file descriptors and the download state machine live on the background
  // worker (services/BackgroundWorker); this window only keeps the last counts
  // for the toolbar hints and forwards the native events by handle.
  private backgroundSink: (event: BackgroundEvent) => void =
    (event: BackgroundEvent): void => {
      this.onBackgroundEvent(event);
    };
  // The native handle the audio worker is servicing (see services/AudioPlayer).
  // Tracked here so the renderer is released with the right handle even after
  // the native session has already reset its own (-1).
  private audioHandle: number = -1;
  // Local toggle-key state and the remote-side re-assert bookkeeping. The policy
  // (dedup + 1s rate limit) lives here; native only relays the server's report
  // and sends the PDU when asked (RdpNative.reassertLockState).
  private localLockFlags: number = 0;
  private localLockKnown: boolean = false;
  private lastRemoteLocks: number = 0;
  private lastRemoteLocksValid: boolean = false;
  private lastReassertMs: number = 0;
  // Clipboard negotiation (see onRemoteFormatList). Only one data request may be
  // in flight; a newer format list is remembered and pulled once the current one
  // settles (remote responses carry no format id).
  private remoteFormatInFlight: boolean = false;
  private remoteFormatPending: boolean = false;
  private remotePendingId: number = 0;
  private remotePendingKind: number = RdpClipKind.NONE;
  // Remote cursor shape, installed as the HarmonyOS system cursor. The server
  // sends the full bitmap + hot spot; we keep the last PixelMap so it can be
  // re-installed when the window re-acquires the pointer. `cursorHidden` tracks
  // the server's SYSPTR_NULL (e.g. a full-screen video hides the cursor).
  private cursorPixelMap: image.PixelMap | undefined = undefined;
  private cursorHotX: number = 0;
  private cursorHotY: number = 0;
  private cursorHidden: boolean = false;
  // Hash of the last installed cursor shape (geometry + pixels). Native hands
  // over every shape the server sends; identical repeats (pointer cache hits)
  // are dropped here so the system cursor is only re-installed on a real change.
  private cursorShapeHash: number = 0;
  private cursorShapeHashValid: boolean = false;
  // Set once a cursor API call fails, so we stop retrying on every update.
  private cursorSupported: boolean = true;
  // Global "使用RDP光标" setting; when off the session leaves the system cursor
  // alone and the native side ignores RDP pointer updates entirely.
  private useRdpCursor: boolean = true;
  // Global "回到前台自动重连" setting, plus the state of the check it arms: a
  // window that comes back to a session the system already dropped reconnects
  // without waiting for the user to press 重试.
  private reconnectOnForeground: boolean = false;
  // True between leaving the foreground and the reconnect check that follows the
  // return. A drop while the user is actually watching the window is left to the
  // manual 重试 button: turning that into an automatic retry loop is a different
  // (and unwanted) behaviour.
  private awayFromForeground: boolean = false;
  private reconnectTimer: number = -1;
  private appContext: common.ApplicationContext | undefined = undefined;
  private appStateCallback: ForegroundStateCallback | undefined = undefined;

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
  // 快捷键穿透 (ACL INTERCEPT_INPUT_EVENT): route this window's intercepted keys
  // to the remote (see updateShortcutPassthrough).
  private shortcutPassthrough: boolean = false;
  // Whether the native interceptor is registered for this session; a denied
  // permission leaves it false and the ordinary onKeyEvent path in charge.
  private interceptActive: boolean = false;
  // Focus of this window; the interceptor is released when another window
  // (settings, main list) becomes the active one.
  private windowFocused: boolean = true;

  aboutToAppear(): void {
    this.resolveRequest();
    const settings: AppSettings = SettingsStore.load();
    this.developerMode = settings.developerMode;
    // Developer mode adds the telemetry line, so the toolbar is taller and the
    // fullscreen auto-hide boundary must follow.
    this.toolbarHeight = this.developerMode
      ? TOOLBAR_ROW_HEIGHT + TOOLBAR_TELEMETRY_HEIGHT
      : TOOLBAR_ROW_HEIGHT;
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.touchpadWheel.setSpeed(settings.touchpadScrollSpeed);
    this.touchpadMiddleDoubleTap = settings.touchpadMiddleDoubleTap;
    try {
      // Device IDs can change on replug, so drop the touchpad cache on a change.
      inputDevice.on('change', this.deviceChangeListener);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'inputDevice.on change failed: %{public}s', JSON.stringify(error));
    }
    this.pinchZoomSpeed =
      Math.max(MIN_SPEED_SCALE, Math.min(MAX_SPEED_SCALE, settings.pinchZoomSpeed));
    this.pinchAsTouch = settings.pinchMode === PINCH_MODE_TOUCH;
    this.pinchDirect = settings.pinchMode === PINCH_MODE_ZOOM;
    this.touchPinchAngle = settings.pinchTouchAngle;
    this.pinchTouchDistance = settings.pinchTouchDistance;
    RdpNative.applyTouchHighRate(settings.touchHighRate);
    // Never ask for Vulkan presentation on a device that cannot do it (the
    // settings page already forces the stored value off; this covers a config
    // imported from another device).
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
    this.useRdpCursor = settings.useRdpCursor;
    this.reconnectOnForeground = settings.reconnectOnForeground;
    this.watchApplicationState();
    RdpNative.applyRdpCursor(settings.useRdpCursor);
    this.winKeySubstitute = settings.winKeySubstitute;
    this.shortcutPassthrough = settings.shortcutPassthrough;
    // With 超分辨率 the server renders the session (pre-upscale) size, not the
    // configured output size; the Resize event corrects this if the desktop turns
    // out different from what was asked for.
    this.desktopWidth = SettingsStore.srSessionSize(this.options.width,
      this.options.srEnabled ? this.options.srRatioPercent : 0);
    this.desktopHeight = SettingsStore.srSessionSize(this.options.height,
      this.options.srEnabled ? this.options.srRatioPercent : 0);
    this.loadSessionWindow();
    this.controller.setCallbacks(
      (surfaceId: string, width: number, height: number) => {
        this.surfaceId = surfaceId;
        this.surfaceWidth = width;
        this.surfaceHeight = height;
        this.native.bindSurface(surfaceId, width, height);
      },
      (surfaceId: string, width: number, height: number) => {
        // Hidden: hold the size and re-measure once the window is visible.
        if (!this.windowVisible) {
          this.pendingSurfaceResize = true;
          return;
        }
        this.applySurfaceSize(width, height);
      },
      (surfaceId: string) => {
        this.native.unbindSurface();
      }
    );
    // The manager owns the session and its native listener; this window only
    // mirrors the manager's state and renders the live session. If the session
    // dropped between "connected" and this window loading, the manager is the one
    // that already recorded it, so the drop is shown instead of silently
    // reconnecting.
    this.applySessionState(SessionManager.stateOf(this.connectionId),
      SessionManager.messageOf(this.connectionId));
    if (this.state === ConnectionState.Connected) {
      this.showOverlay = false;
      this.showToolbar = false;
    }
  }

  aboutToDisappear(): void {
    BackgroundWorker.unregisterSession(this.backgroundSink);
    this.stopAudio();
    // Give the shell its shortcuts back before the session goes away.
    if (this.interceptActive) {
      this.native.setShortcutPassthrough(false);
      this.interceptActive = false;
    }
    this.toolbarAutoHideArmed = false;
    this.clearToolbarTimers();
    this.stopMetricsTimer();
    if (this.moveTimer !== -1) {
      clearTimeout(this.moveTimer);
      this.moveTimer = -1;
    }
    if (this.reconnectTimer !== -1) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = -1;
    }
    this.unwatchApplicationState();
    try {
      inputDevice.off('change', this.deviceChangeListener);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'inputDevice.off change failed: %{public}s', JSON.stringify(error));
    }
    const win: window.Window | undefined = this.sessionWindow;
    this.sessionWindow = undefined;
    if (win !== undefined) {
      try {
        win.off('windowEvent');
        win.off('windowStatusChange');
        win.off('windowVisibilityChange');
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'off windowEvent failed: %{public}s', JSON.stringify(error));
      }
    }
    this.releaseAllInputs();
    this.restoreCursor();
    // The manager owns the session: this window never stops it (that would race
    // the window-close teardown). SessionAbility.release() is the one place.
    this.options.password = '';
    this.options.gatewayPassword = '';
  }

  onPageShow(): void {
    this.scheduleForegroundReconnect();
  }

  onPageHide(): void {
    this.awayFromForeground = true;
  }

  /**
   * Watches the process-level foreground transitions in addition to the page
   * lifecycle, so leaving and returning is noticed even where the system drives
   * only one of the two. Registered unconditionally: the callbacks re-read the
   * setting, so toggling it while a session is open takes effect without
   * reopening the window.
   */
  private watchApplicationState(): void {
    if (this.appStateCallback !== undefined) {
      return;
    }
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx === undefined) {
        return;
      }
      const application: common.ApplicationContext = ctx.getApplicationContext();
      const callback: ForegroundStateCallback = new ForegroundStateCallback(() => {
        this.awayFromForeground = true;
      }, () => {
        this.scheduleForegroundReconnect();
      });
      application.on('applicationStateChange', callback);
      this.appContext = application;
      this.appStateCallback = callback;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'watch application state failed: %{public}s', JSON.stringify(error));
    }
  }

  private unwatchApplicationState(): void {
    const application: common.ApplicationContext | undefined = this.appContext;
    const callback: ForegroundStateCallback | undefined = this.appStateCallback;
    this.appContext = undefined;
    this.appStateCallback = undefined;
    if (application === undefined || callback === undefined) {
      return;
    }
    try {
      application.off('applicationStateChange', callback);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'unwatch application state failed: %{public}s',
        JSON.stringify(error));
    }
  }

  /**
   * Arms the one reconnect attempt that covers coming back to a session the
   * system dropped while the app was away. Only a session that is actually down
   * is picked up - a deliberate 断开 or 返回 leaves the page in another state and
   * must not be undone by an automatic retry.
   */
  private scheduleForegroundReconnect(): void {
    if (!this.awayFromForeground || !this.reconnectOnForeground) {
      return;
    }
    if (this.reconnectTimer !== -1) {
      clearTimeout(this.reconnectTimer);
    }
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = -1;
      // Cleared even when nothing is reconnected: the flag means "we are away",
      // and running this check is exactly what makes it stale.
      const wasAway: boolean = this.awayFromForeground;
      this.awayFromForeground = false;
      if (wasAway &&
        (this.state === ConnectionState.Idle || this.state === ConnectionState.Failed)) {
        this.retry();
      }
    }, FOREGROUND_RECONNECT_GRACE_MS);
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
    const backend: SessionBackend | undefined = SessionBackendRegistry.find(this.sessionKey);
    if (backend instanceof RdpSessionBackend) {
      // Share the manager's native session instance: this window renders it, the
      // manager owns its lifecycle.
      this.native = backend.native;
    }
    // Download progress/completion events come back from the background worker;
    // route them to this window (it filters by its own current handle).
    BackgroundWorker.registerSession(this.backgroundSink);
    // The manager owns the session: it publishes the lifecycle state and forwards
    // the UI-effect events. This window only renders what it is given.
    SessionManager.attachWindow(this.sessionKey,
      (state: ConnectionState, message: string) => {
        this.applySessionState(state, message);
      },
      (signal: SessionSignal) => {
        this.onSessionSignal(signal);
      },
      () => {
        this.terminateWindow();
      });
  }

  /**
   * Renders the manager's state. Everything about "is this session up / why did it
   * fail" comes from here, so the list and this window can never disagree.
   */
  private applySessionState(state: ConnectionState, message: string): void {
    this.state = state;
    if (state === ConnectionState.Connected) {
      this.statusText = '已连接';
      this.showToolbar = false;
      // A fresh session starts unsuppressed; re-apply this window's visibility.
      this.native.setSuppressed(!this.windowVisible);
      // Audio runs on its own worker; the renderer is built once the session
      // reports its PCM format (AudioFormat event).
      this.audioHandle = this.native.handleId();
      AudioPlayer.start(this.audioHandle);
      // A fresh session has no request in flight and has not reported its lock
      // state yet.
      this.resetClipboardNegotiation();
      this.lastRemoteLocksValid = false;
      this.lastReassertMs = 0;
    } else if (state === ConnectionState.Connecting) {
      this.statusText = '正在连接...';
      this.showOverlay = true;
      this.showToolbar = true;
    } else if (state === ConnectionState.Disconnecting) {
      this.statusText = '正在断开...';
      this.stopAudio();
      this.resetClipboardNegotiation();
    } else {
      // Failed / Idle (a dropped session): show the reason the manager classified
      // and offer 重试.
      this.statusText = message.length > 0 ? message
        : (state === ConnectionState.Failed ? '连接失败' : '连接已断开');
      this.showOverlay = true;
      this.showToolbar = true;
      this.resetCursor();
      this.stopAudio();
      this.resetClipboardNegotiation();
    }
    // A state change invalidates any pending reveal / hide.
    this.refreshToolbarAutoHide();
    // 快捷键穿透 only makes sense on a live session.
    this.updateShortcutPassthrough();
    // Telemetry is only meaningful while connected, and it is pulled, not pushed.
    if (state === ConnectionState.Connected) {
      this.startMetricsTimer();
    } else {
      this.stopMetricsTimer();
    }
  }

  /** Releases this window's audio renderer on the audio worker. */
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

  /** Reads the native telemetry window and paints it (see applyCounters). */
  private refreshCounters(): void {
    const counters: RdpCounters | undefined = this.native.readCounters();
    if (counters !== undefined) {
      this.applyCounters(counters);
    }
  }

  private retry(): void {
    // An attempt is a full negotiation plus a logon, so a retry that lands while
    // one is already on its way (or still tearing down) is dropped instead of
    // tearing the session down and starting a second one.
    if (this.state === ConnectionState.Connecting ||
      this.state === ConnectionState.Disconnecting) {
      return;
    }
    if (this.connectionId.length === 0) {
      return;
    }
    // The manager refuses an attempt that is too soon after the last one; the
    // page only shows progress for an attempt that was really started (the
    // Connecting state is published by the manager).
    if (!SessionManager.retry(this.connectionId)) {
      return;
    }
    // Re-bind the surface: stopping a session destroyed its native window.
    if (this.surfaceId.length > 0) {
      this.native.bindSurface(this.surfaceId, this.surfaceWidth, this.surfaceHeight);
    }
  }

  /**
   * The manager forwards the protocol-neutral signal; this RDP window reads its
   * own event code and payload back out of the envelope.
   */
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
        // A new desktop size moves the fitted rect under any local zoom.
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
      this.cursorHidden = false;
      this.cursorPixelMap = undefined;
      this.cursorShapeHashValid = false;
      this.applyCursor();
    } else if (event === RdpEvent.CursorHidden) {
      this.cursorHidden = true;
      this.cursorShapeHashValid = false;
      this.applyCursor();
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

  /**
   * Paints one telemetry window pulled from the native side (see RdpCounters).
   * rtt/rx/tx/fps/audio drive the toolbar row; the four work phase fields are
   * summed into "本机" (see localUs); `syncUs`/`presentWaitUs` are blocked and stay
   * out of it, and the three GPU figures feed the GPU label/tooltip. There is no
   * pre-summed total and no string parsing here - native stores the struct and
   * this reader takes the fields it shows.
   */
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

  /**
   * Process CPU usage for the telemetry row. hidebug documents its interfaces as
   * comparatively expensive and meant for debugging, so it is called here - once per
   * metrics window - and the row itself only exists while developer options are on.
   * A device without the capability (or a failing call) leaves the figure at -1 and
   * the row shows "CPU --".
   */
  private sampleCpuUsage(): void {
    try {
      const usage: number = hidebug.getCpuUsage();
      this.metricsCpuPercent = usage >= 0 ? Math.round(usage * 100) : -1;
    } catch (error) {
      this.metricsCpuPercent = -1;
    }
  }

  /** "网络 28ms", or "网络 --" until the server reports network stats. */
  private rttLabel(): string {
    return this.metricsRtt < 0 ? '网络 --' : '网络 ' + this.metricsRtt + 'ms';
  }

  /** Green / amber / red vs. the given warn/bad thresholds (ms). */
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

  /**
   * The four per-frame work phases summed (ZGX decompress + parse, decode,
   * compose, present), in microseconds. The native side sends no total; this sum
   * is the reader's choice - blocked time (`sync`, and anything the native side
   * reports as its own blocked field) is deliberately not added in.
   */
  private localUs(): number {
    return this.metricsZgxUs + this.metricsDecodeUs + this.metricsComposeUs +
      this.metricsPresentUs;
  }

  /**
   * "本机" in ms, or -1 when no frame ended in the window (no phase reported).
   * Reported to the ones place like the other latency figures so the row stays
   * comparable.
   */
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

  /** One-decimal ms, for the GPU figures which are a few ms at most. */
  private usToMsTenth(us: number): string {
    return (us / 1000).toFixed(1);
  }

  /** "CPU 42%", or "CPU --" when the device cannot report process CPU usage. */
  private cpuLabel(): string {
    return this.metricsCpuPercent < 0 ? 'CPU --' : 'CPU ' + this.metricsCpuPercent + '%';
  }

  /**
   * "GPU 3.2ms": the GPU time one presented frame costs us - upload + 超分辨率
   * upscale + letterbox. Not a utilization percentage, because the platform exposes
   * no per-process GPU load to an app; it is the figure that answers "what does 超分
   * add", with the split in the tooltip.
   */
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

  /** "本机" tooltip: the per-frame breakdown, every figure to the ones place. */
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
  private onCursorShape(payload: RdpEventPayload): void {
    const imageHandle: number = payload.imageHandle ?? 0;
    if (imageHandle <= 0) {
      return;
    }
    // Always read (and release) the buffer, even when this session will not use
    // it, so the native handle can never leak.
    const bytes: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(imageHandle);
    const width: number = payload.width ?? 0;
    const height: number = payload.height ?? 0;
    const hotX: number = payload.hotX ?? 0;
    const hotY: number = payload.hotY ?? 0;
    if (bytes === undefined || width <= 0 || height <= 0) {
      return;
    }
    if (!this.cursorSupported || !this.useRdpCursor) {
      return;
    }
    // Native already downscales oversized cursors to <=256; this is only a
    // safety net should an out-of-range size ever reach the UI.
    if (width > 256 || height > 256) {
      this.cursorHidden = false;
      this.cursorPixelMap = undefined;
      this.cursorShapeHashValid = false;
      this.applyCursor();
      return;
    }
    const hash: number = SessionPage.cursorHash(width, height, hotX, hotY, bytes);
    if (this.cursorShapeHashValid && hash === this.cursorShapeHash) {
      return;
    }
    const options: image.InitializationOptions = {
      editable: false,
      pixelFormat: image.PixelMapFormat.BGRA_8888,
      size: { width: width, height: height }
    };
    try {
      this.cursorPixelMap = image.createPixelMapSync(bytes, options);
      this.cursorHotX = Math.max(0, Math.min(hotX, width));
      this.cursorHotY = Math.max(0, Math.min(hotY, height));
      this.cursorHidden = false;
      this.cursorShapeHash = hash;
      this.cursorShapeHashValid = true;
      this.applyCursor();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'create cursor pixelmap failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Dedup key over the cursor geometry and pixels (native sends every shape). */
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

  /** Decodes a raw CF_HTML payload (ClipboardCodec) into the remote HTML cache. */
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

  /** Converts a raw RTF payload (ClipboardCodec) into the remote HTML cache. */
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

  /** Caches remote HTML as the active remote clipboard content. */
  private setRemoteHtml(html: string): void {
    this.remoteClipboardHtml = html;
    this.remoteClipboardText = '';
    this.remoteClipboardPixelMap = undefined;
    this.remoteFileCount = 0;
    this.remoteFolderCount = 0;
  }

  /**
   * The server's raw FILEGROUPDESCRIPTORW: the background worker owns the file
   * list (FileTransferCodec) and the download state machine, so the bytes are
   * handed over (transferred, zero-copy); the counts come back as a worker event
   * (fileListSummary). Only the mandatory native->ArkTS copy happens here.
   */
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

  /**
   * The server's clipboard changed: drop the cached counts and let the worker
   * abort any download in flight (it owns the state machine now).
   */
  private onRemoteClipboardChanged(): void {
    this.remoteFileCount = 0;
    this.remoteFolderCount = 0;
    BackgroundWorker.postClipboardChanged(this.native.handleId());
  }

  /**
   * The server offered clipboard formats: pick the richest one ourselves (the
   * choice is policy, not wire handling) and pull it. If a request is already in
   * flight the newest list wins and is pulled once the current response settles.
   */
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

  /** One data request settled: pull whatever was deferred while it ran. */
  private onRemoteFormatResponse(): void {
    this.remoteFormatInFlight = false;
    if (this.remoteFormatPending) {
      this.remoteFormatPending = false;
      this.issueRemoteFormat(this.remotePendingId, this.remotePendingKind);
    }
  }

  /**
   * Picks one candidate from "kind:id,kind:id" by pull priority: files > image >
   * HTML > RTF > text (mirrors the old native order). Native already chose the
   * id within a kind (DIB before DIBV5), so the first candidate of a kind wins.
   */
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

  /** Drops the in-flight/deferred clipboard request state (session changed). */
  private resetClipboardNegotiation(): void {
    this.remoteFormatInFlight = false;
    this.remoteFormatPending = false;
    this.remotePendingId = 0;
    this.remotePendingKind = RdpClipKind.NONE;
  }

  /**
   * The server reported its toggle-key state. Local wins: if we know the local
   * state and it differs, re-assert it (rate-limited so a server that keeps
   * disagreeing cannot make the two sides trade places forever).
   */
  private onRemoteLockState(lockFlags: number): void {
    if (this.lastRemoteLocksValid && this.lastRemoteLocks === lockFlags) {
      return;
    }
    this.lastRemoteLocks = lockFlags;
    this.lastRemoteLocksValid = true;
    if (!this.localLockKnown || lockFlags === this.localLockFlags) {
      return;
    }
    const now: number = Date.now();
    if (now - this.lastReassertMs < 1000) {
      return;
    }
    this.lastReassertMs = now;
    this.native.reassertLockState(this.localLockFlags);
  }

  /** Decodes a remote CF_DIB into a PixelMap (the DIB codec lives in ArkTS). */
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
      if (this.touchPinchCursor) {
        // Locator for the synthetic touch pinch: touch activity hides the pointer,
        // and the remote cursor bitmap would not be shown either, so a built-in
        // hand keeps the anchor visible and shows the gesture direction -
        // spreading (zoom in) is the open hand, closing (zoom out) the grab hand.
        pointer.setPointerVisibleSync(true);
        // A style the device does not implement must not take the whole cursor
        // path down (the outer catch disables it for the session), so fall back to
        // the plain arrow locally.
        try {
          pointer.setPointerStyleSync(windowId, this.touchPinchZoomIn
            ? pointer.PointerStyle.HAND_OPEN
            : pointer.PointerStyle.HAND_GRABBING);
        } catch (styleError) {
          pointer.setPointerStyleSync(windowId, pointer.PointerStyle.DEFAULT);
        }
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
    this.touchPinchCursor = false;
    this.cursorPixelMap = undefined;
    this.cursorShapeHashValid = false;
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

  /**
   * One file transfer at a time. A second 复制 / 粘贴 while one is running (or
   * while an upload is still waiting for the remote paste) would interleave two
   * state machines in one progress row - and, for uploads, invalidate the file
   * list the server is already pulling - so both entries share this gate.
   */
  private fileTransferBlocking(): boolean {
    return this.transferBusy || this.transferActive || this.transferWaiting;
  }

  private rejectConcurrentTransfer(): void {
    this.toast('正在传输文件，请先取消或等待完成');
  }

  /** Local clipboard -> remote, invoked by the toolbar's system paste button. */
  private submitClipboard(): void {
    if (this.fileTransferBlocking()) {
      this.rejectConcurrentTransfer();
      return;
    }
    const board: pasteboard.SystemPasteboard = pasteboard.getSystemPasteboard();
    // A URI record means the clipboard may hold files; the metadata check needs
    // no permission and routes to the file path below. Non-file URIs (web
    // links) produce no files there and fall back to plain text.
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

  /** Local clipboard -> remote for text / HTML / image (the non-file path). */
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

  /**
   * Local clipboard files -> remote. The system copies the clipboard files into
   * our sandbox (`getDataWithProgress`), so no READ_PASTEBOARD permission and no
   * manual URI handling is needed; the sandbox copies are then advertised to the
   * server, which pulls them over cliprdr.
   */
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
            // getDataWithProgress cannot copy folders, so a file URI that
            // produced nothing is a folder.
            this.toast('不支持文件夹传输');
            return;
          }
          // A web link or other non-file URI: paste it as text.
          this.submitPlainClipboard();
          return;
        }
        // The button only advertises the files on the remote clipboard: cliprdr
        // is pull-based, so the bytes move when the remote side pastes. That is
        // not a running transfer, so release the busy guard first.
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
        // getDataWithProgress rejects for a folder (directories are not
        // supported by the system file-copy path).
        this.transferBusy = false;
        hilog.warn(DOMAIN, TAG, 'read clipboard files failed: %{public}s',
          JSON.stringify(error));
        this.toast('不支持文件夹传输');
      });
  }

  /** True when the clipboard's URI is a file/folder rather than a web link. */
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

  /** File-transfer progress for the toolbar. */
  private onFileTransferProgress(payload: RdpEventPayload): void {
    const direction: number = payload.direction ?? 0;
    const done: number = payload.doneBytes ?? 0;
    const total: number = payload.totalBytes ?? 0;
    this.transferFileIndex = payload.fileIndex ?? 0;
    this.transferFileCount = payload.fileCount ?? 0;
    // An upload only advertises the list; native sends a 0% placeholder when
    // that happens. Keep the "waiting for the remote paste" hint then, and only
    // switch to the progress row once bytes actually move.
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

  /**
   * Writes the finished download's sandbox files to the system clipboard as file
   * URIs, so any app can paste them.
   */
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
    // Resumes the normal auto-hide (unless the pointer is still on the strip).
    this.refreshToolbarAutoHide();
  }

  private cancelFileTransfer(): void {
    if (this.transferDirection === 1) {
      // Aborting an upload leaves the remote clipboard listing files it can no
      // longer read, so its paste reports a protocol error. That is harmless
      // but worth warning about first.
      this.getUIContext().showAlertDialog({
        title: '取消文件传输',
        message: '文件仍在发送。取消后远端粘贴会提示错误，已传部分也会作废。是否继续取消？',
        systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY),
        primaryButton: {
          value: '继续传输',
          action: () => {
          }
        },
        secondaryButton: {
          value: '取消传输',
          fontColor: $r('app.color.danger'),
          action: () => {
            this.abortFileTransfer();
          }
        }
      });
      return;
    }
    // Downloads are safe to stop: we are the side pulling the bytes, and the
    // state machine (with its partial-file cleanup) is on the background worker,
    // which reports the cancellation back as a downloadFailed event.
    BackgroundWorker.cancelDownload(this.native.handleId());
  }

  private abortFileTransfer(): void {
    this.native.cancelFileTransfer();
    this.finishFileTransfer();
    this.toast('已取消文件传输');
  }

  /** Creates (and returns) a per-transfer directory under the app sandbox. */
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

  /**
   * Every transfer gets a fresh timestamped directory and the clipboard only
   * ever references the newest one, so the previous directory is unreachable
   * (its URIs were replaced by this transfer). Without this the sandbox would
   * grow by one directory per transfer, forever.
   */
  private static purgeStaleTransferDirs(root: string, keep: string): void {
    let names: string[] = [];
    try {
      names = fileIo.listFileSync(root);
    } catch (error) {
      return;  // Root absent or unreadable: nothing to purge.
    }
    for (const name of names) {
      if (name === keep) {
        continue;
      }
      const path: string = `${root}/${name}`;
      try {
        // Unlink the contents first so this works whether or not rmdirSync
        // removes a non-empty directory.
        for (const child of fileIo.listFileSync(path)) {
          try {
            fileIo.unlinkSync(`${path}/${child}`);
          } catch (error) {
            // Already gone, or not a plain file: leave it to rmdirSync.
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

  /** Basename of an absolute sandbox path (the file clipboard drops the dir). */
  private static baseName(path: string): string {
    const slash: number = path.lastIndexOf('/');
    return slash < 0 ? path : path.substring(slash + 1);
  }

  /** Size of a sandbox file, or 0 when it cannot be stat'ed. */
  private static fileSizeOf(path: string): number {
    try {
      return fileIo.statSync(path).size;
    } catch (error) {
      return 0;
    }
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
          // Encode + native push on the worker (a large document must not block).
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
    BackgroundWorker.pushText(this.native.handleId(), text).then((ok: boolean) => {
      // The clipboard is pull-based: advertise puts it on the remote clipboard
      // (the remote can paste); the bytes move only when it pastes.
      this.toast(ok ? '已放入远端剪贴板' : '提交本机文本失败');
    });
  }

  /**
   * Local image -> remote. The pixel read is a PixelMap API (UI thread), but the
   * CF_DIB encode and the native push run on the worker so a large image cannot
   * block the UI.
   */
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
            // `buffer` is transferred to the worker (zero-copy); it encodes both
            // CF_DIB and PNG and pushes them.
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

  /** Remote clipboard -> local, invoked by the toolbar's pull button. */
  private pullClipboard(): void {
    if (this.pasteboardBusy) {
      return;
    }
    if (this.fileTransferBlocking()) {
      this.rejectConcurrentTransfer();
      return;
    }
    if (this.remoteFolderCount > 0) {
      // Folders are unsupported in both directions, so a list that contains any
      // directory is refused whole rather than partially downloaded.
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

  /**
   * Remote files -> local. The download state machine and its file IO run on the
   * background worker; this window only creates the per-transfer sandbox
   * directory (it owns the app context) and hands the worker the session handle
   * and the directory. Progress/completion arrive as worker events.
   */
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

  /** One FILECONTENTS response: forward it (bytes stay native) to the worker. */
  private onRemoteFileChunk(payload: RdpEventPayload): void {
    BackgroundWorker.postFileChunk(this.native.handleId(),
      payload.listIndex ?? 0, payload.isSize ?? false, payload.sizeValue ?? 0,
      payload.failed ?? false, payload.dataHandle ?? 0);
  }

  /**
   * Background-worker events for this session's download (the worker facade
   * routes every event to every window; ignore other windows' handles). The
   * worker owns the state machine, so these replace the old inline
   * progress/finish/abort handlers.
   */
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

  /** Paints the toolbar progress row from a worker progress event. */
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
      this.getUIContext().getPromptAction().showToast({
        message: message,
        duration: 1500,
        systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY)
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'show toast failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Reports which zone the pointer is in; zones come from the surface or the strip. */
  private setToolbarPointerZone(zone: number): void {
    if (this.pointerZone === zone) {
      return;
    }
    this.pointerZone = zone;
    this.refreshToolbarAutoHide();
  }

  /**
   * The single place the toolbar's fullscreen visibility is decided: a hide is
   * never pending while the pointer is on the strip, whatever triggered the call.
   */
  private refreshToolbarAutoHide(): void {
    if (!this.toolbarAutoHideArmed) {
      return;
    }
    // Nothing but a pending reveal keeps the hint up, so anything else retracts it.
    if (!this.toolbarWillReveal()) {
      this.hideRevealHint();
    }
    // Outside fullscreen the strip is either laid out above the picture
    // (windowed mode) or covered by the status overlay, so it cannot auto-hide.
    if (!this.isFullscreen || this.state !== ConnectionState.Connected) {
      this.clearToolbarTimers();
      return;
    }
    // A running or pending transfer pins the strip.
    if (this.transferActive || this.transferWaiting) {
      this.pinToolbar();
      return;
    }
    if (this.pointerZone !== TOOLBAR_ZONE_NONE) {
      this.cancelHideTimer();
    }
    if (this.toolbarWillReveal()) {
      // Dwell in the top band reveals a hidden strip; leaving the band cancels
      // the dwell (the gesture was a pass-by).
      if (this.revealTimer === -1) {
        this.revealTimer = setTimeout(() => {
          this.revealTimer = -1;
          this.showToolbar = true;
          this.hideRevealHint();
        }, this.hoverDelay);
        this.showRevealHint(this.hoverDelay);
      }
      return;
    }
    this.cancelRevealTimer();
    if (this.pointerZone === TOOLBAR_ZONE_NONE) {
      this.scheduleToolbarHide();
      return;
    }
    // Pointer on the strip itself: it stays up, whatever revealed it.
    this.showToolbar = true;
  }

  /** True while the pointer dwells for a strip that is not up yet. */
  private toolbarWillReveal(): boolean {
    return this.isFullscreen
      && this.state === ConnectionState.Connected
      && !this.transferActive
      && !this.transferWaiting
      && this.pointerZone === TOOLBAR_ZONE_TRIGGER
      && !this.showToolbar;
  }

  /** Fills the top-edge hint over `duration`, i.e. over the pending dwell. */
  private showRevealHint(duration: number): void {
    this.getUIContext().animateTo({ duration: duration, curve: Curve.Linear }, () => {
      this.revealHintFill = 1;
    });
  }

  /** Retracts the hint: the strip will not appear after all. */
  private hideRevealHint(): void {
    if (this.revealHintFill === 0) {
      return;
    }
    this.getUIContext().animateTo({
      duration: REVEAL_HINT_FADE_MS,
      curve: Curve.FastOutSlowIn
    }, () => {
      this.revealHintFill = 0;
    });
  }

  /** Pins the strip for as long as a transfer is running or pending. */
  private pinToolbar(): void {
    this.clearToolbarTimers();
    if (!this.showToolbar) {
      this.showToolbar = true;
    }
  }

  private scheduleToolbarHide(): void {
    if (this.showToolbar && this.hideTimer === -1) {
      this.hideTimer = setTimeout(() => {
        this.hideTimer = -1;
        this.showToolbar = false;
      }, this.hideDelay);
    }
  }

  private clearToolbarTimers(): void {
    this.cancelRevealTimer();
    this.cancelHideTimer();
  }

  private cancelRevealTimer(): void {
    if (this.revealTimer !== -1) {
      clearTimeout(this.revealTimer);
      this.revealTimer = -1;
    }
  }

  private cancelHideTimer(): void {
    if (this.hideTimer !== -1) {
      clearTimeout(this.hideTimer);
      this.hideTimer = -1;
    }
  }

  private lengthToNumber(value: Length): number {
    if (typeof value === 'number') {
      return value as number;
    }
    return 0;
  }

  /**
   * The fitted (letterboxed) rectangle of the desktop inside the component, in
   * vp: `[x, y, width, height]`. It is the mapping both the pointer translation
   * and the direct picture zoom are built on, so they cannot drift apart.
   */
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

  /**
   * 触控板: whether an event came from a touchpad. A touchpad's own gestures
   * (two-finger scroll / pinch) carry SourceTool.TOUCHPAD. A touchpad *tap*,
   * however, is delivered as a standard mouse button event and loses that tag
   * (sourceTool MOUSE, exactly like a real mouse), so the event's originating
   * input device is the fallback. sourceTool is checked first, so the
   * high-frequency touchpad scroll never pays for the device lookup.
   */
  private isTouchpad(deviceId: number | undefined, sourceTool: SourceTool | undefined): boolean {
    return sourceTool === SourceTool.TOUCHPAD || this.isTouchpadDevice(deviceId);
  }

  /**
   * Whether the input device with this id is a touchpad, from inputDevice's
   * device sources ('touchpad'). Cached per device id; ids can change on replug,
   * so the cache is dropped on a device change.
   */
  private isTouchpadDevice(deviceId: number | undefined): boolean {
    if (deviceId === undefined || deviceId <= 0) {
      return false;
    }
    const cached: boolean | undefined = this.touchpadDevices.get(deviceId);
    if (cached !== undefined) {
      return cached;
    }
    try {
      const info: inputDevice.InputDeviceData = inputDevice.getDeviceInfoSync(deviceId);
      const touchpad: boolean = info.sources.indexOf('touchpad') >= 0;
      this.touchpadDevices.set(deviceId, touchpad);
      return touchpad;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'getDeviceInfo(%{public}d) failed: %{public}s', deviceId,
        JSON.stringify(error));
    }
    return false;
  }

  private handleMouse(event: MouseEvent): void {
    // A touchscreen can also emit compatibility mouse events; those are owned by
    // handleTouch. Forward only real mouse / touchpad mouse input here so the
    // remote never sees two input streams for one finger (which shows up as
    // stray clicks during a drag).
    if (event.source === SourceType.TouchScreen) {
      return;
    }
    // The strip itself is never reported here: its own hover does that.
    this.setToolbarPointerZone(event.y < this.revealBand
      ? TOOLBAR_ZONE_TRIGGER : TOOLBAR_ZONE_NONE);
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
    // 触控板中键手势: take over touchpad right clicks while the setting is on so a
    // two-finger double tap can become middle; real mice are untouched.
    if (this.touchpadMiddleDoubleTap && event.button === MouseButton.Right
      && this.isTouchpad(event.deviceId, event.sourceTool)) {
      this.lastPointerX = remoteX;
      this.lastPointerY = remoteY;
      this.handleTouchpadRight(event.action, remoteX, remoteY);
      return;
    }
    this.lastPointerX = remoteX;
    this.lastPointerY = remoteY;
    if (event.action === MouseAction.Press) {
      this.flushMouseMove();
      this.native.mouse(RdpPointerFlags.MOVE, remoteX, remoteY);
      this.native.mouse(button | RdpPointerFlags.DOWN, remoteX, remoteY);
      if (button !== 0) {
        this.heldButtons.add(button);
      }
    } else if (event.action === MouseAction.Release) {
      this.flushMouseMove();
      this.native.mouse(button, remoteX, remoteY);
      this.heldButtons.delete(button);
    } else if (event.action === MouseAction.Move) {
      this.queueMouseMove(remoteX, remoteY);
    }
  }

  /**
   * 触控板中键手势: a two-finger tap is a right click and two of them in a row
   * mean middle. A single one is only known to be single once the window passes,
   * so it is held back; each click is sent as one whole press+release, so
   * nothing can stick if focus is lost while one is pending.
   */
  private handleTouchpadRight(action: MouseAction, x: number, y: number): void {
    if (action === MouseAction.Press) {
      if (this.touchpadClickPending) {
        // Second tap inside the window: the double is confirmed -> middle.
        this.cancelTouchpadClickTimer();
        this.touchpadClickPending = false;
        this.touchpadMiddleSwallow = true;
        this.sendSyntheticClick(RdpPointerFlags.BUTTON3, x, y);
        return;
      }
      this.touchpadClickPending = true;
      this.touchpadClickX = x;
      this.touchpadClickY = y;
      return;
    }
    if (action !== MouseAction.Release) {
      return;
    }
    if (this.touchpadMiddleSwallow) {
      this.touchpadMiddleSwallow = false;
      return;
    }
    if (this.touchpadClickPending && this.touchpadClickTimer === -1) {
      this.touchpadClickTimer = setTimeout(() => {
        this.touchpadClickTimer = -1;
        if (!this.touchpadClickPending) {
          return;
        }
        this.touchpadClickPending = false;
        this.sendSyntheticClick(RdpPointerFlags.BUTTON2, this.touchpadClickX, this.touchpadClickY);
      }, TOUCHPAD_DOUBLE_TAP_MS);
    }
  }

  /** One whole click as press + release, so the remote never keeps a button down. */
  private sendSyntheticClick(button: number, x: number, y: number): void {
    this.flushMouseMove();
    this.native.mouse(RdpPointerFlags.MOVE, x, y);
    this.native.mouse(button | RdpPointerFlags.DOWN, x, y);
    this.native.mouse(button, x, y);
  }

  private cancelTouchpadClickTimer(): void {
    if (this.touchpadClickTimer !== -1) {
      clearTimeout(this.touchpadClickTimer);
      this.touchpadClickTimer = -1;
    }
  }

  private clearTouchpadClick(): void {
    this.cancelTouchpadClickTimer();
    this.touchpadClickPending = false;
    this.touchpadMiddleSwallow = false;
  }

  /**
   * Coalesces pointer moves: `onMouse` fires at the device polling rate, and
   * every event would otherwise become its own FastPath input PDU. A faster link
   * means less backpressure, so an uncapped stream of moves is exactly the input
   * load the remote has to absorb. At most one MOVE per
   * MOUSE_MOVE_MIN_INTERVAL_MS is sent; the newest position is always kept and a
   * trailing flush is scheduled, so the pointer never stops short of where it
   * really is.
   */
  private queueMouseMove(x: number, y: number): void {
    this.pendingMoveX = x;
    this.pendingMoveY = y;
    const elapsed: number = Date.now() - this.lastMoveSentAt;
    if (elapsed >= MOUSE_MOVE_MIN_INTERVAL_MS) {
      this.flushMouseMove();
    } else if (this.moveTimer === -1) {
      this.moveTimer = setTimeout(() => {
        this.moveTimer = -1;
        this.flushMouseMove();
      }, MOUSE_MOVE_MIN_INTERVAL_MS - elapsed);
    }
  }

  /** Sends the pending move, if any. Called before any non-move pointer event. */
  private flushMouseMove(): void {
    if (this.moveTimer !== -1) {
      clearTimeout(this.moveTimer);
      this.moveTimer = -1;
    }
    if (this.pendingMoveX < 0) {
      return;
    }
    const x: number = this.pendingMoveX;
    const y: number = this.pendingMoveY;
    this.pendingMoveX = -1;
    this.pendingMoveY = -1;
    this.lastMoveSentAt = Date.now();
    this.native.mouse(RdpPointerFlags.MOVE, x, y);
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
    // Zoom mode: while the picture is magnified, a scroll pans it locally and
    // nothing reaches the remote; at 100% the normal scroll returns. Touchpad
    // displacement is in vp (1:1), a wheel reports notches (fixed step).
    if (this.pinchDirect && this.viewScale > MIN_VIEW_SCALE) {
      if (this.isTouchpad(event.deviceId, event.sourceTool)) {
        this.panView(-horizontal, -vertical);
      } else {
        this.panView(0, -vertical * PAN_WHEEL_STEP);
      }
      return;
    }
    const point: number[] = this.mapPointer(event.x, event.y);
    if (this.isTouchpad(event.deviceId, event.sourceTool)) {
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
      if (this.pinchDirect) {
        this.beginViewPinch(event, scale);
      } else if (this.pinchAsTouch) {
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
    if (this.pinchDirect) {
      this.updateViewPinch(scale);
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
   * Direct-zoom pinch start: anchor on the point under the cursor (the desktop
   * pixel there must not move) and let the gesture ratio only change the scale.
   * The cursor is the anchor, 100% the floor, and the pan is clamped to cover the
   * viewport (centred once the picture is smaller) - the automatic alignment.
   */
  private beginViewPinch(event: AxisEvent, scale: number): void {
    const compWidth: number = this.componentWidth > 0 ? this.componentWidth : 1;
    const compHeight: number = this.componentHeight > 0 ? this.componentHeight : 1;
    const cx: number = compWidth / 2;
    const cy: number = compHeight / 2;
    this.viewPinchAnchorX = Math.max(0, Math.min(compWidth, event.x));
    this.viewPinchAnchorY = Math.max(0, Math.min(compHeight, event.y));
    this.viewPinchBaseScale = this.viewScale;
    // The anchor's current screen position is C + s0*(A - C) + T0; the gesture
    // keeps exactly that, so the constant pan below is its offset from A.
    this.viewPinchPanX = cx + this.viewScale * (this.viewPinchAnchorX - cx) + this.viewTransX
      - this.viewPinchAnchorX;
    this.viewPinchPanY = cy + this.viewScale * (this.viewPinchAnchorY - cy) + this.viewTransY
      - this.viewPinchAnchorY;
    this.updateViewPinch(scale > 0 ? scale : 1);
  }

  /**
   * Applies the gesture ratio on top of the scale the pinch started at (axisPinch
   * is already relative to the start), clamped to the allowed zoom range.
   */
  private updateViewPinch(scale: number): void {
    const ratio: number = Math.max(MIN_PINCH_RATIO, Math.min(MAX_PINCH_RATIO,
      scale > 0 ? scale : 1));
    const clamped: number = Math.max(MIN_VIEW_SCALE, Math.min(MAX_VIEW_SCALE,
      this.viewPinchBaseScale * ratio));
    this.applyViewZoom(clamped);
  }

  /** Rebuilds the transform for `scale`, keeping the pinch anchor in place. */
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

  /** Pans the magnified picture by a screen shift (vp), clamped like the zoom. */
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

  /**
   * Covers the viewport while the picture is larger than it, and centres it once
   * it is smaller - this is also what aligns the view back to 100%.
   */
  private clampViewEdge(position: number, viewport: number, content: number): number {
    if (content >= viewport) {
      return Math.max(viewport - content, Math.min(0, position));
    }
    return (viewport - content) / 2;
  }

  /** The transform ArkUI applies: scale about the component centre + px shift. */
  private commitViewMatrix(): void {
    const pxPerVp: number = this.getUIContext().vp2px(1);
    this.viewMatrix = matrix4.init([
      this.viewScale, 0, 0, 0,
      0, this.viewScale, 0, 0,
      0, 0, 1, 0,
      this.viewTransX * pxPerVp, this.viewTransY * pxPerVp, 0, 1
    ]);
  }

  /** Drops the local zoom back to the fitted (100%) view. */
  private resetViewZoom(): void {
    if (this.viewScale === MIN_VIEW_SCALE && this.viewTransX === 0 && this.viewTransY === 0) {
      return;
    }
    this.viewScale = MIN_VIEW_SCALE;
    this.viewTransX = 0;
    this.viewTransY = 0;
    this.viewMatrix = matrix4.identity();
  }

  /** Snaps a pinch that ended just above 100% smoothly back to exactly 100%. */
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
    // Keep an aim point while the synthetic contacts are down (the OS hides the
    // pointer during touch) - only meaningful with the RDP cursor on. Start on the
    // open hand and let the direction pick between the two.
    this.touchPinchCursor = true;
    this.touchPinchZoomIn = true;
    // axisPinch is already relative to the gesture start (1.0); apply the
    // initial sample so a late first event does not drop the first movement.
    this.updateTouchPinch(scale);
    this.applyCursor();
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
    // Flip the locator between open (spreading, zoom in) and grab (closing, zoom
    // out) hand only past the dead zone, and only when it actually changes - a
    // per-motion pointer-style call would be a syscall per axis event.
    const zoomIn: boolean = ratio > 1 + PINCH_CURSOR_HYSTERESIS
      ? true : (ratio < 1 - PINCH_CURSOR_HYSTERESIS ? false : this.touchPinchZoomIn);
    if (zoomIn !== this.touchPinchZoomIn) {
      this.touchPinchZoomIn = zoomIn;
      this.applyCursor();
    }
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
    this.touchPinchCursor = false;
    this.applyCursor();
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
    // Direct mode: a pinch that stopped just above 100% settles back to it.
    this.snapViewZoom();
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

  /**
   * 快捷键穿透: register/release the interceptor. It may only be held while this
   * window is the active one and the session is live, because the interceptor
   * sees the whole application's keys.
   */
  private updateShortcutPassthrough(): void {
    const wanted: boolean = this.shortcutPassthrough
      && this.state === ConnectionState.Connected
      && this.windowFocused;
    if (wanted === this.interceptActive) {
      return;
    }
    if (wanted) {
      // Keep the real registration result: a denied ACL permission leaves this
      // false and the ordinary onKeyEvent path stays in charge.
      this.interceptActive = this.native.setShortcutPassthrough(true);
    } else {
      this.native.setShortcutPassthrough(false);
      this.interceptActive = false;
    }
  }

  /**
   * 快捷键穿透: an intercepted key, mapped and accounted for like onKeyEvent so
   * focus loss still sends the matching key-up.
   */
  private handleInterceptedKey(payload: RdpEventPayload): void {
    const keyCode: number = payload.keyCode ?? -1;
    if (keyCode < 0) {
      return;
    }
    const down: boolean = payload.down ?? false;
    const scan: RdpScanCode | undefined = KeyMapper.map(keyCode);
    if (scan === undefined) {
      return;
    }
    this.trackScanKey('s' + scan.scancode + (scan.extended ? 'e' : ''), scan.scancode,
      scan.extended, down);
    this.native.key(scan.scancode, down, scan.extended);
  }

  private handleKey(event: KeyEvent): void {
    // The interceptor already forwarded and swallowed this key; don't send twice.
    if (this.interceptActive) {
      return;
    }
    const down: boolean = event.type === KeyType.Down;
    // The real Win key is owned by the system, so it is never forwarded;
    // Win for the remote is produced by the configurable substitute key.
    if (event.keyCode === 2076 || event.keyCode === 2077) {
      return;
    }
    // The remote session follows the local toggle keys. A non-lock key may carry
    // a change made while another window had focus, so it is synced before the
    // key itself - the character then gets the right case; a lock key is synced
    // after it, because that key press toggles the remote on its own.
    const lockKey: boolean = event.keyCode === 2074 || event.keyCode === 2075
      || event.keyCode === 2102;
    if (!lockKey) {
      this.syncLocalLocks(event);
    }
    if (this.winKeySubstitute !== 0 && event.keyCode === this.winKeySubstitute) {
      this.trackScanKey('win', 0x5B, true, down);
      this.native.winKey(down);
      return;
    }
    const scan: RdpScanCode | undefined = KeyMapper.map(event.keyCode);
    if (scan !== undefined) {
      this.trackScanKey('s' + scan.scancode + (scan.extended ? 'e' : ''), scan.scancode,
        scan.extended, down);
      this.native.key(scan.scancode, down, scan.extended);
      if (lockKey) {
        this.syncLocalLocks(event);
      }
      return;
    }
    const text: string = event.keyText;
    if (text.length === 1) {
      const code: number = text.charCodeAt(0);
      if (code >= 0x20) {
        this.trackUnicodeKey(code, down);
        this.native.unicode(code, down);
      }
    }
  }

  /**
   * Reports the local toggle-key state to the remote so the session follows
   * HarmonyOS. The states are only exposed from API 19; when the device does not
   * provide them nothing is known and the remote keeps its own state. The native
   * side drops a value the server was already sent, so this is safe per event.
   */
  private syncLocalLocks(event: KeyEvent): void {
    const caps: boolean | undefined = event.isCapsLockOn;
    const num: boolean | undefined = event.isNumLockOn;
    const scroll: boolean | undefined = event.isScrollLockOn;
    if (caps === undefined || num === undefined || scroll === undefined) {
      return;
    }
    let flags: number = 0;
    if (scroll) {
      flags |= RdpLockFlags.SCROLL;
    }
    if (num) {
      flags |= RdpLockFlags.NUM;
    }
    if (caps) {
      flags |= RdpLockFlags.CAPS;
    }
    this.localLockFlags = flags;
    this.localLockKnown = true;
    this.native.lockState(flags);
  }

  private trackScanKey(key: string, scancode: number, extended: boolean, down: boolean): void {
    if (down) {
      if (!this.heldScanKeys.has(key)) {
        this.heldScanKeys.set(key, RdpScanCode.of(scancode, extended));
      }
    } else {
      this.heldScanKeys.delete(key);
    }
  }

  private trackUnicodeKey(code: number, down: boolean): void {
    if (down) {
      this.heldUnicodeKeys.add(code);
    } else {
      this.heldUnicodeKeys.delete(code);
    }
  }

  private releaseAllKeys(): void {
    this.heldScanKeys.forEach((key: RdpScanCode) => {
      this.native.key(key.scancode, false, key.extended);
    });
    this.heldScanKeys.clear();
    this.heldUnicodeKeys.forEach((code: number) => {
      this.native.unicode(code, false);
    });
    this.heldUnicodeKeys.clear();
  }

  private releaseAllButtons(): void {
    if (this.heldButtons.size === 0) {
      return;
    }
    this.flushMouseMove();
    this.heldButtons.forEach((button: number) => {
      this.native.mouse(button, this.lastPointerX, this.lastPointerY);
    });
    this.heldButtons.clear();
  }

  /** Releases every input the remote still sees as down (focus left the window). */
  private releaseAllInputs(): void {
    this.releaseAllKeys();
    this.releaseAllButtons();
    this.releaseAllTouches();
    this.endPinch();
    // Don't let a pending 中键 candidate fire into a window that lost focus.
    this.clearTouchpadClick();
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
        this.setWindowMinimized(status === window.WindowStatusType.MINIMIZE);
        // The auto-hide only exists in fullscreen: drop or re-arm its timers.
        this.refreshToolbarAutoHide();
      });
      win.on('windowEvent', (data: window.WindowEventType) => {
        // Focus/visibility events are not visibility sources; they only signal
        // that the inputs the remote still sees as held must be let go. Focus
        // also drives 快捷键穿透 (application-wide, so released on blur).
        if (data === window.WindowEventType.WINDOW_ACTIVE) {
          this.windowFocused = true;
          this.updateShortcutPassthrough();
        } else if (data === window.WindowEventType.WINDOW_INACTIVE
          || data === window.WindowEventType.WINDOW_HIDDEN) {
          this.windowFocused = false;
          this.releaseAllInputs();
          this.updateShortcutPassthrough();
        }
      });
      // Only a *fully* covered window reports false here; a partly covered one
      // still reports true (doc_agent/session-and-input.md §1.1).
      try {
        win.on('windowVisibilityChange', (visible: boolean) => {
          this.setWindowOnScreen(visible);
        });
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'on windowVisibilityChange failed: %{public}s',
          JSON.stringify(error));
      }
    }).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'getLastWindow failed: %{public}s', JSON.stringify(error));
    });
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

  /** Recomputes windowVisible and syncs the held surface size + suppression. */
  private syncWindowVisible(): void {
    const visible: boolean = this.windowOnScreen && !this.windowMinimized;
    if (this.windowVisible === visible) {
      return;
    }
    this.windowVisible = visible;
    if (visible) {
      this.flushPendingSurfaceSize();
    }
    this.native.setSuppressed(!visible);
  }

  /** Applies a surface size as the letterbox target; unchanged sizes are dropped. */
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

  /** Re-measures the surface once the window is visible again. */
  private flushPendingSurfaceSize(): void {
    if (!this.pendingSurfaceResize) {
      return;
    }
    this.pendingSurfaceResize = false;
    const rect: SurfaceRect = this.controller.getXComponentSurfaceRect();
    this.applySurfaceSize(rect.surfaceWidth, rect.surfaceHeight);
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
    BackgroundWorker.unregisterSession(this.backgroundSink);
    this.stopAudio();
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
      if (this.state === ConnectionState.Connecting) {
        LoadingProgress().width(44).height(44).color($r('app.color.accent'))
      } else {
        Image($r('app.media.ic_remote_desktop')).width(52).height(52)
          .borderRadius(14)
          .draggable(false)
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
            if (this.state === ConnectionState.Connecting) {
              this.disconnect();
            } else {
              this.retry();
            }
          })
        // The secondary action is optional: "正在连接" only offers 取消, so an
        // empty label must not render as a blank button next to it.
        if (secondary.length > 0) {
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
      }
      .margin({ top: 24 })
    }
    .width(360)
    .padding({ top: 32, bottom: 28, left: 28, right: 28 })
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : $r('app.color.card_bg'))
    .borderRadius(20)
    .shadow({ radius: 30, color: '#33000000', offsetX: 0, offsetY: 10 })
    .attributeModifier(this.statusCardMaterial)
    .alignItems(HorizontalAlign.Center)
  }

  /**
   * Toolbar suffix for 超分辨率: the remote desktop shown next to the host is the
   * session (pre-upscale) size, so the backend and ratio say what it is scaled back
   * up to with (and that the session is not actually running at the configured
   * output size). Short on purpose, e.g. " (FSR 1.5x)".
   */
  private srIndicator(): string {
    if (!this.options.srEnabled || this.options.srRatioPercent <= 100) {
      return '';
    }
    const backend: string = this.options.srBackend === SR_BACKEND_FSR ? 'FSR' : 'XEngine';
    return ' (' + backend + ' ' + srRatioLabel(this.options.srRatioPercent) + ')';
  }

  /**
   * Hairline at the top edge that announces a hidden strip: it fills over the
   * dwell (so the strip drops in when the fill completes) and retracts as soon
   * as the pointer leaves the reveal band.
   */
  @Builder
  toolbarRevealHint() {
    Row()
      .width(REVEAL_HINT_WIDTH * this.revealHintFill)
      .height(REVEAL_HINT_HEIGHT)
      .borderRadius(REVEAL_HINT_HEIGHT / 2)
      .backgroundColor(TOOLBAR_FG)
      .opacity(this.revealHintFill)
      // A hint only: the pointer events the reveal band and the strip depend on
      // must pass straight through it.
      .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  sessionToolbarMainRow() {
    Row() {
      Text(this.options.displayName())
        .fontSize(14)
        .fontColor(TOOLBAR_FG)
      Text('  ' + this.desktopWidth + ' × ' + this.desktopHeight + this.srIndicator())
        .fontSize(12)
        .fontColor(TOOLBAR_FG_SECONDARY)
      Blank()
      // File transfer progress: an inline bar next to the clipboard actions,
      // with a cancel affordance while a download is running.
      // File transfer row: an upload first shows a short "paste on the remote"
      // hint in this same place, then turns into the progress row.
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
          Text('取消')
            .fontSize(12)
            .fontColor(TOOLBAR_FG_SECONDARY)
            .padding({ left: 10, right: 10, top: 4, bottom: 4 })
            .border({ width: 1, color: TOOLBAR_CHIP_BORDER })
            .borderRadius(6)
            .margin({ left: 8 })
            .hoverEffect(HoverEffect.Highlight)
            .onClick(() => {
              this.abortFileTransfer();
            })
        }
        .margin({ right: 12 })
      } else if (this.transferActive) {
        Row() {
          Text(this.transferText)
            .fontSize(12)
            .fontColor(TOOLBAR_FG)
            .maxLines(1)
          if (this.transferFileCount > 1 && this.transferFileIndex > 0) {
            // Multi-file transfers: which file is moving, so the bar is not the
            // only cue that the whole list is progressing.
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
          // Secondary action: outlined rather than filled, a step below the
          // clipboard / window chips.
          Text('取消')
            .fontSize(12)
            .fontColor(TOOLBAR_FG_SECONDARY)
            .padding({ left: 10, right: 10, top: 4, bottom: 4 })
            .border({ width: 1, color: TOOLBAR_CHIP_BORDER })
            .borderRadius(6)
            .margin({ left: 8 })
            .hoverEffect(HoverEffect.Highlight)
            .onClick(() => {
              this.cancelFileTransfer();
            })
        }
        .margin({ right: 12 })
      }
      // Clipboard actions sit left of the window actions, tinted
      // differently and separated from them by a fixed gap.
      Row() {
        Image($r('sys.media.ohos_ic_public_copy'))
          .width(16)
          .height(16)
          .fillColor(TOOLBAR_FG)
          .draggable(false)
        Text('复制')
          .fontSize(13)
          .fontColor(TOOLBAR_FG)
          .margin({ left: 4 })
      }
      .padding({ left: 14, right: 14, top: 7, bottom: 7 })
      .backgroundColor(TOOLBAR_ACCENT_CHIP_BG)
      .borderRadius(8)
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.pullClipboard();
      })
      .bindTips('从远端复制到本机剪贴板', this.tipsOptions)
      // Local clipboard -> remote. Security control: clicking it grants a
      // one-time clipboard read without the READ_PASTEBOARD permission.
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
      Text(this.isFullscreen ? '退出全屏' : '全屏')
        .fontSize(13)
        .fontColor(TOOLBAR_FG)
        .padding({ left: 16, right: 16, top: 7, bottom: 7 })
        .backgroundColor(TOOLBAR_CHIP_BG)
        .borderRadius(8)
        .margin({ right: 8 })
        .hoverEffect(HoverEffect.Highlight)
        .onClick(() => {
          this.toggleFullscreen();
        })
      Text('最小化')
        .fontSize(13)
        .fontColor(TOOLBAR_FG)
        .padding({ left: 16, right: 16, top: 7, bottom: 7 })
        .backgroundColor(TOOLBAR_CHIP_BG)
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
    .height(TOOLBAR_ROW_HEIGHT)
    .padding({ left: 16, right: 16 })
  }

  @Builder
  sessionToolbar() {
    Column() {
      this.sessionToolbarMainRow()
      // Developer telemetry gets its own line so the main row keeps exactly the
      // same layout as a normal (non-developer) session.
      if (this.state === ConnectionState.Connected && this.developerMode) {
        this.toolbarTelemetry()
      }
    }
    .width('100%')
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : TOOLBAR_BG)
    .attributeModifier(this.toolbarMaterial)
    // In fullscreen the strip covers the surface, so only its own hover can
    // report that the pointer is on it.
    .onHover((isHover: boolean) => {
      this.setToolbarPointerZone(isHover ? TOOLBAR_ZONE_BAR : TOOLBAR_ZONE_NONE);
    })
    .transition(TransitionEffect.translate({ y: -this.toolbarHeight })
      .animation({ duration: 180, curve: Curve.EaseOut }))
  }

  /**
   * Developer-only telemetry (network / 本机 / FPS / CPU / GPU / bandwidth /
   * audio), on its own toolbar line. Hidden unless developer options are on.
   *
   * The fixed fields come first; the two that can disappear follow in reverse
   * order of how often they show - bandwidth (any traffic) before audio (only
   * while playing) - so a field that comes and goes never shifts one that shows
   * up more often, and 网络/本机/FPS/CPU/GPU never move at all.
   */
  @Builder
  toolbarTelemetry() {
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
        .bindTips(this.localTips(), this.tipsOptions)
      Text(this.fpsLabel())
        .fontSize(12)
        .fontColor(TOOLBAR_FG_SECONDARY)
        .width(METRIC_WIDTH_FPS)
        .margin({ left: METRIC_GAP_GROUP })
        .maxLines(1)
      Text(this.cpuLabel())
        .fontSize(12)
        .fontColor(TOOLBAR_FG_SECONDARY)
        .width(METRIC_WIDTH_CPU)
        .margin({ left: METRIC_GAP_GROUP })
        .maxLines(1)
      Text(this.gpuLabel())
        .fontSize(12)
        .fontColor(TOOLBAR_FG_SECONDARY)
        .width(METRIC_WIDTH_GPU)
        .margin({ left: METRIC_GAP_GROUP })
        .maxLines(1)
        .bindTips(this.gpuTips(), this.tipsOptions)
      if (this.hasBandwidth()) {
        Text(this.bandwidthLabel())
          .fontSize(12)
          .fontColor(TOOLBAR_FG_SECONDARY)
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
    .width('100%')
    .height(TOOLBAR_TELEMETRY_HEIGHT)
    .padding({ left: 16, right: 16 })
  }

  build() {
    Column() {
      // Windowed mode: the toolbar is always visible and laid out above the
      // remote image, so the picture starts below it instead of under it.
      if (!this.isFullscreen && !this.showOverlay) {
        this.sessionToolbar()
      }
      Stack({ alignContent: Alignment.TopStart }) {
        // The picture lives in its own layer so the direct pinch mode can scale
        // it locally (applyViewZoom); the Stack below clips it, so a magnified
        // picture cannot spill over the toolbar or past the window.
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
              // The system may drop a custom cursor when the pointer leaves and
              // re-enters the window, so re-install the last known shape.
              if (isHover) {
                this.applyCursor();
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
              const width: number = this.lengthToNumber(newArea.width);
              const height: number = this.lengthToNumber(newArea.height);
              if (this.componentWidth !== width || this.componentHeight !== height) {
                // The fitted rect changed under the zoom; re-fit rather than
                // leave a stale transform (also the window-resize case).
                this.resetViewZoom();
              }
              this.componentWidth = width;
              this.componentHeight = height;
            })
        }
        .width('100%')
        .height('100%')
        .transform(this.viewMatrix)

        // Fullscreen: the reveal hint hugs the top edge, behind the strip (which
        // slides in over it).
        if (this.isFullscreen && !this.showOverlay) {
          Stack() {
            this.toolbarRevealHint()
          }
          .width('100%')
          .height('100%')
          .alignContent(Alignment.Top)
          .hitTestBehavior(HitTestMode.None)
        }

        // Fullscreen mode: the toolbar floats over the image and auto-hides.
        if (this.isFullscreen && this.showToolbar && !this.showOverlay) {
          this.sessionToolbar()
        }

      if (this.showOverlay) {
        Column() {
          if (this.state === ConnectionState.Connecting) {
            this.statusCard('正在连接', this.options.targetLabel(), '取消', '')
          } else if (this.state === ConnectionState.Disconnecting) {
            this.statusCard('正在断开', this.statusText, '', '')
          } else if (this.state === ConnectionState.Failed) {
            this.statusCard('连接失败', this.statusText, '重试', '返回')
          } else if (this.state === ConnectionState.Idle) {
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
      .clip(true)
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
