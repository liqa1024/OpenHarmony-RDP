import {
  MoonlightEventExtra, MoonlightSessionConfig, bufferRelease,
  moonlightHighResHScroll, moonlightHighResScroll, moonlightHostFeatures,
  moonlightKey, moonlightMouseButton, moonlightMouseMove,
  moonlightMousePosition, moonlightSessionActive, moonlightSessionClearSurface,
  moonlightSessionSetSurface, moonlightSessionStart, moonlightSessionStop, moonlightSetSessionListener,
  moonlightSetShortcutPassthrough, moonlightText, moonlightTouch
} from 'libhmrdp.so';
import { ML_CURSOR_HIDE_HOST, SettingsStore } from './SettingsStore';
import { RemoteCursorUpdate } from './RemoteCursor';
import { PendingConnection, PROTOCOL_MOONLIGHT, SavedConnection } from '../model/RdpModels';
import { SessionSignal, SessionSignalKind } from '../model/SessionSignal';
import { ConnectionStore } from './ConnectionStore';
import { MoonlightConnectResult, MoonlightConnector, MoonlightHostPolicy,
  MoonlightPinProvider } from './MoonlightConnector';
import { MoonlightHostState } from './MoonlightHostState';
import { PairResult } from '../moonlight/MoonlightModels';
import { SessionBackend, SessionSignalListener } from './SessionBackend';
import { InputAction, InputButton, InputSink, TouchpadContact, TouchPhase } from './SessionInput';
import { KeyMapping } from '../utils/KeyMapper';
import { MlVirtualKey } from '../utils/MoonlightKeyMapper';

export const ML_EVENT_STARTED: number = 0;
export const ML_EVENT_TERMINATED: number = 1;
export const ML_EVENT_STAGE_STARTING: number = 2;
export const ML_EVENT_STAGE_COMPLETE: number = 3;
export const ML_EVENT_STAGE_FAILED: number = 4;
export const ML_EVENT_CURSOR_SHAPE: number = 5;
// Shortcut passthrough: a system-intercepted key, down/up carry the keyCode in value.
export const ML_EVENT_KEY_INTERCEPT_DOWN: number = 6;
export const ML_EVENT_KEY_INTERCEPT_UP: number = 7;

// LiGetHostFeatureFlags bits we consume.
export const ML_HOST_FEATURE_TOUCH: number = 0x01;

export type MlKeyInterceptListener = (keyCode: number, down: boolean) => void;

export type MlCursorListener = (update: RemoteCursorUpdate) => void;

export class MoonlightSessionBackend implements SessionBackend {
  private listener: SessionSignalListener | undefined = undefined;
  private cursorListener: MlCursorListener | undefined = undefined;
  private keyInterceptListener: MlKeyInterceptListener | undefined = undefined;
  private request: PendingConnection;
  private config: MoonlightSessionConfig | undefined = undefined;
  private windowBound: boolean = false;

  constructor(request: PendingConnection) {
    this.request = request;
    // One process-global session at a time (moonlight-common-c is single connection), so the listener is registered here rather than per stream.
    moonlightSetSessionListener((event: number, value: number, text: string,
      extra: MoonlightEventExtra): void => {
      this.onNativeEvent(event, value, text, extra);
    });
  }

  setCursorListener(listener: MlCursorListener): void {
    this.cursorListener = listener;
  }

  clearCursorListener(): void {
    this.cursorListener = undefined;
  }

  setKeyInterceptListener(listener: MlKeyInterceptListener): void {
    this.keyInterceptListener = listener;
  }

  clearKeyInterceptListener(): void {
    this.keyInterceptListener = undefined;
  }

  // Returns whether the requested state is in effect (ACL may refuse).
  setShortcutPassthrough(enabled: boolean): boolean {
    return moonlightSetShortcutPassthrough(enabled);
  }

  private onNativeEvent(event: number, value: number, text: string,
    extra: MoonlightEventExtra): void {
    if (event === ML_EVENT_KEY_INTERCEPT_DOWN || event === ML_EVENT_KEY_INTERCEPT_UP) {
      const listener: MlKeyInterceptListener | undefined = this.keyInterceptListener;
      if (listener !== undefined) {
        listener(value, event === ML_EVENT_KEY_INTERCEPT_DOWN);
      }
      return;
    }
    if (event === ML_EVENT_CURSOR_SHAPE) {
      const handle: number = extra.imageHandle ?? 0;
      const listener: MlCursorListener | undefined = this.cursorListener;
      if (listener === undefined) {
        if (handle > 0) {
          bufferRelease(handle);
        }
        return;
      }
      const update: RemoteCursorUpdate = new RemoteCursorUpdate();
      update.visible = extra.visible ?? true;
      update.hasShape = extra.hasShape ?? false;
      update.shapeKey = extra.shapeId ?? 0;
      update.width = extra.width ?? 0;
      update.height = extra.height ?? 0;
      update.hotX = extra.hotX ?? 0;
      update.hotY = extra.hotY ?? 0;
      update.imageHandle = handle;
      listener(update);
      return;
    }
    this.emit(event, value, text);
  }

