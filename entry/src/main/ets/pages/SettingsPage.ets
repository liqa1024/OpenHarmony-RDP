/*
 * HmRdp - global settings.
 */
import { common } from '@kit.AbilityKit';
import { display } from '@kit.ArkUI';
import { DisplayProfile } from '../model/RdpModels';
import { ConfigTransfer, ConfigTransferResult } from '../services/ConfigTransfer';
import { AppSettings, SCALE_PRESETS, SettingsStore } from '../services/SettingsStore';

// Remote desktop sizes are not limited to the local screen, so manual
// resolution is capped only to a sane upper bound.
const MAX_DESKTOP_SIZE: number = 16384;

// @Builder parameters are only reactive when passed as a single object literal
// (by reference); passing primitives by value would freeze the displayed value.
class SliderItemConfig {
  title: string = '';
  subtitle: string = '';
  value: number = 0;
  min: number = 0;
  max: number = 100;
  step: number = 1;
  unit: string = '';
  onChange: (v: number) => void = (v: number) => {};
}

@Entry
@Component
struct SettingsPage {
  @State hoverDelay: number = 350;
  @State hideDelay: number = 250;
  @State toolbarInWindowed: boolean = false;
  @State touchpadScrollUnit: number = 40;
  @State pinchZoomStep: number = 5;
  @State resolutionAuto: boolean = true;
  @State manualWidthText: string = '';
  @State manualHeightText: string = '';
  @State scaleAuto: boolean = true;
  @State manualScaleText: string = '100';
  @State detectedText: string = '';
  @State recommendedScale: number = 100;
  @State mainSizeEnabled: boolean = false;
  @State sessionSizeEnabled: boolean = false;
  @State sessionFullscreen: boolean = false;
  @State autoHideMainWindow: boolean = false;
  @State mainWidthText: string = '';
  @State mainHeightText: string = '';
  @State sessionWidthText: string = '';
  @State sessionHeightText: string = '';
  @State transferBusy: boolean = false;
  @State transferMessage: string = '';
  private defaultMainWidth: number = 0;
  private defaultMainHeight: number = 0;
  private defaultSessionWidth: number = 0;
  private defaultSessionHeight: number = 0;
  private detectedWidth: number = 0;
  private detectedHeight: number = 0;

  aboutToAppear(): void {
    const defaults: AppSettings = SettingsStore.defaults();
    this.defaultMainWidth = defaults.mainWindowWidth;
    this.defaultMainHeight = defaults.mainWindowHeight;
    this.defaultSessionWidth = defaults.sessionWindowWidth;
    this.defaultSessionHeight = defaults.sessionWindowHeight;
    const detected: DisplayProfile = SettingsStore.detectedDisplay();
    this.detectedText = detected.valid()
      ? `${detected.width} × ${detected.height}` : '无法获取显示器分辨率';
    this.detectedWidth = detected.width;
    this.detectedHeight = detected.height;
    this.recommendedScale = SettingsStore.recommendedScalePercent();
    this.syncFromStore();
  }

  /** Pulls the persisted settings back into the page state (also after import). */
  private syncFromStore(): void {
    const settings: AppSettings = SettingsStore.load();
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.toolbarInWindowed = settings.toolbarInWindowed;
    this.touchpadScrollUnit = settings.touchpadScrollUnit;
    this.pinchZoomStep = settings.pinchZoomStepPercent;
    this.resolutionAuto = settings.resolutionAuto;
    this.manualWidthText = this.sizeText(settings.manualWidth);
    this.manualHeightText = this.sizeText(settings.manualHeight);
    this.scaleAuto = settings.scaleAuto;
    this.manualScaleText = settings.manualScalePercent > 0
      ? settings.manualScalePercent.toString() : this.recommendedScale.toString();
    this.mainSizeEnabled = settings.mainWindowSizeEnabled;
    this.sessionSizeEnabled = settings.sessionWindowSizeEnabled;
    this.sessionFullscreen = settings.sessionFullscreen;
    this.autoHideMainWindow = settings.autoHideMainWindow;
    this.mainWidthText = this.sizeText(settings.mainWindowWidth);
    this.mainHeightText = this.sizeText(settings.mainWindowHeight);
    this.sessionWidthText = this.sizeText(settings.sessionWindowWidth);
    this.sessionHeightText = this.sizeText(settings.sessionWindowHeight);
  }

