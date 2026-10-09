import { KeyMapping, KeyMapper } from '../utils/KeyMapper';
import { RdpLockFlags } from '../model/RdpModels';

// Abstract pointer button ids shared by both protocols.
export class InputButton {
  static readonly LEFT: number = 1;
  static readonly MIDDLE: number = 2;
  static readonly RIGHT: number = 3;
  static readonly X1: number = 4;
  static readonly X2: number = 5;
}

export class InputAction {
  static readonly PRESS: number = 1;
  static readonly RELEASE: number = 2;
}

// Protocol-neutral touch transition; each sink maps it to its own event constants.
export class TouchPhase {
  static readonly DOWN: number = 1;
  static readonly UP: number = 2;
  static readonly MOVE: number = 3;
  static readonly CANCEL: number = 4;
  static readonly CANCEL_ALL: number = 5;
}

// Protocol-facing verbs; each session backend implements this over its own transport.
export interface InputSink {
  // mapping carries both scancode (RDP) and vk (Moonlight); extended is shared.
  key(mapping: KeyMapping, down: boolean): void;
  // Windows/Meta key, used by the Win-key substitute.
  metaKey(down: boolean): void;
  // A composed character's code point (IME / layout fallback).
  unicode(code: number, down: boolean): void;
  // Mirror the local lock state; RDP sends Client Synchronize, Moonlight is a no-op.
  lockState(flags: number): void;
  // Absolute pointer position in the pointer space set by the page.
  pointerMove(x: number, y: number): void;
  // One button transition at the given position.
  button(action: number, button: number, x: number, y: number): void;
  // One touch transition; x/y are in the pointer space, pressure is 0..1 (0 = unknown).
  touch(phase: number, pointerId: number, x: number, y: number, pressure: number): void;
}

// Each forwarded move is a separate input message, so absolute moves are rate-limited.
const MOUSE_MOVE_MIN_INTERVAL_MS: number = 8;

// A TouchType.Cancel does not always mean the finger lifted: a Down within this distance of its last position resumes the contact instead of lifting and re-pressing.
const CANCEL_RESUME_RATIO: number = 0.02;
const CANCEL_RESUME_MIN_DISTANCE: number = 16;
const CANCEL_RESUME_MAX_DISTANCE: number = 64;
const CANCEL_HOLD_MS: number = 400;

// Protocol-neutral input pipeline: source classification, key/button accounting,
// move coalescing, lock ordering, touch contacts and release-all. Touchpad/axis
// landing on top of the same state.
export class SessionInput {
  private sink: InputSink;
  private winKeySubstitute: number = 0;
  private interceptActive: boolean = false;
  private heldKeys: Map<string, KeyMapping> = new Map<string, KeyMapping>();
  private heldMeta: boolean = false;
  private heldButtons: Set<number> = new Set<number>();
  private lastPointerX: number = 0;
  private lastPointerY: number = 0;
  private pendingMoveX: number = -1;
  private pendingMoveY: number = -1;
  private lastMoveSentAt: number = 0;
  private moveTimer: number = -1;
  // Pointer space (remote desktop/stream pixels) used for touch clamping and cancel resume.
  private pointerWidth: number = 0;
  private pointerHeight: number = 0;
  // Touchscreen-as-native-touch is only enabled when the host advertises it.
  private touchEnabled: boolean = false;
  private touchPoints: Map<number, number[]> = new Map<number, number[]>();
  // Kept separate from touchPoints so a contact can be resumed after a Cancel even if the UI renumbers the finger.
  private touchContactIds: Map<number, number> = new Map<number, number>();
  private pendingCancelIds: number[] = [];
  private cancelTimers: Map<number, number> = new Map<number, number>();

  constructor(sink: InputSink) {
    this.sink = sink;
  }

  setWinKeySubstitute(keyCode: number): void {
    this.winKeySubstitute = keyCode;
  }

  setPointerSpace(width: number, height: number): void {
    this.pointerWidth = width;
    this.pointerHeight = height;
  }

  setTouchEnabled(enabled: boolean): void {
    this.touchEnabled = enabled;
  }

  // True while touchscreen input is routed to native host touch, so the same
  // touch must not also be processed as its compatibility mouse events.
  isTouchEnabled(): boolean {
    return this.touchEnabled;
  }

  // While a system key interceptor owns the keyboard, onKeyEvent must not also forward.
  setInterceptActive(active: boolean): void {
    this.interceptActive = active;
  }

