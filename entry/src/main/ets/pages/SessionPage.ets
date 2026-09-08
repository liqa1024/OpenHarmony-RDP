/*
 * HmRdp - remote session page: XComponent surface + input forwarding.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PendingConnection, RdpConnectOptions, RdpConnectOrigin, RdpConnectionState,
  RdpEvent, RdpPointerFlags, RdpTouchFlags } from '../model/RdpModels';
import { CredentialStore } from '../services/CredentialStore';
import { RdpNative } from '../services/RdpNative';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
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

@Entry
@Component
struct SessionPage {
  @State state: RdpConnectionState = RdpConnectionState.Connecting;
  @State statusText: string = '正在连接...';
  @State showOverlay: boolean = true;
  @State showToolbar: boolean = true;

  private native: RdpNative = RdpNative.get();
  private options: RdpConnectOptions = PendingConnection.current.options;
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

  aboutToAppear(): void {
    const settings: AppSettings = SettingsStore.load();
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.desktopWidth = this.options.width;
    this.desktopHeight = this.options.height;
    this.native.setListener((event: RdpEvent, data: string) => {
      this.onRdpEvent(event, data);
    });
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
    this.startSession();
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
    this.native.stopSession();
    this.native.clearListener();
    PendingConnection.current.options.password = '';
    PendingConnection.current.options.gatewayPassword = '';
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
    this.startSession();
    if (this.surfaceId.length > 0) {
      this.native.bindSurface(this.surfaceId, this.surfaceWidth, this.surfaceHeight);
    }
  }

  private onRdpEvent(event: RdpEvent, data: string): void {
    if (event === RdpEvent.Connected) {
      this.state = RdpConnectionState.Connected;
      this.statusText = '已连接';
      this.showToolbar = false;
      this.handleConnected();
    } else if (event === RdpEvent.FirstFrame) {
      this.showOverlay = false;
      this.showToolbar = false;
    } else if (event === RdpEvent.Disconnected) {
      this.state = RdpConnectionState.Closed;
      this.statusText = data.length > 0 ? data : '连接已断开';
      this.showOverlay = true;
      this.showToolbar = true;
    } else if (event === RdpEvent.Error) {
      this.state = RdpConnectionState.Failed;
      this.statusText = data.length > 0 ? data : '连接失败';
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
    }
  }

  /**
   * Post-connect handling only applies to sessions launched from the editor:
   * the verified password is persisted and the editor returns to the list.
   * Sessions launched from the home list must not trigger any extra action.
   */
  private handleConnected(): void {
    if (PendingConnection.current.origin !== RdpConnectOrigin.Editor) {
      return;
    }
    this.persistPassword();
    const callback: (() => void) | undefined = PendingConnection.current.onConnected;
    PendingConnection.current.onConnected = undefined;
    if (callback !== undefined) {
      callback();
    }
  }

  private persistPassword(): void {
    const connectionId: string = PendingConnection.current.connectionId;
    const password: string = this.options.password;
    if (connectionId.length === 0 || password.length === 0) {
      return;
    }
    CredentialStore.savePassword(connectionId, password).catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'save password failed: %{public}s', JSON.stringify(error));
    });
  }

  private updateToolbar(y: number): void {
    if (this.state !== RdpConnectionState.Connected) {
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
    if (event.action !== AxisAction.BEGIN && event.action !== AxisAction.UPDATE) {
      return;
    }
    const vertical: number = event.axisVertical ?? 0;
    if (vertical === 0) {
      return;
    }
    const point: number[] = this.mapPointer(event.x, event.y);
    let flags: number = RdpPointerFlags.WHEEL;
    if (vertical < 0) {
      flags = flags | 0x78;
    } else {
      flags = flags | RdpPointerFlags.WHEEL_NEGATIVE | 0x88;
    }
    this.native.mouse(flags, point[0], point[1]);
  }

  private handleKey(event: KeyEvent): void {
    const down: boolean = event.type === KeyType.Down;
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

  private disconnect(): void {
    this.native.stopSession();
    const context = this.getUIContext().getHostContext();
    if (context !== undefined) {
      const abilityContext: common.UIAbilityContext = context as common.UIAbilityContext;
      abilityContext.terminateSelf().catch((error: BusinessError) => {
        hilog.error(DOMAIN, TAG, 'terminate session failed: %{public}s', JSON.stringify(error));
      });
    }
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

  build() {
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

      if (this.showToolbar && !this.showOverlay) {
        Row() {
          Text(this.options.host)
            .fontSize(14)
            .fontColor('#FFFFFF')
          Text('  ' + this.desktopWidth + ' × ' + this.desktopHeight)
            .fontSize(12)
            .fontColor('#B8BCC4')
          Blank()
          Text('断开')
            .fontSize(13)
            .fontColor('#FFFFFF')
            .padding({ left: 16, right: 16, top: 7, bottom: 7 })
            .backgroundColor('#33FFFFFF')
            .borderRadius(8)
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
    .height('100%')
    .backgroundColor('#000000')
  }
}
