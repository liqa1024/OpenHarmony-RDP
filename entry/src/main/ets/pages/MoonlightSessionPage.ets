/*
 * HmRdp - Moonlight (GameStream) session window.
 *
 * The protocol-specific sibling of SessionPage: it binds the XComponent surface
 * and only then asks SessionManager to start the native Moonlight session
 * (LiStartConnection takes the render target, so the window must exist first),
 * forwards input through the MoonlightSessionBackend's concrete verbs, and
 * renders the session state published by the manager. See
 * doc_agent/moonlight-native.md §3 and session-and-input.md §1.
 */
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { PendingConnection, SessionRequests } from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { ConnectionState, SessionManager } from '../services/SessionManager';
import { SessionBackend, SessionBackendRegistry } from '../services/SessionBackend';
import {
  ML_EVENT_STAGE_COMPLETE, ML_EVENT_STAGE_STARTING, MoonlightSessionBackend
} from '../services/MoonlightSessionBackend';
import { MlVirtualKey } from '../utils/MoonlightKeyMapper';
import { MoonlightClient } from '../moonlight/MoonlightClient';
import { WindowController } from '../services/WindowController';
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';
import { StatusCard } from '../components/StatusCard';
import { TOOLBAR_BG, TOOLBAR_FG, TOOLBAR_ROW_HEIGHT, ToolbarChip } from '../components/SessionChrome';

const DOMAIN: number = 0xD004;
const TAG: string = 'HmRdpMoonlightPage';

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
  // Component size in vp (for mapping pointer coords to the stream frame).
  private componentWidth: number = 0;
  private componentHeight: number = 0;
  // The reference frame absolute mouse positions are expressed in. Until the
  // control layer supplies the negotiated stream size, the 1080p default is a
  // best effort (the host rescales the reference frame anyway).
  private streamWidth: number = 1920;
  private streamHeight: number = 1080;
  // Modifier state tracked from key events (Moonlight wants a modifier bitmask).
  private modifiers: number = 0;
  // Session name shown on the toolbar (the saved connection's label).
  private hostLabel: string = '';
  // Fixed-dark toolbar material (see component/SessionChrome); a no-op on a
  // system without the material capability, which keeps the flat bar colour.
  private toolbarMaterial: AttributeModifier<RowAttribute> =
    new ImmersiveModifier<RowAttribute>(SystemMaterial.of(MaterialSurface.FLOATING));

  @State state: ConnectionState = ConnectionState.Connecting;
  @State statusText: string = '正在连接…';
  @State stageText: string = '';

  aboutToAppear(): void {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    if (request !== undefined) {
      this.connectionId = request.connectionId;
      if (request.savedConnection !== undefined) {
        this.hostLabel = request.savedConnection.label();
      }
    }
    const backend: SessionBackend | undefined = SessionBackendRegistry.find(this.sessionKey);
    if (backend instanceof MoonlightSessionBackend) {
      this.backend = backend;
    }
    this.controller.setCallbacks(
      (surfaceId: string, width: number, height: number): void => {
        this.onSurfaceCreated(surfaceId, width, height);
      },
      (surfaceId: string, width: number, height: number): void => {
        // The surface id is stable across resizes; the bound OHNativeWindow
        // stays valid, so nothing to redo here (onSurfaceCreated rebinds).
        hilog.info(DOMAIN, TAG, 'surface changed %{public}dx%{public}d', width, height);
      },
      (surfaceId: string): void => {
        this.backend?.clearSurface();
      });
    SessionManager.attachWindow(this.sessionKey,
      (state: ConnectionState, message: string): void => {
        this.state = state;
        this.statusText = message;
      },
      (signal: SessionSignal): void => {
        this.onSignal(signal);
      },
      () => {
        this.terminateWindow();
      });
  }

  /** Binds the surface and starts the stream (once). */
  private onSurfaceCreated(surfaceId: string, width: number, height: number): void {
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
    if (signal.eventCode === ML_EVENT_STAGE_STARTING || signal.eventCode === ML_EVENT_STAGE_COMPLETE) {
      this.stageText = signal.message;
    }
  }

  private lengthToNumber(length: Length): number {
    if (typeof length === 'number') {
      return length;
    }
    if (typeof length === 'string') {
      const value: number = Number.parseFloat(length);
      return Number.isNaN(value) ? 0 : value;
    }
    return 0;
  }

  private handleMouse(event: MouseEvent): void {
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
    if (this.connectionId.length > 0) {
      SessionManager.retry(this.connectionId);
    }
  }

  private disconnect(): void {
    if (this.connectionId.length > 0) {
      SessionManager.disconnect(this.connectionId);
      return;
    }
    this.terminateWindow();
  }

  /** The second disconnect mode: stop the host app (/cancel), then disconnect. */
  private async closeAppAndDisconnect(): Promise<void> {
    const request: PendingConnection | undefined = SessionRequests.get(this.sessionKey);
    const context = this.getUIContext().getHostContext();
    if (request !== undefined && request.savedConnection !== undefined && context !== undefined) {
      const filesDir: string = (context as common.UIAbilityContext).filesDir;
      await MoonlightClient.cancel(filesDir, request.savedConnection.host,
        request.moonlightHttpsPort);
    }
    this.disconnect();
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
    if (WindowController.closeSession(abilityContext)) {
      return;
    }
    abilityContext.terminateSelf().catch((error: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'terminate session failed: %{public}s', JSON.stringify(error));
    });
  }

  build() {
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

      if (this.state === ConnectionState.Connected) {
        // Session toolbar in the same fixed-dark chrome as the RDP page; the
        // full auto-hide / telemetry surface is a later milestone.
        Row() {
          Text(this.hostLabel)
            .fontSize(14)
            .fontColor(TOOLBAR_FG)
          Blank()
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
      }

      if (this.state === ConnectionState.Connecting || this.state === ConnectionState.Disconnecting
        || this.state === ConnectionState.Failed || this.state === ConnectionState.Idle) {
        Column() {
          if (this.state === ConnectionState.Connecting) {
            StatusCard({
              title: '正在连接',
              subtitle: this.stageText.length > 0 ? this.stageText : this.statusText,
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
    .height('100%')
    .backgroundColor('#000000')
  }
}