  private sizeText(value: number): string {
    return value > 0 ? value.toString() : '';
  }

  private parseSize(text: string, fallback: number, max: number): number {
    const value: number = Number.parseInt(text, 10);
    if (Number.isNaN(value) || value <= 0) {
      return fallback;
    }
    return Math.min(value, max);
  }

  private parseScale(text: string, fallback: number): number {
    const value: number = Number.parseInt(text, 10);
    if (Number.isNaN(value) || value < 100) {
      return fallback;
    }
    return Math.min(value, 500);
  }

  private parsedManualScale(): number {
    const value: number = Number.parseInt(this.manualScaleText, 10);
    return Number.isNaN(value) ? 0 : value;
  }

  private isPresetScale(value: number): boolean {
    return SCALE_PRESETS.indexOf(value) >= 0;
  }

  private displayOrUndefined(): display.Display | undefined {
    try {
      return display.getDefaultDisplaySync();
    } catch (error) {
      return undefined;
    }
  }

  private persist(): void {
    const disp: display.Display | undefined = this.displayOrUndefined();
    const maxWidth: number = disp !== undefined ? disp.width : Number.MAX_SAFE_INTEGER;
    const maxHeight: number = disp !== undefined ? disp.height : Number.MAX_SAFE_INTEGER;
    const settings: AppSettings = new AppSettings();
    settings.toolbarHoverDelay = this.hoverDelay;
    settings.toolbarHideDelay = this.hideDelay;
    settings.toolbarInWindowed = this.toolbarInWindowed;
    settings.touchpadScrollUnit = this.touchpadScrollUnit;
    settings.pinchZoomStepPercent = this.pinchZoomStep;
    settings.resolutionAuto = this.resolutionAuto;
    settings.manualWidth =
      this.parseSize(this.manualWidthText, this.detectedWidth, MAX_DESKTOP_SIZE);
    settings.manualHeight =
      this.parseSize(this.manualHeightText, this.detectedHeight, MAX_DESKTOP_SIZE);
    settings.scaleAuto = this.scaleAuto;
    settings.manualScalePercent = this.parseScale(this.manualScaleText, this.recommendedScale);
    settings.sessionFullscreen = this.sessionFullscreen;
    settings.autoHideMainWindow = this.autoHideMainWindow;
    settings.mainWindowSizeEnabled = this.mainSizeEnabled;
    settings.mainWindowWidth =
      this.parseSize(this.mainWidthText, this.defaultMainWidth, maxWidth);
    settings.mainWindowHeight =
      this.parseSize(this.mainHeightText, this.defaultMainHeight, maxHeight);
    settings.sessionWindowSizeEnabled = this.sessionSizeEnabled;
    settings.sessionWindowWidth =
      this.parseSize(this.sessionWidthText, this.defaultSessionWidth, maxWidth);
    settings.sessionWindowHeight =
      this.parseSize(this.sessionHeightText, this.defaultSessionHeight, maxHeight);
    SettingsStore.save(settings);
  }

