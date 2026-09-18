/*
 * HmRdp - dev-only RDP replay page (doc_agent/gfx-engine.md §6).
 *
 * Binds this XComponent's surface and replays the recorded hmrdp_gfx.bin capture
 * straight to the screen. No RDP session or network is involved. The capture is
 * not shipped in the HAP - `hdc file send` it into the app filesDir as
 * hmrdp_gfx.bin before opening this page (entered from the home screen "回放测试"
 * entry).
 *
 * The page drives FreeRDP's gdi decode; 「路线」 picks which presenter it runs
 * through (CPU = GLES, 硬件加速 = Vulkan), 「参考」 runs it against the golden
 * reference image recorded earlier from the same capture.
 */
import { RdpNative } from '../services/RdpNative';
import { Capability, DeviceCapabilities } from '../services/DeviceCapabilities';
import { AppSettings, SettingsStore } from '../services/SettingsStore';
import { window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpReplay';

// 「路线」 = which presentation line the gdi decode runs through:
//   CPU      - the GLES presenter (no Vulkan at all)
//   硬件加速  - the Vulkan presenter
// This is the session's "硬件加速" setting, exposed here so both presenters can be
// A/B'd on the same capture.
const LINE_CPU: number = 0;
const LINE_ACCEL: number = 1;
const LINE_LABEL: string[] = ['CPU', '硬件加速'];

// 「参考」 = golden-reference gate (doc_agent/gfx-engine.md §8.4).
//   关   - plain run (the perf modes above)
//   导出 - record this run as the reference (hmrdp_ref.hash + hmrdp_ref.bmp)
//   对比 - check every frame against the stored reference; the stats line reports
//          `ref compare: … bad=…` plus the pixel diff of the stored image's frame
const REF_OFF: number = 0;
const REF_EXPORT: number = 1;
const REF_COMPARE: number = 2;
const REF_LABEL: string[] = ['关', '导出', '对比'];

// Progressive decode widths offered by the 「线程」 row. 0 is the automatic
// choice (all online cores, capped at 16); 1 decodes on the receiving thread
// with no task submission. The decode is the CPU (gdi) path's dominant cost, so
// this is the knob for its time-vs-energy curve
// (doc_agent/cpu-accel-plan.md §0/§2).
//
// Every value is a button of its own: one click selects it and restarts the
// replay, so a measurement point is reached in one action instead of by cycling
// through the values in between.
const DECODE_THREAD_CHOICES: number[] = [0, 1, 2, 3, 4, 6, 8];
const DECODE_THREAD_LABEL: string[] = ['自动', '1', '2', '3', '4', '6', '8'];

// Dev A/B for the tile work decomposition (doc_agent/cpu-accel-plan.md §0):
//   正常    - shared claim cursor: small blocks handed out dynamically (balance)
//   段+偷取  - each task owns one contiguous home range (workers stay on
//              contiguous memory, homes far apart) and steals the others'
//              remaining blocks, so the tail stays block-granular
//   排队    - home split, plus width 1 is routed through the platform queue
//              instead of the receiving thread's serial loop, to price the
//              executor's own cost (see hmrdp_parallel.h)
const PAR_MODE_LABEL: string[] = ['正常', '段+偷取', '排队'];

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
  // Presentation line (see LINE_LABEL): CPU (GLES) or 硬件加速 (Vulkan).
  @State line: number = LINE_ACCEL;
  // Golden-reference mode (see REF_LABEL).
  @State refMode: number = REF_OFF;
  // Playback clock: false = not throttled at all (runs flat out, a throughput
  // measurement), true = the capture's own arrival times (the cadence the live
  // session ran at). Needs a capture recorded with arrival times; the stats line
  // reports the mode actually used (`mode=fast|realtime`).
  @State replayRealtime: boolean = false;
  // Index into DECODE_THREAD_CHOICES: which Progressive decode worker count this
  // dev page asks for (applied process-wide, so the CPU route uses it). 0 is the
  // automatic choice.
  @State decodeWorkerIndex: number = 0;
  // Index into PAR_MODE_LABEL: tile work decomposition (see PAR_MODE_LABEL).
  // Defaults to the home + tail-steal partition, which is what the decoder ships.
  @State parModeIndex: number = 1;
  // Whether this device can run the Vulkan presenter at all (see
  // DeviceCapabilities.hardwareAccel); when it cannot, 硬件加速 is not offered.
  @State hardwareAccelSupported: boolean = true;
  // On-device performance panel: the stats string is multi-line, shown in a
  // floating translucent box so the actual picture keeps the full surface.
  @State showStats: boolean = true;
  private controller: ReplaySurfaceController = new ReplaySurfaceController();
  private statsTimer: number = -1;
  private surfaceId: string = '';
  private surfaceW: number = 0;
  private surfaceH: number = 0;

  // Whether this run should present through Vulkan: the 硬件加速 line was picked
  // and the device supports it.
  private accelOn(): boolean {
    return this.line === LINE_ACCEL && this.hardwareAccelSupported;
  }

  aboutToAppear(): void {
    this.applyFullScreen(true);
    // Same source of truth as the settings page: the stored value decides which
    // line the page starts on, clamped by what this device can actually do.
    const capability: Capability = DeviceCapabilities.hardwareAccel();
    this.hardwareAccelSupported = capability.supported;
    this.line = (SettingsStore.load().hardwareAccel && capability.supported) ? LINE_ACCEL : LINE_CPU;
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
      RdpNative.applyDecodeParallelMode(this.parModeIndex);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'decode threads unavailable: %{public}s', JSON.stringify(error));
    }
    // The presenter backend is chosen when the run starts, so apply the
    // acceleration state here as well (it may have been changed on the settings
    // page since this page was opened).
    RdpNative.applyHardwareAccel(this.accelOn());
    RdpNative.stopReplayTest();
    // The line picked above decides which presenter the gdi decode presents
    // through.
    const summary: string = RdpNative.startReplayTest(this.surfaceId, this.surfaceW,
      this.surfaceH, `${dir}/hmrdp_gfx.bin`, this.replayRealtime, this.refMode);
    this.status = summary.startsWith('failed')
      ? `${summary}（需先把录制的 hmrdp_gfx.bin 放入应用 filesDir）`
      : summary;
    this.startStatsTimer();
  }

  // Switches the presentation line and mirrors it into the "硬件加速" setting
  // (persisted, so the session path sees the same value), then restarts: the
  // presenter is created per run, so a restart is what makes it take effect.
  private switchLine(): void {
    // 硬件加速 is not offered on a device whose Vulkan cannot present.
    this.line = (!this.hardwareAccelSupported || this.line === LINE_ACCEL) ? LINE_CPU : LINE_ACCEL;
    const settings: AppSettings = SettingsStore.load();
    settings.hardwareAccel = this.line === LINE_ACCEL;
    SettingsStore.save(settings);
    this.startReplay();
  }

  // Selects a Progressive decode worker count from the 「线程」 row. The value is
  // process-wide and applied before the next feed (see startReplay), so a
  // restart is what makes it take effect; re-selecting the current value is a
  // no-op, so a driver never pays for a redundant run.
  private selectDecodeThreads(index: number): void {
    if (this.decodeWorkerIndex === index) {
      return;
    }
    this.decodeWorkerIndex = index;
    this.startReplay();
  }

  // Selects the decode executor probe (see PAR_MODE_LABEL); same restart rule as
  // the 「线程」 row.
  private selectParMode(index: number): void {
    if (this.parModeIndex === index) {
      return;
    }
    this.parModeIndex = index;
    this.startReplay();
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

        // Presentation line: CPU (GLES) / 硬件加速 (Vulkan). Mirrors the
        // "硬件加速" setting and restarts the run.
        Button(`路线:${this.hardwareAccelSupported ? LINE_LABEL[this.line] : 'CPU'}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.switchLine();
          })

        // Golden-reference gate (CPU line): 关 / 导出 / 对比.
        Button(`参考:${REF_LABEL[this.refMode]}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.refMode = (this.refMode + 1) % REF_LABEL.length;
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
      }
      .width('100%')
      .padding({ left: 12, right: 12, top: 8, bottom: 8 })

      // Progressive decode worker count for the CPU (gdi) route: one button per
      // value, one click per point on the time-vs-energy curve (see
      // DECODE_THREAD_LABEL). The active one is filled in.
      Row() {
        ForEach(DECODE_THREAD_LABEL, (label: string, index: number) => {
          Button(`线程:${label}`)
            .height(32)
            .fontSize(13)
            .fontColor(this.decodeWorkerIndex === index ? $r('app.color.page_bg') : $r('app.color.accent'))
            .backgroundColor(this.decodeWorkerIndex === index ? $r('app.color.accent') : $r('app.color.input_bg'))
            .borderRadius(9)
            .margin({ right: 8 })
            .onClick(() => {
              this.selectDecodeThreads(index);
            })
        }, (label: string) => label)
      }
      .width('100%')
      .padding({ left: 12, right: 12, bottom: 8 })

      // Decode executor probe (see PAR_MODE_LABEL): one button per mode.
      Row() {
        ForEach(PAR_MODE_LABEL, (label: string, index: number) => {
          Button(`模式:${label}`)
            .height(32)
            .fontSize(13)
            .fontColor(this.parModeIndex === index ? $r('app.color.page_bg') : $r('app.color.accent'))
            .backgroundColor(this.parModeIndex === index ? $r('app.color.accent') : $r('app.color.input_bg'))
            .borderRadius(9)
            .margin({ right: 8 })
            .onClick(() => {
              this.selectParMode(index);
            })
        }, (label: string) => label)
      }
      .width('100%')
      .padding({ left: 12, right: 12, bottom: 8 })

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
            Text(`路线:${this.hardwareAccelSupported ? LINE_LABEL[this.line] : 'CPU'}  参考:${REF_LABEL[this.refMode]}  线程:${DECODE_THREAD_LABEL[this.decodeWorkerIndex]}  模式:${PAR_MODE_LABEL[this.parModeIndex]}`)
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
