import { RdpNative } from '../services/RdpNative';
import { Capability, DeviceCapabilities } from '../services/DeviceCapabilities';
import { window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpReplay';

// 「路线」: CPU = the GLES presenter (no Vulkan at all), 硬件加速 = the Vulkan presenter.
const LINE_CPU: number = 0;
const LINE_ACCEL: number = 1;
const LINE_LABEL: string[] = ['CPU', '硬件加速'];

const REF_OFF: number = 0;
const REF_EXPORT: number = 1;
const REF_COMPARE: number = 2;
const REF_LABEL: string[] = ['关', '导出', '对比'];

const MAX_SLOTS: number = 4;

class ReplaySlot {
  readonly index: number;
  surfaceId: string = '';
  width: number = 0;
  height: number = 0;
  handle: number = -1;

  constructor(index: number) {
    this.index = index;
  }
}

class ReplaySurfaceController extends XComponentController {
  private slot: ReplaySlot;
  private createdCb: (slot: ReplaySlot, surfaceId: string, width: number, height: number) => void;
  private changedCb: (slot: ReplaySlot, width: number, height: number) => void;
  private destroyedCb: (slot: ReplaySlot) => void;

  constructor(slot: ReplaySlot,
    created: (slot: ReplaySlot, surfaceId: string, width: number, height: number) => void,
    changed: (slot: ReplaySlot, width: number, height: number) => void,
    destroyed: (slot: ReplaySlot) => void) {
    super();
    this.slot = slot;
    this.createdCb = created;
    this.changedCb = changed;
    this.destroyedCb = destroyed;
  }

  onSurfaceCreated(surfaceId: string): void {
    const rect: SurfaceRect = this.getXComponentSurfaceRect();
    this.createdCb(this.slot, surfaceId, rect.surfaceWidth, rect.surfaceHeight);
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    this.changedCb(this.slot, rect.surfaceWidth, rect.surfaceHeight);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    this.destroyedCb(this.slot);
  }
}

@Entry
@Component
struct GfxReplayPage {
  @State status: string = '等待 surface...';
  @State line: number = LINE_ACCEL;
  @State refMode: number = REF_OFF;
  @State replayRealtime: boolean = false;
  @State slotCount: number = 3;
  @State hardwareAccelSupported: boolean = true;
  @State showStats: boolean = true;
  private slots: ReplaySlot[] = [];
  private controllers: ReplaySurfaceController[] = [];
  private statsTimer: number = -1;
  private capturePath: string = '';

  private accelOn(): boolean {
    return this.line === LINE_ACCEL && this.hardwareAccelSupported;
  }

  aboutToAppear(): void {
    this.applyFullScreen(true);
    const capability: Capability = DeviceCapabilities.hardwareAccel();
    this.hardwareAccelSupported = capability.supported;
    // Presentation is Vulkan whenever the device supports it; this page's route toggle is dev-only
    // A/B (it is not persisted), so start on the supported route.
    this.line = capability.supported ? LINE_ACCEL : LINE_CPU;
    let dir: string = '';
    try {
      const ctx: common.Context | undefined = this.getUIContext().getHostContext();
      if (ctx !== undefined) {
        dir = ctx.filesDir;
      }
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'filesDir unavailable: %{public}s', JSON.stringify(error));
    }
    this.capturePath = dir.length > 0 ? `${dir}/hmrdp_gfx.bin` : '';
    this.buildSlots();
    this.startStatsTimer();
  }

  aboutToDisappear(): void {
    this.stopStatsTimer();
    RdpNative.stopAllReplayInstances();
    this.applyFullScreen(false);
  }

  // Controllers live for the page's lifetime, so a slot count change only
  // mounts/unmounts surfaces; the handles bound to them stay addressable.
  private buildSlots(): void {
    this.slots = [];
    this.controllers = [];
    for (let i = 0; i < MAX_SLOTS; i++) {
      const slot: ReplaySlot = new ReplaySlot(i);
      this.slots.push(slot);
      this.controllers.push(new ReplaySurfaceController(slot,
        (s: ReplaySlot, id: string, w: number, h: number) => {
          this.onSurfaceCreated(s, id, w, h);
        },
        (s: ReplaySlot, w: number, h: number) => {
          this.onSurfaceChanged(s, w, h);
        },
        (s: ReplaySlot) => {
          this.onSurfaceDestroyed(s);
        }));
    }
  }

  private activeIndexes(): number[] {
    const out: number[] = [];
    for (let i = 0; i < this.slotCount; i++) {
      out.push(i);
    }
    return out;
  }

  private onSurfaceCreated(slot: ReplaySlot, surfaceId: string, width: number, height: number): void {
    hilog.info(DOMAIN, TAG, 'surface %{public}d created %{public}s %{public}dx%{public}d',
      slot.index, surfaceId, width, height);
    slot.surfaceId = surfaceId;
    slot.width = width;
    slot.height = height;
    this.startSlot(slot);
  }

  private onSurfaceChanged(slot: ReplaySlot, width: number, height: number): void {
    slot.width = width;
    slot.height = height;
    if (slot.handle > 0) {
      RdpNative.resizeReplayInstance(slot.handle, width, height);
    }
  }

  private onSurfaceDestroyed(slot: ReplaySlot): void {
    RdpNative.stopReplayInstance(slot.handle);
    slot.handle = -1;
  }

  private startSlot(slot: ReplaySlot): void {
    if (this.capturePath.length === 0 || slot.surfaceId.length === 0 ||
      slot.width <= 0 || slot.height <= 0) {
      this.status = '需先把录制的 hmrdp_gfx.bin 放入应用 filesDir';
      hilog.warn(DOMAIN, TAG,
        'slot %{public}d start skipped: path=%{public}s surface=%{public}s %{public}dx%{public}d',
        slot.index, this.capturePath, slot.surfaceId, slot.width, slot.height);
      return;
    }
    // The presenter backend is chosen when the run starts, so apply the
    // acceleration state here as well (it may have changed since page open).
    RdpNative.applyHardwareAccel(this.accelOn());
    slot.handle = RdpNative.startReplayInstance(slot.surfaceId, slot.width, slot.height,
      this.capturePath, this.replayRealtime, this.refMode);
    hilog.info(DOMAIN, TAG, 'slot %{public}d handle=%{public}d', slot.index, slot.handle);
  }

  // The presenter is created per instance, so a restart is what makes a
  // route/reference/clock change take effect.
  private restartAll(): void {
    RdpNative.stopAllReplayInstances();
    for (let i = 0; i < this.slotCount; i++) {
      const slot: ReplaySlot = this.slots[i];
      slot.handle = -1;
      this.startSlot(slot);
    }
  }

  private switchLine(): void {
    this.line = (!this.hardwareAccelSupported || this.line === LINE_ACCEL) ? LINE_CPU : LINE_ACCEL;
    this.restartAll();
  }

  private cycleSlots(): void {
    this.slotCount = this.slotCount >= MAX_SLOTS ? 1 : this.slotCount + 1;
    this.restartAll();
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
      this.status = RdpNative.listReplayInstances();
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

        Button(`并发:${this.slotCount}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.cycleSlots();
          })

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

        Button(`参考:${REF_LABEL[this.refMode]}`)
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .margin({ right: 8 })
          .onClick(() => {
            this.refMode = (this.refMode + 1) % REF_LABEL.length;
            this.restartAll();
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
            this.restartAll();
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

        Button('重放')
          .height(34)
          .fontSize(13)
          .fontColor($r('app.color.accent'))
          .backgroundColor($r('app.color.input_bg'))
          .borderRadius(9)
          .onClick(() => {
            this.restartAll();
          })
      }
      .width('100%')
      .padding({ left: 12, right: 12, top: 8, bottom: 8 })

      Stack({ alignContent: Alignment.BottomStart }) {
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(this.activeIndexes(), (index: number) => {
            XComponent({
              id: `replay_${index}`,
              type: XComponentType.SURFACE,
              controller: this.controllers[index]
            })
              .width(this.slotCount > 1 ? '50%' : '100%')
              .height(this.slotCount > 2 ? '50%' : '100%')
              .backgroundColor('#000000')
          }, (index: number) => `slot_${index}`)
        }
        .width('100%')
        .height('100%')

        if (this.showStats) {
          Column() {
            Text(`并发:${this.slotCount}  路线:${this.hardwareAccelSupported ? LINE_LABEL[this.line] : 'CPU'}  参考:${REF_LABEL[this.refMode]}`)
              .fontSize(12)
              .fontColor('#FFFFD479')
              .width('100%')
            Text(this.status)
              .fontSize(12)
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