  protocol(): number {
    return PROTOCOL_MOONLIGHT;
  }

  requiresPrepare(): boolean {
    return true;
  }

  async prepare(): Promise<boolean> {
    const request: PendingConnection = this.request;
    const conn = request.savedConnection;
    if (conn === undefined || request.filesDir.length === 0) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, '缺少连接参数');
      return false;
    }
    const pinProvider: MoonlightPinProvider =
      request.moonlightPinProvider !== undefined
        ? request.moonlightPinProvider
        : (pin: string, pairing: Promise<PairResult>): Promise<boolean> => Promise.resolve(false);
    const policy: MoonlightHostPolicy = new MoonlightHostPolicy();
    policy.expectedId = conn.mlHostId;
    const connections: SavedConnection[] = ConnectionStore.list();
    for (let i = 0; i < connections.length; i++) {
      const other: SavedConnection = connections[i];
      if (other.isMoonlight() && other.id !== conn.id && other.mlHostId.length > 0) {
        policy.knownIds.push(other.mlHostId);
      }
    }
    const result: MoonlightConnectResult = await MoonlightConnector.prepare(request.filesDir,
      conn.host, conn.port, request.stream, request.moonlightAppId,
      request.stream.width, request.stream.height, pinProvider, policy);
    if (!result.ok) {
      MoonlightHostState.refresh(conn, request.filesDir);
      this.emit(ML_EVENT_STAGE_FAILED, 0, result.error);
      return false;
    }
    this.config = result.config;
    request.moonlightConfig = result.config;
    request.moonlightHttpsPort = result.httpsPort;
    MoonlightHostState.markLaunched(conn.id, result.effectiveAppId);
    if (result.hostId.length > 0 && conn.mlHostId !== result.hostId) {
      conn.mlHostId = result.hostId;
      conn.updatedAt = Date.now();
      ConnectionStore.save(conn);
    }
    return true;
  }

  requiresWindowAtStart(): boolean {
    return true;
  }

  handleId(): number {
    return 0;
  }

  bindSurface(surfaceId: string, width: number, height: number): boolean {
    this.windowBound = moonlightSessionSetSurface(surfaceId, width, height);
    return this.windowBound;
  }

  clearSurface(): void {
    if (this.windowBound) {
      moonlightSessionClearSurface();
      this.windowBound = false;
    }
  }

  start(): number {
    const config: MoonlightSessionConfig | undefined = this.config;
    if (config === undefined) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, '尚未配置串流参数');
      return -1;
    }
    if (!this.windowBound) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, '会话窗口未就绪');
      return -1;
    }
    // HIDE_HOST is a no-op unless the host is a foundation-sunshine build.
    config.localCursorMode = SettingsStore.load().mlCursorMode === ML_CURSOR_HIDE_HOST;
    const error: string = moonlightSessionStart(config);
    if (error.length > 0) {
      this.emit(ML_EVENT_STAGE_FAILED, 0, `无法启动串流（${error}）`);
      return -1;
    }
    return 1;
  }

  stop(): void {
    moonlightSessionStop();
    this.clearSurface();
    this.clearSignalListener();
  }

  active(): boolean {
    return moonlightSessionActive();
  }

  // LI_FF_* bits the host advertised; 0 until the streams are established.
  hostFeatureFlags(): number {
    return moonlightHostFeatures();
  }

  setSignalListener(listener: SessionSignalListener): void {
    this.listener = listener;
  }

  clearSignalListener(): void {
    this.listener = undefined;
  }

  pointer(flags: number, x: number, y: number): void {
  }

  touch(flags: number, finger: number, pressure: number, x: number, y: number): void {
  }

  key(scancode: number, down: boolean, extended: boolean): void {
  }

  unicode(codepoint: number, down: boolean): void {
  }

  mouseMove(deltaX: number, deltaY: number): void {
    moonlightMouseMove(deltaX, deltaY);
  }

  mousePosition(x: number, y: number, referenceWidth: number, referenceHeight: number): void {
    moonlightMousePosition(x, y, referenceWidth, referenceHeight);
  }

  mouseButton(action: number, button: number): void {
    moonlightMouseButton(action, button);
  }

  // Moonlight takes Windows virtual-key codes, not PS/2 scancodes.
  keyVirtual(vk: number, down: boolean, modifiers: number): void {
    moonlightKey(vk, down ? 1 : 0, modifiers);
  }

  private emit(event: number, value: number, text: string): void {
    const listener: SessionSignalListener | undefined = this.listener;
    if (listener === undefined) {
      return;
    }
    const signal: SessionSignal = new SessionSignal();
    signal.eventCode = event;
    signal.code = value;
    signal.message = text;
    if (event === ML_EVENT_STARTED) {
      signal.kind = SessionSignalKind.Connected;
    } else if (event === ML_EVENT_TERMINATED) {
      signal.kind = SessionSignalKind.Disconnected;
    } else if (event === ML_EVENT_STAGE_FAILED) {
      signal.kind = SessionSignalKind.Error;
    } else if (event === ML_EVENT_STAGE_STARTING || event === ML_EVENT_STAGE_COMPLETE) {
      signal.kind = SessionSignalKind.UiEvent;
    } else {
      signal.kind = SessionSignalKind.UiEvent;
    }
    listener(signal);
  }
}

