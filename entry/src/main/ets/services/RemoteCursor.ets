/*
 * HmRdp - remote cursor installer, shared by the RDP and Moonlight session
 * pages. Both protocols deliver the same thing: a decoded BGRA bitmap + hot
 * spot, a content key for dedup (RDP: native pixel hash; Moonlight: the
 * host's shape id) and a bitmap-density magnification (RDP: 超分 desktop
 * factor; Moonlight: 1). Everything past that point is policy and lives here:
 * target size (magnification, the 256x256 system cap), hotspot scaling,
 * dedup, PixelMap construction and the system-cursor calls. Native keeps only
 * what must be native: the protocol decode and the scaleBgraBitmap kernel
 * (handle-to-handle, so the bitmap never crosses NAPI for the transform).
 */
import { image } from '@kit.ImageKit';
import { pointer } from '@kit.InputKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { scaleBgraBitmap } from 'libhmrdp.so';
import { RdpNative } from './RdpNative';

const DOMAIN: number = 0x0dfa;
const TAG: string = 'RemoteCursor';

// HarmonyOS custom cursors cap at 256x256 (see setCustomCursorSync); larger
// shapes are scaled down to fit, never dropped.
const MAX_CURSOR_SIDE: number = 256;
// Source sanity cap: a larger shape is treated as corrupt (native applies the
// same bound at decode; this guards the Moonlight path, which has none).
const MAX_CURSOR_SOURCE_SIDE: number = 1024;
// Unsharp strength handed to an upscale, in 5-tap mask units; downscales
// don't sharpen.
const UPSCALE_SHARPNESS: number = 0.8;

/**
 * One remote cursor update. `hasShape` false means a visibility-only change
 * (no bitmap); otherwise the bitmap is a native buffer handle that
 * RemoteCursor consumes on every path (scale / read / release).
 */
export class RemoteCursorUpdate {
  hasShape: boolean = false;
  visible: boolean = true;
  /** Protocol-native content key (RDP: pixel hash; Moonlight: shape id). */
  shapeKey: number = 0;
  /** Bitmap density relative to the picture (RDP: 超分 factor; else 1). */
  magnification: number = 1;
  width: number = 0;
  height: number = 0;
  hotX: number = 0;
  hotY: number = 0;
  imageHandle: number = 0;
}

/**
 * Installs remote cursor shapes as the HarmonyOS system cursor of the session
 * window. Owns the installed state (PixelMap, hot spot, visibility, dedup
 * key) and the capability probe (a device without a mouse throws once and is
 * left alone afterwards).
 *
 * The shape is per window, but `setPointerVisibleSync` is process-wide ("the
 * process's windows all share the state"), so visibility is not free to be
 * set by every session at once. The installer therefore keeps the switch
 * owned by the active window only, and drops the page's transient hide when
 * the window goes inactive - otherwise a session that hid the cursor would
 * leave it hidden on the main window after switching away. Each session still
 * holds its own shape/hidden state, so they stay independent and reactivating
 * one replays exactly its own cursor.
 */
export class RemoteCursor {
  // Host-driven cursor (shape plus the remote's own hide); off when the RDP
  // cursor setting is off. The process-global switch below is still driven for
  // the page's local (Moonlight HIDE_SYSTEM) hide, which is independent of it.
  private enabled: boolean = true;
  private supported: boolean = true;
  private sessionWindow: window.Window | undefined = undefined;
  private pixelMap: image.PixelMap | undefined = undefined;
  private hotX: number = 0;
  private hotY: number = 0;
  // The remote asked the cursor to be invisible (RDP SetNull / Moonlight
  // flag). It belongs to the session and is kept across a focus change.
  private hostHidden: boolean = false;
  // The page asked the system cursor to be invisible over the picture
  // (Moonlight HIDE_SYSTEM hover). It is a transient of the pointer being on
  // this window, so it is dropped when the window stops being active.
  private localHidden: boolean = false;
  private lastKey: string = '';
  // Locator for a synthetic two-finger touch pinch: the system hides the
  // pointer on touch activity and the remote bitmap would not show either, so
  // a built-in hand marks the anchor (open = spread, grabbing = close).
  private pinchActive: boolean = false;
  private pinchZoomIn: boolean = true;