  handleKey(event: KeyEvent): void {
    if (this.interceptActive) {
      return;
    }
    const down: boolean = event.type === KeyType.Down;
    // The real Win key is owned by the system; Win for the remote comes from the substitute key.
    if (event.keyCode === 2076 || event.keyCode === 2077) {
      return;
    }
    const mapping: KeyMapping | undefined = KeyMapper.mapFull(event.keyCode);
    const lockKey: boolean = mapping !== undefined && mapping.lock !== 0;
    // A non-lock key is synced before itself so the character gets the right case; a lock key after, since its press toggles the remote.
    if (!lockKey) {
      this.syncLocalLocks(event);
    }
    if (this.winKeySubstitute !== 0 && event.keyCode === this.winKeySubstitute) {
      this.trackMeta(down);
      this.sink.metaKey(down);
      return;
    }
    if (mapping !== undefined) {
      this.trackKey(event.keyCode, mapping, down);
      this.sink.key(mapping, down);
      if (lockKey) {
        this.syncLocalLocks(event);
      }
      return;
    }
    const text: string = event.keyText;
    if (text.length === 1) {
      const code: number = text.charCodeAt(0);
      if (code >= 0x20) {
        this.sink.unicode(code, down);
      }
    }
  }

  handleMove(x: number, y: number): void {
    this.lastPointerX = x;
    this.lastPointerY = y;
    this.pendingMoveX = x;
    this.pendingMoveY = y;
    const elapsed: number = Date.now() - this.lastMoveSentAt;
    if (elapsed >= MOUSE_MOVE_MIN_INTERVAL_MS) {
      this.flushMove();
    } else if (this.moveTimer === -1) {
      this.moveTimer = setTimeout(() => {
        this.moveTimer = -1;
        this.flushMove();
      }, MOUSE_MOVE_MIN_INTERVAL_MS - elapsed);
    }
  }

  handleButton(action: number, button: number, x: number, y: number): void {
    this.lastPointerX = x;
    this.lastPointerY = y;
    // Any button event flushes the pending move first, or the click lands on a stale position.
    this.flushMove();
    this.sink.button(action, button, x, y);
    if (action === InputAction.PRESS) {
      this.heldButtons.add(button);
    } else {
      this.heldButtons.delete(button);
    }
  }

  // Only real touchscreen input reaches here; converted mouse touches go through handleMove/handleButton.
  handleTouch(event: TouchEvent, mapPoint: (x: number, y: number) => number[]): void {
    if (!this.touchEnabled || event.source !== SourceType.TouchScreen) {
      return;
    }
    const changed: TouchObject[] = event.changedTouches;
    for (let i = 0; i < changed.length; i++) {
      const touch: TouchObject = changed[i];
      const point: number[] = mapPoint(touch.x, touch.y);
      const pressure: number = SessionInput.normalizePressure(touch.pressure);
      if (touch.type === TouchType.Down) {
        this.onTouchDown(touch.id, point, pressure);
      } else if (touch.type === TouchType.Move) {
        this.onTouchMove(touch.id, point, pressure);
      } else if (touch.type === TouchType.Up) {
        this.onTouchUp(touch.id, point, pressure);
      } else if (touch.type === TouchType.Cancel) {
        this.onTouchCancel(touch.id, point);
      }
    }
  }

  releaseAll(): void {
    if (this.moveTimer !== -1) {
      clearTimeout(this.moveTimer);
      this.moveTimer = -1;
    }
    this.pendingMoveX = -1;
    this.pendingMoveY = -1;
    this.heldKeys.forEach((mapping: KeyMapping) => {
      this.sink.key(mapping, false);
    });
    this.heldKeys.clear();
    if (this.heldMeta) {
      this.sink.metaKey(false);
      this.heldMeta = false;
    }
    this.heldButtons.forEach((button: number) => {
      this.sink.button(InputAction.RELEASE, button, this.lastPointerX, this.lastPointerY);
    });
    this.heldButtons.clear();
    this.releaseAllTouches();
  }

