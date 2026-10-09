/*
 * HmRdp - remote cursor installer, shared by the RDP and Moonlight session
 * pages. Both protocols deliver the same thing: a decoded BGRA bitmap + hot
 * spot, a content key for dedup (RDP: native pixel hash; Moonlight: the
 * host's shape id) and a bitmap-density magnification (RDP: the 超分 desktop
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
 */
export class RemoteCursor {
  private enabled: boolean = true;
  private supported: boolean = true;
  private sessionWindow: window.Window | undefined = undefined;
  private pixelMap: image.PixelMap | undefined = undefined;
  private hotX: number = 0;
  private hotY: number = 0;
  private hidden: boolean = false;
  private lastKey: string = '';
  // Locator for a synthetic two-finger touch pinch: the system hides the
  // pointer on touch activity and the remote bitmap would not show either, so
  // a built-in hand marks the anchor (open = spread, grabbing = close).
  private pinchActive: boolean = false;
  private pinchZoomIn: boolean = true;

  setWindow(win: window.Window | undefined): void {
    this.sessionWindow = win;
  }

  /** Off: the remote stops driving the cursor (state cleared, nothing applied). */
  setEnabled(enabled: boolean): void {
    if (this.enabled === enabled) {
      return;
    }
    this.enabled = enabled;
    if (!enabled) {
      this.pixelMap = undefined;
      this.hidden = false;
      this.lastKey = '';
    }
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
      this.hidden = !update.visible;
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
      if (this.hidden && update.visible) {
        // A hide keeps the installed shape and the dedup key, so the resend
        // issued when the pointer becomes visible again (RDP has no "show"
        // event - the server resends its cached shape on the next pointer
        // update) is a hit: un-hide and re-install the kept PixelMap.
        this.hidden = false;
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
      this.hidden = !update.visible;
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
    this.hidden = false;
    this.lastKey = '';
    this.apply();
  }

  /** Session stopped driving the cursor; back to the default arrow. */
  reset(): void {
    this.hidden = false;
    this.pinchActive = false;
    this.pixelMap = undefined;
    this.lastKey = '';
    this.apply();
  }

  /** Restores cursor visibility on teardown; setPointerVisible is process-wide. */
  restoreVisibility(): void {
    this.pixelMap = undefined;
    try {
      pointer.setPointerVisibleSync(true);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'restore cursor failed: %{public}s', JSON.stringify(error));
    }
  }

  /** Pushes the current state onto the window's system cursor. */
  private apply(): void {
    if (!this.enabled || !this.supported) {
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
      if (this.hidden) {
        pointer.setPointerVisibleSync(false);
        return;
      }
      pointer.setPointerVisibleSync(true);
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