  private resetDefaults(): void {
    const defaults: AppSettings = SettingsStore.defaults();
    this.hoverDelay = defaults.toolbarHoverDelay;
    this.hideDelay = defaults.toolbarHideDelay;
    this.toolbarInWindowed = defaults.toolbarInWindowed;
    this.touchpadScrollUnit = defaults.touchpadScrollUnit;
    this.pinchZoomStep = defaults.pinchZoomStepPercent;
    this.resolutionAuto = true;
    this.manualWidthText = this.sizeText(this.detectedWidth);
    this.manualHeightText = this.sizeText(this.detectedHeight);
    this.scaleAuto = true;
    this.manualScaleText = this.recommendedScale.toString();
    this.mainSizeEnabled = defaults.mainWindowSizeEnabled;
    this.sessionSizeEnabled = defaults.sessionWindowSizeEnabled;
    this.sessionFullscreen = defaults.sessionFullscreen;
    this.autoHideMainWindow = defaults.autoHideMainWindow;
    this.mainWidthText = this.sizeText(defaults.mainWindowWidth);
    this.mainHeightText = this.sizeText(defaults.mainWindowHeight);
    this.sessionWidthText = this.sizeText(defaults.sessionWindowWidth);
    this.sessionHeightText = this.sizeText(defaults.sessionWindowHeight);
    this.persist();
  }

  private confirmExport(): void {
    this.getUIContext().showAlertDialog({
      title: '导出配置',
      message: '将导出全局设置和全部连接。密码保存在系统安全存储中，出于安全考虑不会导出。',
      primaryButton: {
        value: '取消',
        action: () => {
        }
      },
      secondaryButton: {
        value: '导出',
        fontColor: $r('app.color.accent'),
        action: () => {
          this.handleExport();
        }
      }
    });
  }

