/*
 * HmRdp - remote session page: XComponent surface + input forwarding.
 */
import { common } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { BusinessError, pasteboard } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PendingConnection, RdpConnectOptions, RdpConnectionState,
  RdpEvent, RdpPointerFlags, RdpTouchFlags, SessionRequests } from '../model/RdpModels';
import { RdpNative } from '../services/RdpNative';
import { SessionManager } from '../services/SessionManager';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
import { WindowController } from '../services/WindowController';
import { KeyMapper, RdpScanCode } from '../utils/KeyMapper';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpSession';

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
  // Touchpad scroll accumulators: axis values are displacement, not notches.
  private touchpadScrollX: number = 0;
  private touchpadScrollY: number = 0;
  // Touchpad pinch state: axisPinch is a cumulative scale ratio (starts at 1).
  private pinchScale: number = 0;
  private pinchAccum: number = 0;
  private pinchActive: boolean = false;
  private pinchCtrlDown: boolean = false;
  private pinchTimer: number = -1;
  private touchpadScrollUnit: number = 40;
  private pinchZoomStepPercent: number = 5;
  private readonly pinchSensitivity: number = 2;
  private readonly ctrlScancode: number = 0x1D;
  private sessionWindow: window.Window | undefined = undefined;
  // Clipboard redirection is manual, driven by the toolbar. The remote
  // clipboard is cached when the server offers it; "复制" pulls it to the local
  // clipboard and "粘贴" (system paste control, no READ_PASTEBOARD) pushes the
  // local clipboard to the remote.
  private clipboardEnabled: boolean = true;
  private remoteClipboardText: string = '';
  private pasteboardBusy: boolean = false;

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
    this.touchpadScrollUnit = settings.touchpadScrollUnit;
    this.pinchZoomStepPercent = settings.pinchZoomStepPercent;
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
    } else if (event === RdpEvent.Error) {
      this.state = RdpConnectionState.Failed;
      this.statusText = data.length > 0 ? SessionManager.describeError(data) : '连接失败';
      this.showOverlay = true;
      this.showToolbar = true;
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
    }
  }

  /** Local clipboard -> remote, invoked by the toolbar's system paste button. */
  private submitClipboard(): void {
    pasteboard.getSystemPasteboard().getData()
      .then((data: pasteboard.PasteData) => {
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
        this.toast('已提交到远端');
      })
      .catch((error: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'read pasteboard failed: %{public}s', JSON.stringify(error));
        this.toast('读取本机剪贴板失败');
      });
  }

  /** Remote clipboard -> local, invoked by the toolbar's pull button. */
  private pullClipboard(): void {
    if (this.remoteClipboardText.length === 0) {
      this.toast('远端剪贴板为空');
      return;
    }
    if (this.pasteboardBusy) {
      return;
    }
    this.pasteboardBusy = true;
    const data: pasteboard.PasteData =
      pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.remoteClipboardText);
    pasteboard.getSystemPasteboard().setData(data)
      .then(() => {
        this.pasteboardBusy = false;
        this.toast('已拉取到本机');
      })
      .catch((error: BusinessError) => {
        this.pasteboardBusy = false;
        hilog.warn(DOMAIN, TAG, 'write pasteboard failed: %{public}s', JSON.stringify(error));
        this.toast('写入本机剪贴板失败');
      });
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
      // FreeRDP reserves contact id 0 for "empty slot", so shift ArkUI's
      // 0-based finger ids into a non-zero range.
      const rdpId: number = touch.id + 1;
      const point: number[] = this.mapPointer(touch.x, touch.y);
      const type: TouchType = touch.type;
      if (type === TouchType.Down) {
        // A down for an already-active contact means the previous sequence was
        // lost; close it first so RDPEI stays well formed.
        if (this.touchPoints.has(touch.id)) {
          this.native.touch(RdpTouchFlags.UP, rdpId, 0, point[0], point[1]);
        }
        this.touchPoints.set(touch.id, point);
        this.native.touch(RdpTouchFlags.DOWN, rdpId, 0, point[0], point[1]);
      } else if (type === TouchType.Move) {
        const last: number[] | undefined = this.touchPoints.get(touch.id);
        // Drop moves for unknown contacts and moves that did not change the
        // remote pixel; RDPEI would drop the former and the latter only adds
        // redundant frames.
        if (last === undefined) {
          continue;
        }
        if (last[0] === point[0] && last[1] === point[1]) {
          continue;
        }
        this.touchPoints.set(touch.id, point);
        this.native.touch(RdpTouchFlags.MOTION, rdpId, 0, point[0], point[1]);
      } else if (type === TouchType.Up || type === TouchType.Cancel) {
        if (!this.touchPoints.has(touch.id)) {
          continue;
        }
        this.touchPoints.delete(touch.id);
        this.native.touch(RdpTouchFlags.UP, rdpId, 0, point[0], point[1]);
      }
    }
    if (event.type === TouchType.Cancel && event.touches.length === 0) {
      this.releaseAllTouches();
    }
  }

  private releaseAllTouches(): void {
    this.touchPoints.forEach((point: number[], id: number) => {
      this.native.touch(RdpTouchFlags.UP, id + 1, 0, point[0], point[1]);
    });
    this.touchPoints.clear();
  }

  private handleAxis(event: AxisEvent): void {
    const isPinch: boolean = this.isPinchEvent(event);
    if (isPinch || this.pinchActive) {
      // Ctrl is held for the whole gesture and never toggled between notches.
      if (event.action === AxisAction.END) {
        this.endPinch();
        return;
      }
      if (isPinch) {
        this.updatePinch(event);
      }
      // Scroll axes produced by the same two fingers are suppressed while the
      // pinch is active, so zoom never mixes with scroll.
      this.armPinchEnd();
      return;
    }
    if (event.action !== AxisAction.BEGIN && event.action !== AxisAction.UPDATE) {
      return;
    }
    if (event.action === AxisAction.BEGIN) {
      this.touchpadScrollX = 0;
      this.touchpadScrollY = 0;
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
    if (!this.pinchActive) {
      this.pinchActive = true;
      this.pinchScale = scale > 0 ? scale : 1.0;
      this.pinchAccum = 0;
      this.setPinchCtrl(true);
      return;
    }
    if (scale <= 0) {
      return;
    }
    this.pinchAccum += Math.log(scale / this.pinchScale);
    this.pinchScale = scale;
    // Sensitivity multiplier makes a pinch trigger sooner without changing the
    // configured scale-to-notch ratio.
    const step: number =
      Math.log(1 + this.pinchZoomStepPercent / 100) / this.pinchSensitivity;
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

  private setPinchCtrl(down: boolean): void {
    if (this.pinchCtrlDown === down) {
      return;
    }
    this.pinchCtrlDown = down;
    this.native.key(this.ctrlScancode, down, false);
  }

  private endPinch(): void {
    if (this.pinchTimer !== -1) {
      clearTimeout(this.pinchTimer);
      this.pinchTimer = -1;
    }
    this.pinchActive = false;
    this.pinchScale = 0;
    this.pinchAccum = 0;
    // Release unconditionally: the final event may not carry the pinch axis, so
    // pinchActive can already be false while Ctrl is still logically held.
    this.setPinchCtrl(false);
  }

  private armPinchEnd(): void {
    if (this.pinchTimer !== -1) {
      clearTimeout(this.pinchTimer);
    }
    // Safety net only; the normal end is AxisAction.END. Long enough that a
    // brief pause while reversing zoom direction does not release Ctrl.
    this.pinchTimer = setTimeout(() => {
      this.pinchTimer = -1;
      this.endPinch();
    }, 1000);
  }

  private handleTouchpadScroll(vertical: number, horizontal: number, point: number[]): void {
    // Natural scrolling: swipe up scrolls the remote content up (WHEEL negative).
    this.touchpadScrollY -= vertical;
    while (Math.abs(this.touchpadScrollY) >= this.touchpadScrollUnit) {
      const up: boolean = this.touchpadScrollY > 0;
      this.touchpadScrollY += up ? -this.touchpadScrollUnit : this.touchpadScrollUnit;
      this.sendWheel(up, point);
    }
    // Natural scrolling: swipe left scrolls the remote content right (HWHEEL positive).
    this.touchpadScrollX -= horizontal;
    while (Math.abs(this.touchpadScrollX) >= this.touchpadScrollUnit) {
      const right: boolean = this.touchpadScrollX < 0;
      this.touchpadScrollX += right ? this.touchpadScrollUnit : -this.touchpadScrollUnit;
      this.sendHorizontalWheel(right, point);
    }
  }

  private sendWheel(up: boolean, point: number[]): void {
    this.native.mouse(up
      ? RdpPointerFlags.WHEEL | 0x78
      : RdpPointerFlags.WHEEL | RdpPointerFlags.WHEEL_NEGATIVE | 0x88,
      point[0], point[1]);
  }

  private sendHorizontalWheel(right: boolean, point: number[]): void {
    this.native.mouse(right
      ? RdpPointerFlags.HWHEEL | 0x78
      : RdpPointerFlags.HWHEEL | RdpPointerFlags.WHEEL_NEGATIVE | 0x88,
      point[0], point[1]);
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
        .onClick(() => {
          this.minimizeWindow();
        })
      Text('断开')
        .fontSize(13)
        .fontColor('#FFFFFF')
        .padding({ left: 16, right: 16, top: 7, bottom: 7 })
        .backgroundColor($r('app.color.danger'))
        .borderRadius(8)
        .onClick(() => {
          this.disconnect();
        })
    }
    .width('100%')
    .height(48)
    .padding({ left: 16, right: 16 })
    .backgroundColor('#CC1A1A1A')
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