  // setPointerVisibleSync applies to every window of the process, so the
  // switch is owned by at most one window at a time: the active one. A window
  // that stops being active hands it back to "shown" unless the incoming
  // active window already claimed it (its activation can land first) - the
  // owner is only cleared here, never by a window that no longer holds it.
  private static visibilityOwner: RemoteCursor | undefined = undefined;

  /**
   * Binds the session window and pushes the current state onto it. Binding is
   * one of the replay points that keep the invariant: the window arrives
   * asynchronously while shape events can land earlier (their state is kept
   * regardless), and neither protocol guarantees a resend - Moonlight sends a
   * shape only when its id changes - so readiness must re-apply, not wait.
   *
   * Also subscribes to window activation: activation is another point where
   * the window's custom cursor can be dropped, and its timing is independent
   * of pointer hover (a hover-driven re-apply can land before the activation
   * reset and lose), so activation must re-push the state too. A protocol
   * that never resends the unchanged shape (Moonlight) has no other recovery.
   * The same subscription carries the deactivation that frees the global
   * visibility switch - which is the only reliable signal, since a closed
   * session window does not run the page's teardown.
   */
  setWindow(win: window.Window | undefined): void {
    const previous: window.Window | undefined = this.sessionWindow;
    if (previous !== undefined && previous !== win) {
      try {
        previous.off('windowEvent', this.windowEventCallback);
      } catch (error) {
        hilog.warn(DOMAIN, TAG, 'unwatch windowEvent failed: %{public}s', JSON.stringify(error));
      }
    }
    this.sessionWindow = win;
    if (win !== undefined) {
      if (win !== previous) {
        try {
          win.on('windowEvent', this.windowEventCallback);
        } catch (error) {
          hilog.warn(DOMAIN, TAG, 'watch windowEvent failed: %{public}s', JSON.stringify(error));
        }
      }
      // A freshly bound session window is the one being presented, so it takes
      // the process-global switch; activation re-claims it if it was dropped.
      this.claim();
    } else {
      this.release();
    }
  }

  /** Claims / re-pushes the cursor state when the window becomes active. */
  private windowEventCallback: (event: window.WindowEventType) => void =
    (event: window.WindowEventType): void => {
      if (event === window.WindowEventType.WINDOW_ACTIVE
        || event === window.WindowEventType.WINDOW_SHOWN) {
        this.claim();
        return;
      }
      if (event === window.WindowEventType.WINDOW_INACTIVE
        || event === window.WindowEventType.WINDOW_HIDDEN
        || event === window.WindowEventType.WINDOW_DESTROYED) {
        // The pointer-over-picture hide is a transient of this window being
        // active; the host's own hide belongs to the session and is kept, so a
        // session that comes back replays its cursor.
        this.localHidden = false;
        this.release();
      }
    };

  /** Makes this window the one driving the process-global cursor switch. */
  private claim(): void {
    RemoteCursor.visibilityOwner = this;
    this.apply();
  }

  /**
   * Gives the switch back. Only the current owner may do so: a window whose
   * deactivation lands after the incoming window already claimed must not
   * overwrite that window's state.
   */
  private release(): void {
    if (RemoteCursor.visibilityOwner !== this) {
      return;
    }
    RemoteCursor.visibilityOwner = undefined;
    this.showGlobal();
  }

  private isOwner(): boolean {
    return RemoteCursor.visibilityOwner === this;
  }

  /**
   * Off: the remote stops driving the cursor (state cleared, nothing applied).
   * The page's local hide (setLocalHidden) is unaffected.
   */
  setEnabled(enabled: boolean): void {
    if (this.enabled === enabled) {
      return;
    }
    this.enabled = enabled;
    if (!enabled) {
      this.pixelMap = undefined;
      this.hostHidden = false;
      this.lastKey = '';
    }
    this.apply();
  }