  private async handleExport(): Promise<void> {
    if (this.transferBusy) {
      return;
    }
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      this.transferMessage = '无法获取应用上下文';
      return;
    }
    this.transferBusy = true;
    this.transferMessage = '';
    const result: ConfigTransferResult =
      await ConfigTransfer.exportToFile(context as common.UIAbilityContext);
    this.transferBusy = false;
    this.transferMessage = result.message;
  }

  private confirmImport(): void {
    this.getUIContext().showAlertDialog({
      title: '导入配置',
      message: '导入会应用全局设置，并按连接 ID 覆盖或新增连接。密码不会被导入，需重新输入。是否继续？',
      primaryButton: {
        value: '取消',
        action: () => {
        }
      },
      secondaryButton: {
        value: '导入',
        fontColor: $r('app.color.accent'),
        action: () => {
          this.handleImport();
        }
      }
    });
  }

  private async handleImport(): Promise<void> {
    if (this.transferBusy) {
      return;
    }
    const context = this.getUIContext().getHostContext();
    if (context === undefined) {
      this.transferMessage = '无法获取应用上下文';
      return;
    }
    this.transferBusy = true;
    this.transferMessage = '';
    const result: ConfigTransferResult =
      await ConfigTransfer.importFromFile(context as common.UIAbilityContext);
    this.transferBusy = false;
    this.transferMessage = result.message;
    if (result.success) {
      this.syncFromStore();
    }
  }

  @Builder
  resolutionModeChip(label: string, auto: boolean, leftMargin: number) {
    Text(label)
      .fontSize(13)
      .fontColor(this.resolutionAuto === auto ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.resolutionAuto === auto
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .onClick(() => {
        this.resolutionAuto = auto;
        this.persist();
      })
  }

  @Builder
  scaleModeChip(label: string, auto: boolean, leftMargin: number) {
    Text(label)
      .fontSize(13)
      .fontColor(this.scaleAuto === auto ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.scaleAuto === auto
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .onClick(() => {
        this.scaleAuto = auto;
        this.persist();
      })
  }

  @Builder
  scaleChip(preset: number) {
    Text(preset.toString() + '%')
      .fontSize(13)
      .fontColor(this.parsedManualScale() === preset ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.parsedManualScale() === preset
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .onClick(() => {
        this.manualScaleText = preset.toString();
        this.persist();
      })
  }

  @Builder
  sliderItem(item: SliderItemConfig) {
    Column() {
      Row() {
        Column() {
          Text(item.title).fontSize(14).fontColor($r('app.color.text_primary'))
          Text(item.subtitle).fontSize(11).fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(item.value.toString() + item.unit)
          .fontSize(14)
          .fontColor($r('app.color.accent'))
      }
      .width('100%')

      Slider({ value: item.value, min: item.min, max: item.max, step: item.step })
        .width('100%')
        .selectedColor($r('app.color.accent'))
        .trackColor($r('app.color.input_bg'))
        .margin({ top: 6 })
        .onChange((newValue: number, mode: SliderChangeMode) => {
          item.onChange(Math.round(newValue));
        })
    }
    .width('100%')
    .padding({ top: 14, bottom: 14 })
  }

  build() {
    Column() {
      Row() {
        Text('返回')
          .fontSize(14)
          .fontColor($r('app.color.accent'))
          .padding({ top: 8, bottom: 8 })
          .onClick(() => {
            this.getUIContext().getRouter().back();
          })
        Text('设置')
          .fontSize(17)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
          .textAlign(TextAlign.Center)
        Text('重置')
          .fontSize(14)
          .fontColor($r('app.color.accent'))
          .padding({ top: 8, bottom: 8 })
          .onClick(() => {
            this.resetDefaults();
          })
      }
      .width('100%')
      .padding({ left: 24, right: 24, top: 16, bottom: 12 })

      Divider().color($r('app.color.divider'))

      Scroll() {
        Column() {
          Column() {
            Text('会话交互')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            Row() {
              Column() {
                Text('窗口模式下显示工具栏').fontSize(14).fontColor($r('app.color.text_primary'))
                Text('全屏模式下始终显示工具栏')
                  .fontSize(11)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)

              Toggle({ type: ToggleType.Switch, isOn: this.toolbarInWindowed })
                .selectedColor($r('app.color.accent'))
                .onChange((isOn: boolean) => {
                  this.toolbarInWindowed = isOn;
                  this.persist();
                })
            }
            .width('100%')
            .height(58)

            Divider().color($r('app.color.divider'))

            this.sliderItem({
              title: '工具栏触发停留时间',
              subtitle: '鼠标停留在屏幕顶部多久后弹出工具栏',
              value: this.hoverDelay,
              min: 100,
              max: 1200,
              step: 50,
              unit: ' ms',
              onChange: (v: number) => {
                this.hoverDelay = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            this.sliderItem({
              title: '工具栏隐藏延迟',
              subtitle: '鼠标移开后多久收起工具栏',
              value: this.hideDelay,
              min: 100,
              max: 1000,
              step: 50,
              unit: ' ms',
              onChange: (v: number) => {
                this.hideDelay = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            Text('触控板')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            this.sliderItem({
              title: '滚动步长',
              subtitle: '触控板双指滑动多少位移换算为一格滚轮',
              value: this.touchpadScrollUnit,
              min: 10,
              max: 120,
              step: 5,
              unit: ' vp',
              onChange: (v: number) => {
                this.touchpadScrollUnit = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            this.sliderItem({
              title: '捏合缩放步进',
              subtitle: '捏合比例每变化多少发一格 Ctrl+滚轮 缩放',
              value: this.pinchZoomStep,
              min: 2,
              max: 30,
              step: 1,
              unit: ' %',
              onChange: (v: number) => {
                this.pinchZoomStep = v;
                this.persist();
              }
            })

            Divider().color($r('app.color.divider'))

            Text('显示')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            Column() {
              Row() {
                Column() {
                  Text('分辨率').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('自动使用当前显示器的分辨率')
                    .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Row() {
                  this.resolutionModeChip('自动', true, 0)
                  this.resolutionModeChip('手动', false, 8)
                }
              }
              .width('100%')

              if (this.resolutionAuto) {
                Text('自动检测：' + this.detectedText)
                  .fontSize(11).fontColor($r('app.color.text_tertiary'))
                  .width('100%').margin({ top: 10 })
              } else {
                Row() {
                  TextInput({ placeholder: '1920', text: this.manualWidthText })
                    .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
                    .backgroundColor($r('app.color.input_bg')).borderRadius(10)
                    .onChange((value: string) => {
                      this.manualWidthText = value;
                      this.persist();
                    })

                  Text('×')
                    .fontSize(14).fontColor($r('app.color.text_tertiary'))
                    .margin({ left: 10, right: 10 })

                  TextInput({ placeholder: '1080', text: this.manualHeightText })
                    .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
                    .backgroundColor($r('app.color.input_bg')).borderRadius(10)
                    .onChange((value: string) => {
                      this.manualHeightText = value;
                      this.persist();
                    })
                }
                .width('100%').margin({ top: 10 })

                Text('自动检测：' + this.detectedText + '，可参考输入')
                  .fontSize(11).fontColor($r('app.color.text_tertiary'))
                  .width('100%').margin({ top: 6 })
              }
            }
            .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            Column() {
              Row() {
                Column() {
                  Text('缩放比例').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('按屏幕密度自动选择推荐缩放比例')
                    .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Row() {
                  this.scaleModeChip('自动', true, 0)
                  this.scaleModeChip('手动', false, 8)
                }
              }
              .width('100%')

              if (this.scaleAuto) {
                Text('推荐：' + this.recommendedScale + '%')
                  .fontSize(11).fontColor($r('app.color.text_tertiary'))
                  .width('100%').margin({ top: 10 })
              } else {
                Flex({ wrap: FlexWrap.Wrap }) {
                  ForEach(SCALE_PRESETS, (preset: number) => {
                    this.scaleChip(preset)
                  }, (preset: number) => preset.toString())
                }
                .width('100%').margin({ top: 10 })

                Row() {
                  TextInput({ placeholder: '100', text: this.manualScaleText })
                    .type(InputType.Number).fontSize(14).height(40).layoutWeight(1)
                    .backgroundColor($r('app.color.input_bg')).borderRadius(10)
                    .onChange((value: string) => {
                      this.manualScaleText = value;
                      this.persist();
                    })

                  Text('%')
                    .fontSize(14).fontColor($r('app.color.text_tertiary'))
                    .margin({ left: 10 })
                }
                .width('100%')

                if (this.parsedManualScale() > 0 && !this.isPresetScale(this.parsedManualScale())) {
                  Text('自定义缩放比例可能导致部分应用显示模糊，建议使用上方固定档位')
                    .fontSize(11).fontColor($r('app.color.danger'))
                    .width('100%').margin({ top: 6 })
                }
              }
            }
            .width('100%').padding({ top: 14, bottom: 14 }).alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            Text('窗口大小')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            Column() {
              Row() {
                Column() {
                  Text('默认主窗口尺寸').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('主窗口创建时使用的大小，下次启动生效（像素）')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.mainSizeEnabled })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.mainSizeEnabled = isOn;
                    this.persist();
                  })
              }
              .width('100%')

              Row() {
                TextInput({ text: this.mainWidthText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.mainSizeEnabled)
                  .opacity(this.mainSizeEnabled ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.mainWidthText = value;
                    this.persist();
                  })

                Text('×')
                  .fontSize(14)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ left: 10, right: 10 })

                TextInput({ text: this.mainHeightText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.mainSizeEnabled)
                  .opacity(this.mainSizeEnabled ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.mainHeightText = value;
                    this.persist();
                  })
              }
              .width('100%')
              .margin({ top: 10 })
            }
            .width('100%')
            .padding({ top: 14, bottom: 14 })
            .alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            Column() {
              Row() {
                Column() {
                  Text('默认会话窗口尺寸').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('会话窗口创建时使用的大小，下次连接生效（像素）')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.sessionSizeEnabled })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.sessionSizeEnabled = isOn;
                    this.persist();
                  })
              }
              .width('100%')

              Row() {
                TextInput({ text: this.sessionWidthText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.sessionSizeEnabled && !this.sessionFullscreen)
                  .opacity(this.sessionSizeEnabled && !this.sessionFullscreen ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.sessionWidthText = value;
                    this.persist();
                  })

                Text('×')
                  .fontSize(14)
                  .fontColor($r('app.color.text_tertiary'))
                  .margin({ left: 10, right: 10 })

                TextInput({ text: this.sessionHeightText })
                  .type(InputType.Number)
                  .fontSize(14)
                  .height(40)
                  .layoutWeight(1)
                  .backgroundColor($r('app.color.input_bg'))
                  .borderRadius(10)
                  .enabled(this.sessionSizeEnabled && !this.sessionFullscreen)
                  .opacity(this.sessionSizeEnabled && !this.sessionFullscreen ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.sessionHeightText = value;
                    this.persist();
                  })
              }
              .width('100%')
              .margin({ top: 10 })

              Row() {
                Column() {
                  Text('默认最大化（全屏）').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('会话窗口打开时直接最大化到全屏，忽略上面的尺寸')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.sessionFullscreen })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.sessionFullscreen = isOn;
                    this.persist();
                  })
              }
              .width('100%')
              .height(58)
              .margin({ top: 4 })

              Divider().color($r('app.color.divider'))

              Row() {
                Column() {
                  Text('自动隐藏主窗口').fontSize(14).fontColor($r('app.color.text_primary'))
                  Text('单窗口模式：会话窗口打开时隐藏主窗口，关闭后恢复（不支持多会话）')
                    .fontSize(11)
                    .fontColor($r('app.color.text_tertiary'))
                    .margin({ top: 2 })
                }
                .alignItems(HorizontalAlign.Start)
                .layoutWeight(1)

                Toggle({ type: ToggleType.Switch, isOn: this.autoHideMainWindow })
                  .selectedColor($r('app.color.accent'))
                  .onChange((isOn: boolean) => {
                    this.autoHideMainWindow = isOn;
                    this.persist();
                  })
              }
              .width('100%')
              .height(58)
              .margin({ top: 4 })
            }
            .width('100%')
            .padding({ top: 14, bottom: 14 })
            .alignItems(HorizontalAlign.Start)

            Divider().color($r('app.color.divider'))

            Text('配置备份')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ top: 4, bottom: 4 })

            Text('将全局设置和全部连接导出为一个文件，或从文件导入并覆盖。密码保存在系统安全存储中，不会被导出，导入后需重新输入。')
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .margin({ bottom: 12 })

            Row() {
              Button('导出配置')
                .layoutWeight(1)
                .height(44)
                .fontSize(15)
                .fontColor($r('app.color.accent'))
                .backgroundColor($r('app.color.input_bg'))
                .borderRadius(12)
                .enabled(!this.transferBusy)
                .onClick(() => {
                  this.confirmExport();
                })

              Button('导入配置')
                .layoutWeight(1)
                .height(44)
                .fontSize(15)
                .fontColor('#FFFFFF')
                .backgroundColor($r('app.color.accent'))
                .borderRadius(12)
                .margin({ left: 12 })
                .enabled(!this.transferBusy)
                .onClick(() => {
                  this.confirmImport();
                })
            }
            .width('100%')

            if (this.transferBusy || this.transferMessage.length > 0) {
              Row() {
                if (this.transferBusy) {
                  LoadingProgress()
                    .width(16)
                    .height(16)
                    .color($r('app.color.accent'))
                }
                Text(this.transferBusy ? '处理中…' : this.transferMessage)
                  .fontSize(12)
                  .fontColor($r('app.color.text_secondary'))
                  .layoutWeight(1)
                  .margin({ left: this.transferBusy ? 8 : 0 })
              }
              .width('100%')
              .margin({ top: 10 })
            }
          }
          .width('100%')
          .constraintSize({ maxWidth: 720 })
          .padding(24)
          .backgroundColor($r('app.color.card_bg'))
          .borderRadius(20)
          .alignItems(HorizontalAlign.Start)
          .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
        }
        .width('100%')
        .padding({ top: 24, bottom: 32, left: 24, right: 24 })
        .alignItems(HorizontalAlign.Center)
      }
      .layoutWeight(1)
      .scrollBar(BarState.Auto)
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.page_bg'))
  }
}
