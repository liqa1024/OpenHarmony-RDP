import { image } from '@kit.ImageKit';
import { pointer } from '@kit.InputKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { scaleBgraBitmap } from 'libhmrdp.so';
import { RdpNative } from './RdpNative';

const DOMAIN: number = 0x0dfa;
const TAG: string = 'RemoteCursor';

// HarmonyOS custom cursors cap at 256x256 (see setCustomCursorSync); larger shapes are scaled down, never dropped.
const MAX_CURSOR_SIDE: number = 256;
// 源尺寸 sanity 上限，超出视为损坏；Moonlight 路径在原生侧没有同样上限，故在此把守。
const MAX_CURSOR_SOURCE_SIDE: number = 1024;
// 仅放大时加锐（5-tap mask 单位）；缩小不锐化。
const UPSCALE_SHARPNESS: number = 0.8;

// hasShape 为 false = 仅可见性变化；位图是原生 buffer 句柄，每条路径都要消费（缩放 / 读取 / 释放）。
export class RemoteCursorUpdate {
  hasShape: boolean = false;
  visible: boolean = true;
  shapeKey: number = 0;
  magnification: number = 1;
  width: number = 0;
  height: number = 0;
  hotX: number = 0;
  hotY: number = 0;
  imageHandle: number = 0;
}

// setPointerVisibleSync 是进程级开关，只归当前活动窗口持有；失活时归还并丢弃本页临时隐藏，否则切走后主窗口光标仍被藏着。
export class RemoteCursor {
  // 远端驱动开关；关掉后进程级可见性开关仍会被页面本地隐藏（Moonlight HIDE_SYSTEM）驱动，二者独立。
  private enabled: boolean = true;
  private supported: boolean = true;
  private sessionWindow: window.Window | undefined = undefined;
  private pixelMap: image.PixelMap | undefined = undefined;
  private hotX: number = 0;
  private hotY: number = 0;
  // 远端要求的隐藏（RDP SetNull / Moonlight 标志）；属于会话，跨焦点变化保留。
  private hostHidden: boolean = false;
  // 页面本地隐藏（Moonlight HIDE_SYSTEM 悬停）；是指针在本窗口上的临时态，窗口失活即丢弃。
  private localHidden: boolean = false;
  private lastKey: string = '';
  // 双指捏合定位标记：触摸时系统隐藏指针、远端位图也画不出，故用内置手型标锚点（张开 = 放大，握住 = 缩小）。
  private pinchActive: boolean = false;
  private pinchZoomIn: boolean = true;

  // 开关同一时刻至多归一个窗口持有；新窗口激活可能先于旧窗口失活到达，故 owner 只能由持有者自己清除。
  private static visibilityOwner: RemoteCursor | undefined = undefined;

  // 窗口异步到达而形状事件可能更早（两协议都不保证重发），故绑定与激活都必须重放当前状态；失活同时负责释放进程级可见性开关。
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
      this.claim();
    } else {
      this.release();
    }
  }

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
        // 本地隐藏是窗口活跃的临时态故丢弃；远端的隐藏属于会话故保留。
        this.localHidden = false;
        this.release();
      }
    };

  private claim(): void {
    RemoteCursor.visibilityOwner = this;
    this.apply();
  }

  // 只有当前持有者可归还：失活晚于新窗口认领到达的旧窗口不得覆盖新窗口状态。
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

  setLocalHidden(hidden: boolean): void {
    if (this.localHidden === hidden) {
      return;
    }
    this.localHidden = hidden;
    this.apply();
  }

  setPinchOverride(active: boolean, zoomIn: boolean): void {
    if (this.pinchActive === active && this.pinchZoomIn === zoomIn) {
      return;
    }
    this.pinchActive = active;
    this.pinchZoomIn = zoomIn;
    this.apply();
  }

  // 不变式：系统光标始终等于最新送达的（形状, 可见性）；去重只是该不变式上的纯优化，每条路径都要消费 update 的 buffer 句柄。
  applyUpdate(update: RemoteCursorUpdate): void {
    const handle: number = update.imageHandle;
    if (!this.enabled) {
      if (handle > 0) {
        RdpNative.releaseBuffer(handle);
      }
      return;
    }
    if (!update.hasShape) {
      if (handle > 0) {
        RdpNative.releaseBuffer(handle);
      }
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
      this.applyDefault();
      return;
    }
    const magnification: number = update.magnification > 1 ? update.magnification : 1;
    const magMilli: number = Math.round(magnification * 1000);
    const key: string = `${update.shapeKey}:${width}x${height}:${update.hotX},${update.hotY}:${magMilli}`;
    if (key === this.lastKey) {
      RdpNative.releaseBuffer(handle);
      if (this.hostHidden && update.visible) {
        // RDP 没有「显示」事件：指针恢复可见时服务器会重发缓存的形状，而隐藏不改去重键，故这次重发必命中——在此取消隐藏并重装保留的 PixelMap。
        this.hostHidden = false;
        this.apply();
      }
      return;
    }
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

  // The system may drop a custom cursor when the pointer leaves and re-enters the window, so the page re-applies on hover.
  reapply(): void {
    this.apply();
  }

  applyDefault(): void {
    this.pixelMap = undefined;
    this.hostHidden = false;
    this.lastKey = '';
    this.apply();
  }

  reset(): void {
    this.hostHidden = false;
    this.localHidden = false;
    this.pinchActive = false;
    this.pixelMap = undefined;
    this.lastKey = '';
    this.apply();
  }

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

  private apply(): void {
    if (!this.supported) {
      return;
    }
    const win: window.Window | undefined = this.sessionWindow;
    if (win === undefined) {
      return;
    }
    // Only the active window drives the (process-global) system cursor; an inactive one keeps its state and re-pushes it on activation.
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
        // A style the device does not implement must not take the whole cursor path down; fall back to the plain arrow.
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
      // Capability probing: on a device without a mouse the call throws, so stop trying.
      this.supported = false;
      hilog.warn(DOMAIN, TAG, 'set cursor failed: %{public}s', JSON.stringify(error));
    }
  }
}