  private onTouchDown(id: number, point: number[], pressure: number): void {
    // A Down for a contact already tracked is a retransmission.
    if (this.touchContactIds.has(id)) {
      return;
    }
    // A held Cancel resumed near its last position means the same finger is still down: move it instead of lifting and re-pressing.
    const resumed: number = this.findResumableCancel(point);
    if (resumed >= 0) {
      const known: number | undefined = this.touchContactIds.get(resumed);
      const resumedId: number = known !== undefined ? known : resumed + 1;
      this.clearCancelTimer(resumed);
      this.removePendingCancel(resumed);
      this.touchPoints.delete(resumed);
      this.touchContactIds.delete(resumed);
      this.touchPoints.set(id, point);
      this.touchContactIds.set(id, resumedId);
      this.sink.touch(TouchPhase.MOVE, resumedId, point[0], point[1], pressure);
      return;
    }
    this.finalizeAllCancels();
    const pointerId: number = id + 1;
    this.touchPoints.set(id, point);
    this.touchContactIds.set(id, pointerId);
    this.sink.touch(TouchPhase.DOWN, pointerId, point[0], point[1], pressure);
  }

  private onTouchMove(id: number, point: number[], pressure: number): void {
    const pointerId: number | undefined = this.touchContactIds.get(id);
    if (pointerId === undefined) {
      return;
    }
    if (this.isPendingCancel(id)) {
      this.clearCancelTimer(id);
      this.removePendingCancel(id);
    }
    const last: number[] | undefined = this.touchPoints.get(id);
    if (last !== undefined && last[0] === point[0] && last[1] === point[1]) {
      return;
    }
    this.touchPoints.set(id, point);
    this.sink.touch(TouchPhase.MOVE, pointerId, point[0], point[1], pressure);
  }

  private onTouchUp(id: number, point: number[], pressure: number): void {
    const pointerId: number | undefined = this.touchContactIds.get(id);
    if (pointerId === undefined) {
      return;
    }
    this.clearCancelTimer(id);
    this.removePendingCancel(id);
    this.touchPoints.delete(id);
    this.touchContactIds.delete(id);
    this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], pressure);
  }

  private onTouchCancel(id: number, point: number[]): void {
    if (!this.touchContactIds.has(id) || this.isPendingCancel(id)) {
      return;
    }
    // The finger is often still down; CANCEL_HOLD_MS is only a safety release for a Cancel that never resumes.
    this.touchPoints.set(id, point);
    this.pendingCancelIds.push(id);
    const timer: number = setTimeout(() => {
      this.cancelTimers.delete(id);
      this.finalizeCancel(id);
    }, CANCEL_HOLD_MS);
    this.cancelTimers.set(id, timer);
  }

  private findResumableCancel(point: number[]): number {
    const minDim: number = Math.min(this.pointerWidth, this.pointerHeight);
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
    const pointerId: number | undefined = this.touchContactIds.get(id);
    const point: number[] | undefined = this.touchPoints.get(id);
    this.touchPoints.delete(id);
    this.touchContactIds.delete(id);
    if (pointerId !== undefined && point !== undefined) {
      this.sink.touch(TouchPhase.UP, pointerId, point[0], point[1], 0);
    }
  }

  private finalizeAllCancels(): void {
    // Copy first: finalizeCancel mutates the list.
    const ids: number[] = this.pendingCancelIds.slice();
    for (let i = 0; i < ids.length; i++) {
      this.finalizeCancel(ids[i]);
    }
  }

  private releaseAllTouches(): void {
    this.finalizeAllCancels();
    if (this.touchContactIds.size > 0) {
      // One cancel-all is the protocol's own "all contacts gone" signal.
      this.sink.touch(TouchPhase.CANCEL_ALL, 0, 0, 0, 0);
    }
    this.touchPoints.clear();
    this.touchContactIds.clear();
  }

  // ArkUI pressure is [0, 65535); shared form is 0..1, where zero means the device did not report it.
  private static normalizePressure(pressure: number | undefined): number {
    if (pressure === undefined || pressure <= 0) {
      return 0;
    }
    return Math.min(1, pressure / 65535);
  }

  private flushMove(): void {
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
    this.sink.pointerMove(x, y);
  }

  private trackKey(keyCode: number, mapping: KeyMapping, down: boolean): void {
    const key: string = keyCode.toString();
    if (down) {
      if (!this.heldKeys.has(key)) {
        this.heldKeys.set(key, mapping);
      }
    } else {
      this.heldKeys.delete(key);
    }
  }

  private trackMeta(down: boolean): void {
    this.heldMeta = down;
  }

  // The lock states are only exposed from API 19; when absent the remote keeps its own state.
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
    this.sink.lockState(flags);
  }
}