  /**
   * Page-driven local hide (Moonlight HIDE_SYSTEM): hides the system cursor
   * while the pointer is over the picture. Independent of the host shape, and
   * cleared whenever the window stops being active (see windowEventCallback).
   */
  setLocalHidden(hidden: boolean): void {
    if (this.localHidden === hidden) {
      return;
    }
    this.localHidden = hidden;
    this.apply();
  }

  /** RDP pinch locator; a no-op call (unchanged) costs nothing. */
  setPinchOverride(active: boolean, zoomIn: boolean): void {
    if (this.pinchActive === active && this.pinchZoomIn === zoomIn) {
      return;
    }
    this.pinchActive = active;
    this.pinchZoomIn = zoomIn;
    this.apply();
  }

  /**
   * Applies one update. The invariant the cursor is kept on: the applied
   * system cursor always equals the latest delivered (shape, visibility).
   * A shape event implies the visibility its protocol carries - RDP has no
   * standalone "show" event, so a shape (or default) arriving *is* the show;
   * Moonlight may attach an explicit visible flag to a shape, which wins.
   * Dedup is a pure optimization over that invariant: a hit means the event
   * implies the already-installed shape, so the only possible divergence is
   * visibility, and the transition re-uses the kept PixelMap (no rebuild).
   * Every path consumes the update's buffer handle.
   */
  applyUpdate(update: RemoteCursorUpdate): void {
    const handle: number = update.imageHandle;
    if (!this.enabled) {
      // Disabled: the remote does not drive the cursor at all - no state
      // mutation, just handle hygiene.
      if (handle > 0) {
        RdpNative.releaseBuffer(handle);
      }
      return;
    }
    if (!update.hasShape) {
      if (handle > 0) {
        RdpNative.releaseBuffer(handle);
      }
      // Visibility-only change: the installed shape is kept.
      this.hostHidden = !update.visible;
      this.apply();
      return;
    }
    const width: number = update.width;
    const height: number = update.height;
    const usable: boolean = handle > 0 && width > 0 && height > 0 &&
      width <= MAX_CURSOR_SOURCE_SIDE && height <= MAX_CURSOR_SOURCE_SIDE;
    if (!usable) {
      if (handle > 0) {
        RdpNative.releaseBuffer(handle);
      }
      // A corrupt shape falls back to the default arrow instead of leaving a
      // stale bitmap behind.
      this.applyDefault();
      return;
    }
    const magnification: number = update.magnification > 1 ? update.magnification : 1;
    const magMilli: number = Math.round(magnification * 1000);
    const key: string = `${update.shapeKey}:${width}x${height}:${update.hotX},${update.hotY}:${magMilli}`;
    if (key === this.lastKey) {
      RdpNative.releaseBuffer(handle);
      if (this.hostHidden && update.visible) {
        // A hide keeps the installed shape and the dedup key, so the resend
        // issued when the pointer becomes visible again (RDP has no "show"
        // event - the server resends its cached shape on the next pointer
        // update) is a hit: un-hide and re-install the kept PixelMap.
        this.hostHidden = false;
        this.apply();
      }
      return;
    }
    // Target: source pixels scaled by the desktop magnification, clamped to
    // the system cap with aspect preserved; the hot spot follows the same
    // factor so a clamped cursor keeps its anchor.
    let outW: number = Math.max(1, Math.round(width * magnification));
    let outH: number = Math.max(1, Math.round(height * magnification));
    const dstSide: number = Math.max(outW, outH);
    if (dstSide > MAX_CURSOR_SIDE) {
      const factor: number = MAX_CURSOR_SIDE / dstSide;
      outW = Math.max(1, Math.round(outW * factor));
      outH = Math.max(1, Math.round(outH * factor));
    }
    let finalHandle: number = handle;
    if (outW !== width || outH !== height) {
      // Consumes the source handle either way; 0 marks the input unusable.
      finalHandle = scaleBgraBitmap(handle, width, height, outW, outH,
        outW > width ? UPSCALE_SHARPNESS : 0);
      if (finalHandle <= 0) {
        hilog.warn(DOMAIN, TAG, 'cursor rescale failed: %{public}dx%{public}d -> %{public}dx%{public}d',
          width, height, outW, outH);
        return;
      }
    }
    const bytes: ArrayBuffer | undefined = RdpNative.readAndReleaseBuffer(finalHandle);
    if (bytes === undefined) {
      return;
    }
    const options: image.InitializationOptions = {
      editable: false,
      pixelFormat: image.PixelMapFormat.BGRA_8888,
      size: { width: outW, height: outH }
    };
    try {
      this.pixelMap = image.createPixelMapSync(bytes, options);
      this.hotX = Math.max(0, Math.min(Math.round(update.hotX * outW / width), outW));
      this.hotY = Math.max(0, Math.min(Math.round(update.hotY * outH / height), outH));
      this.hostHidden = !update.visible;
      this.lastKey = key;
      this.apply();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'create cursor pixelmap failed: %{public}s', JSON.stringify(error));
    }
  }

  /**
   * Re-installs the current state: the system may drop a custom cursor when
   * the pointer leaves and re-enters the window, so the page re-applies on
   * hover.
   */
  reapply(): void {
    this.apply();
  }

  /** The remote asked for the default arrow (RDP SYSPTR_DEFAULT). */
  applyDefault(): void {
    this.pixelMap = undefined;
    this.hostHidden = false;
    this.lastKey = '';
    this.apply();
  }

  /**
   * A session stopped driving the cursor (failed / idle) but its window stays:
   * clear the cached state and push the default arrow, keeping this window's
   * claim on the process-global switch. A retry in the same window must be able
   * to install a cursor without waiting for a focus event, so freeing the
   * switch is left to detaching the window (setWindow(undefined)) or to the
   * window losing focus / being destroyed.
   */
  reset(): void {
    this.hostHidden = false;
    this.localHidden = false;
    this.pinchActive = false;
    this.pixelMap = undefined;
    this.lastKey = '';
    this.apply();
  }

  /** Pushes the system cursor back to plain visible; the switch is global. */
  private showGlobal(): void {
    if (!this.supported) {
      return;
    }
    try {
      pointer.setPointerVisibleSync(true);
    } catch (error) {
      this.supported = false;
      hilog.warn(DOMAIN, TAG, 'restore cursor failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Pushes the current state onto the window's system cursor. */
  private apply(): void {
    if (!this.supported) {
      return;
    }
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    // Only the active window drives the (process-global) system cursor; an
    // inactive one keeps its state and re-pushes it on activation.
    if (!this.isOwner()) {
      return;
    }
    try {
      const windowId: number = win.getWindowProperties().id;
      if (windowId < 0) {
        return;
      }
      if (this.pinchActive) {
        pointer.setPointerVisibleSync(true);
        // A style the device does not implement must not take the whole
        // cursor path down, so fall back to the plain arrow locally.
        try {
          pointer.setPointerStyleSync(windowId, this.pinchZoomIn
            ? pointer.PointerStyle.HAND_OPEN
            : pointer.PointerStyle.HAND_GRABBING);
        } catch (styleError) {
          pointer.setPointerStyleSync(windowId, pointer.PointerStyle.DEFAULT);
        }
        return;
      }
      if (this.hostHidden || this.localHidden) {
        pointer.setPointerVisibleSync(false);
        return;
      }
      pointer.setPointerVisibleSync(true);
      if (!this.enabled) {
        return;
      }
      const pixelMap: image.PixelMap | undefined = this.pixelMap;
      if (pixelMap !== undefined) {
        pointer.setCustomCursorSync(windowId, pixelMap, this.hotX, this.hotY);
      } else {
        pointer.setPointerStyleSync(windowId, pointer.PointerStyle.DEFAULT);
      }
    } catch (error) {
      // Capability probing: on a device without a mouse the call throws, so
      // stop trying and leave the system cursor alone.
      this.supported = false;
      hilog.warn(DOMAIN, TAG, 'set cursor failed: %{public}s', JSON.stringify(error));
    }
  }
}
