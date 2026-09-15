/*
 * HmRdp - dev-only RDP replay page (doc_agent/gfx-engine.md §6).
 *
 * Binds this XComponent's surface and replays the recorded hmrdp_gfx.bin capture
 * straight to the screen through the GPU desktop engine. No RDP session or
 * network is involved. The capture is not shipped in the HAP - `hdc file send`
 * it into the app filesDir as hmrdp_gfx.bin before opening this page (entered
 * from the home screen "回放测试" entry).
 */
import { RdpNative } from '../services/RdpNative';
import { window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpReplay';

// ClearCodec batch presets: union-rectangle pixel cap. 0 = one flush per command
// (minimum mapped bytes, maximum round trips). Only the GLES route uses this;
// the Vulkan engine decodes ClearCodec directly on the mapped surface.
const BATCH_PRESET_PX: number[] = [0, 262144, 1048576, 4194304];
const BATCH_PRESET_LABEL: string[] = ['单条', '小', '中', '大'];
// 0 = CPU (FreeRDP gdi) only (perf reference), 1 = GLES engine only,
// 2 = Vulkan engine only, 3 = GLES vs gdi compare, 4 = Vulkan vs gdi compare.
// Ids match the native GfxReplayRoute enum.
const ROUTE_CPU: number = 0;
const ROUTE_GLES: number = 1;
const ROUTE_VULKAN: number = 2;
const ROUTE_GLES_COMPARE: number = 3;
const ROUTE_VULKAN_COMPARE: number = 4;
const ROUTE_LABEL: string[] = ['CPU', 'GLES', 'Vulkan', 'GLES对比', 'Vulkan对比'];

class ReplaySurfaceController extends XComponentController {
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
struct GfxReplayPage {
  @State status: string = '等待 surface...';
  // Decoder/presenter route; see ROUTE_LABEL (ids match the native enum).
  @State replayRoute: number = ROUTE_CPU;
  // On-device performance panel: the stats string is multi-line, shown in a
  // floating translucent box so the actual picture keeps the full surface.
  @State showStats: boolean = true;
  @State gpuInfo: string = '';
  // ClearCodec batch granularity presets (union-rectangle pixel cap): index into
  // BATCH_PRESET_PX. Larger = fewer map/unmap round trips but more padding in
  // the mapped staging buffer; the right value is device-dependent, which is
  // exactly what this switch is for.
  @State batchPreset: number = 2;  // 1M px (~4MB) default
  // Vulkan capability report, fetched when a Vulkan route starts.
  @State vkInfo: string = '';
  private controller: ReplaySurfaceController = new ReplaySurfaceController();
  private statsTimer: number = -1;
  private surfaceId: string = '';
  private surfaceW: number = 0;
  private surfaceH: number = 0;

  private isVulkanRoute(): boolean {
    return this.replayRoute === ROUTE_VULKAN || this.replayRoute === ROUTE_VULKAN_COMPARE;
  }

  aboutToAppear(): void {
    this.applyFullScreen(true);
    // Renderer / GLES version - the key signal that the GLES path is really in
    // use (compute=1) when running on the target device.
    try {
      this.gpuInfo = RdpNative.describeGpuCompute();
    } catch (error) {
      this.gpuInfo = `gpu info unavailable: ${JSON.stringify(error)}`;
    }
    this.controller.setCallbacks(
      (surfaceId: string, width: number, height: number) => {
        this.surfaceId = surfaceId;
        this.surfaceW = width;
        this.surfaceH = height;
        // The surface is also re-created after a background/foreground round
        // trip, so the current route decides what is brought back up.
        this.startReplay();
      },
      (surfaceId: string, width: number, height: number) => {
        this.surfaceW = width;
        this.surfaceH = height;
        RdpNative.resizeReplayTest(width, height);
      },
      (surfaceId: string) => {
        this.stopStatsTimer();
        RdpNative.stopReplayTest();
      }
    );
  }

  aboutToDisappear(): void {
    this.stopStatsTimer();
    RdpNative.stopReplayTest();
    this.applyFullScreen(false);
  }

  private startReplay(): void {
    if (this.surfaceW <= 0 || this.surfaceH <= 0 || this.surfaceId.length === 0) {
      return;
    }
    let dir: string = '';
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx !== undefined) {
        dir = ctx.filesDir;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'filesDir unavailable: %{public}s', JSON.stringify(error));
    }
    if (dir.length === 0) {
      this.status = 'filesDir 不可用';
      return;
    }
    RdpNative.stopReplayTest();
    RdpNative.setReplayBatchArea(BATCH_PRESET_PX[this.batchPreset]);
    if (this.isVulkanRoute()) {
      try {
        this.vkInfo = RdpNative.describeVulkan();
      } catch (error) {
        this.vkInfo = `vulkan info unavailable: ${JSON.stringify(error)}`;
      }
    }
    const summary: string = RdpNative.startReplayTest(this.surfaceId, this.surfaceW,
      this.surfaceH, `${dir}/hmrdp_gfx.bin`, this.replayRoute);
    this.status = summary.startsWith('failed')
      ? `${summary}（需先把录制的 hmrdp_gfx.bin 放入应用 filesDir）`
      : summary;
    this.startStatsTimer();
  }

  private applyFullScreen(enable: boolean): void {
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx === undefined) {
        return;
      }
      window.getLastWindow(ctx).then((win: window.Window) => {
        win.setWindowDecorVisible(!enable);
        win.setWindowLayoutFullScreen(enable).catch((err: BusinessError) => {
          hilog.warn(DOMAIN, TAG, 'layoutFullScreen failed: %{public}s', JSON.stringify(err));
        });
        if (enable) {
          win.maximize().catch((err: BusinessError) => {
            hilog.warn(DOMAIN, TAG, 'maximize failed: %{public}s', JSON.stringify(err));
          });
        } else {
          // Restore the free-floating window: maximize() must be undone so the
          // home window does not stay maximized after leaving this dev page.
          win.recover().catch((err: BusinessError) => {
            hilog.warn(DOMAIN, TAG, 'recover failed: %{public}s', JSON.stringify(err));
          });
        }
      }).catch((err: BusinessError) => {
        hilog.warn(DOMAIN, TAG, 'getLastWindow failed: %{public}s', JSON.stringify(err));
      });
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'fullscreen error: %{public}s', JSON.stringify(error));
    }
  }

  private startStatsTimer(): void {
    if (this.statsTimer !== -1) {
      return;
    }
    this.statsTimer = setInterval(() => {
      this.status = RdpNative.replayTestStats();
    }, 1000);
  }

  private stopStatsTimer(): void {
    if (this.statsTimer !== -1) {
      clearInterval(this.statsTimer);
      this.statsTimer = -1;
    }
  }

  build() {
    Column() {
      Row() {
        Button('返回')
          .height(36)
          .fontSize(14)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(10)
          .onClick(() => {
            this.getUIContext().getRouter().back();
          })

        Text('RDP 回放测试')
          .fontSize(16)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .margin({ left: 12 })
          .layoutWeight(1)

        Button(`路线:${ROUTE_LABEL[this.replayRoute]}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.replayRoute = (this.replayRoute + 1) % ROUTE_LABEL.length;
            this.startReplay();
          })

        Button(`批次:${BATCH_PRESET_LABEL[this.batchPreset]}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.batchPreset = (this.batchPreset + 1) % BATCH_PRESET_PX.length;
            this.startReplay();
          })


        Button(this.showStats ? '隐藏信息' : '显示信息')
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.showStats = !this.showStats;
          })

        Button('重新回放')
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .onClick(() => {
            this.startReplay();
          })
      }
      .width('100%')
      .padding({ left: 12, right: 12, top: 8, bottom: 8 })

      Stack({ alignContent: Alignment.BottomStart }) {
        XComponent({
          id: 'replaySurface',
          type: XComponentType.SURFACE,
          controller: this.controller
        })
          .width('100%')
          .height('100%')
          .backgroundColor('#000000')

        if (this.showStats) {
          Column() {
            Text(this.isVulkanRoute() && this.vkInfo.length > 0 ? this.vkInfo : this.gpuInfo)
              .fontSize(12)
              .fontColor('#FF9CDCFE')
              .width('100%')
            Text(`路线:${ROUTE_LABEL[this.replayRoute]}  批次:${BATCH_PRESET_LABEL[this.batchPreset]} (${BATCH_PRESET_PX[this.batchPreset]} px)`)
              .fontSize(12)
              .fontColor('#FFFFD479')
              .width('100%')
            Text(this.status)
              .fontSize(13)
              .fontColor('#FFF5F5F5')
              .width('100%')
              .margin({ top: 4 })
          }
          .alignItems(HorizontalAlign.Start)
          .padding(10)
          .margin({ left: 12, bottom: 12 })
          .constraintSize({ maxWidth: '95%' })
          .backgroundColor('#D9000000')
          .borderRadius(10)
        }
      }
      .width('100%')
      .layoutWeight(1)
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