// Windows VK for the Meta/Win key.
const ML_VK_META: number = 0x5B;

export class MoonlightInputSink implements InputSink {
  private backend: MoonlightSessionBackend;
  // Left/right modifiers share a bit but are distinct VKs, so the byte is derived
  // from the set of keys currently down, not from a per-key toggle.
  private heldModifiers: Set<number> = new Set<number>();
  private referenceWidth: number = 1920;
  private referenceHeight: number = 1080;

  constructor(backend: MoonlightSessionBackend) {
    this.backend = backend;
  }

  setReferenceSize(width: number, height: number): void {
    this.referenceWidth = width;
    this.referenceHeight = height;
  }

  key(mapping: KeyMapping, down: boolean): void {
    // 0 means this key has no Windows VK on the Moonlight side.
    if (mapping.vk <= 0) {
      return;
    }
    this.trackModifier(mapping.vk, down);
    let modifiers: number = this.modifierByte();
    if (mapping.extended) {
      modifiers |= MlVirtualKey.MODIFIER_EXTENDED;
    }
    this.backend.keyVirtual(mapping.vk, down, modifiers);
  }

  metaKey(down: boolean): void {
    this.trackModifier(ML_VK_META, down);
    this.backend.keyVirtual(ML_VK_META, down, this.modifierByte() | MlVirtualKey.MODIFIER_EXTENDED);
  }

  private trackModifier(vk: number, down: boolean): void {
    if (MlVirtualKey.modifierFor(vk) === 0) {
      return;
    }
    if (down) {
      this.heldModifiers.add(vk);
    } else {
      this.heldModifiers.delete(vk);
    }
  }

  private modifierByte(): number {
    let modifiers: number = 0;
    this.heldModifiers.forEach((vk: number) => {
      modifiers |= MlVirtualKey.modifierFor(vk);
    });
    return modifiers;
  }

  unicode(code: number, down: boolean): void {
    if (down && code > 0) {
      moonlightText(String.fromCharCode(code));
    }
  }

  lockState(_flags: number): void {
    // No lock-state PDU on Moonlight; the host tracks its own lock state.
  }

  pointerMove(x: number, y: number): void {
    this.backend.mousePosition(x, y, this.referenceWidth, this.referenceHeight);
  }

  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): void {
    const normalizedX: number = this.referenceWidth > 0 ? x / this.referenceWidth : 0;
    const normalizedY: number = this.referenceHeight > 0 ? y / this.referenceHeight : 0;
    moonlightTouch(MoonlightInputSink.mlTouchEvent(phase), pointerId, normalizedX, normalizedY,
      pressure);
  }

  scroll(vertical: number, horizontal: number, _x: number, _y: number): void {
    if (vertical !== 0) {
      moonlightHighResScroll(MoonlightInputSink.clampShort(vertical));
    }
    if (horizontal !== 0) {
      moonlightHighResHScroll(MoonlightInputSink.clampShort(horizontal));
    }
  }

  touchpadFrame(_contacts: TouchpadContact[], _buttonState: number, _widthMm: number,
    _heightMm: number): void {
    // TODO(native touchpad): send a precision-touchpad frame once the vendored
    // moonlight-common-c is patched with the foundation-sunshine SS_TOUCHPAD extension.
  }

  button(action: number, button: number, x: number, y: number): void {
    const mlButton: number = MoonlightInputSink.mlButton(button);
    if (mlButton === 0) {
      return;
    }
    this.backend.mousePosition(x, y, this.referenceWidth, this.referenceHeight);
    const mlAction: number = action === InputAction.PRESS
      ? MlVirtualKey.ACTION_PRESS : MlVirtualKey.ACTION_RELEASE;
    this.backend.mouseButton(mlAction, mlButton);
  }

  private static mlButton(button: number): number {
    if (button === InputButton.LEFT) {
      return MlVirtualKey.BUTTON_LEFT;
    }
    if (button === InputButton.MIDDLE) {
      return MlVirtualKey.BUTTON_MIDDLE;
    }
    if (button === InputButton.RIGHT) {
      return MlVirtualKey.BUTTON_RIGHT;
    }
    if (button === InputButton.X1) {
      return MlVirtualKey.BUTTON_X1;
    }
    if (button === InputButton.X2) {
      return MlVirtualKey.BUTTON_X2;
    }
    return 0;
  }

  private static mlTouchEvent(phase: number): number {
    if (phase === TouchPhase.DOWN) {
      return 0x01; // LI_TOUCH_EVENT_DOWN
    }
    if (phase === TouchPhase.UP) {
      return 0x02; // LI_TOUCH_EVENT_UP
    }
    if (phase === TouchPhase.CANCEL) {
      return 0x04; // LI_TOUCH_EVENT_CANCEL
    }
    return 0x03; // LI_TOUCH_EVENT_MOVE
  }

  private static clampShort(value: number): number {
    return Math.max(-32768, Math.min(32767, Math.round(value)));
  }
}
