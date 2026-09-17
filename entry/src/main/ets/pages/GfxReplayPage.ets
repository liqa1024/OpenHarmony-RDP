/*
 * HmRdp - dev-only RDP replay page (doc_agent/gfx-engine.md §6).
 *
 * Binds this XComponent's surface and replays the recorded hmrdp_gfx.bin capture
 * straight to the screen through the Vulkan desktop engine. No RDP session or
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

// 0 = CPU (FreeRDP gdi) only (perf reference), 1 = Vulkan engine only,
// 2 = Vulkan vs gdi compare. Ids match the native GfxReplayRoute enum.
const ROUTE_CPU: number = 0;
const ROUTE_VULKAN: number = 1;
const ROUTE_VULKAN_COMPARE: number = 2;
const ROUTE_LABEL: string[] = ['CPU', 'Vulkan', 'Vulkan对比'];

// Progressive decode worker counts the 「线程」 button cycles through. 0 is the
// automatic choice (performance-core count, else min(cores, 4)); 1 decodes on
// the receiving thread with no worker pool. The decode is the CPU (gdi) path's
// dominant cost, so this is the knob for its time-vs-energy curve
// (doc_agent/cpu-path.md §5).
const DECODE_THREAD_CHOICES: number[] = [0, 1, 2, 3, 4, 6, 8];
const DECODE_THREAD_LABEL: string[] = ['自动', '1', '2', '3', '4', '6', '8'];

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
  // Playback clock: false = not throttled at all (runs flat out, a throughput
  // measurement), true = the capture's own arrival times (the cadence the live
  // session ran at). Needs a capture recorded with arrival times; the stats line
  // reports the mode actually used (`mode=fast|realtime`).
  @State replayRealtime: boolean = false;
  // Index into DECODE_THREAD_CHOICES: which Progressive decode worker count this
  // dev page asks for (applied process-wide, so the CPU route uses it).
  @State decodeWorkerIndex: number = 0;
  // On-device performance panel: the stats string is multi-line, shown in a
  // floating translucent box so the actual picture keeps the full surface.
  @State showStats: boolean = true;
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
    // The decode worker count is process-wide and picked up at the next
    // Progressive message, so set it before the replay starts feeding.
    try {
      RdpNative.applyDecodeThreads(DECODE_THREAD_CHOICES[this.decodeWorkerIndex]);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'decode threads unavailable: %{public}s', JSON.stringify(error));
    }
    RdpNative.stopReplayTest();
    if (this.isVulkanRoute()) {
      try {
        this.vkInfo = RdpNative.describeVulkan();
      } catch (error) {
        this.vkInfo = `vulkan info unavailable: ${JSON.stringify(error)}`;
      }
    }
    const summary: string = RdpNative.startReplayTest(this.surfaceId, this.surfaceW,
      this.surfaceH, `${dir}/hmrdp_gfx.bin`, this.replayRoute, this.replayRealtime);
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

        Button(`节拍:${this.replayRealtime ? '实时' : '跑满'}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.replayRealtime = !this.replayRealtime;
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

        // Decode worker count for the CPU (gdi) route: each click restarts the
        // replay, so one click per point on the time-vs-energy curve.
        Button(`线程:${DECODE_THREAD_LABEL[this.decodeWorkerIndex]}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ left: 8 })
          .onClick(() => {
            this.decodeWorkerIndex = (this.decodeWorkerIndex + 1) % DECODE_THREAD_CHOICES.length;
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
            if (this.vkInfo.length > 0) {
              Text(this.vkInfo)
                .fontSize(12)
                .fontColor('#FF9CDCFE')
                .width('100%')
            }
            Text(`路线:${ROUTE_LABEL[this.replayRoute]}  线程:${DECODE_THREAD_LABEL[this.decodeWorkerIndex]}`)
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
