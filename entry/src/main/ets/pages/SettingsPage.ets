/*
 * HmRdp - global settings.
 */
import { display } from '@kit.ArkUI';
import { AppSettings, SettingsStore } from '../services/SettingsStore';

@Entry
@Component
struct SettingsPage {
  @State hoverDelay: number = 350;
  @State hideDelay: number = 250;
  @State touchpadScrollUnit: number = 40;
  @State pinchZoomStep: number = 5;
  @State mainSizeEnabled: boolean = false;
  @State sessionSizeEnabled: boolean = false;
  @State mainWidthText: string = '';
  @State mainHeightText: string = '';
  @State sessionWidthText: string = '';
  @State sessionHeightText: string = '';
  private defaultMainWidth: number = 0;
  private defaultMainHeight: number = 0;
  private defaultSessionWidth: number = 0;
  private defaultSessionHeight: number = 0;

  aboutToAppear(): void {
    const defaults: AppSettings = SettingsStore.defaults();
    this.defaultMainWidth = defaults.mainWindowWidth;
    this.defaultMainHeight = defaults.mainWindowHeight;
    this.defaultSessionWidth = defaults.sessionWindowWidth;
    this.defaultSessionHeight = defaults.sessionWindowHeight;
    const settings: AppSettings = SettingsStore.load();
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
    this.touchpadScrollUnit = settings.touchpadScrollUnit;
    this.pinchZoomStep = settings.pinchZoomStepPercent;
    this.mainSizeEnabled = settings.mainWindowSizeEnabled;
    this.sessionSizeEnabled = settings.sessionWindowSizeEnabled;
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
    settings.touchpadScrollUnit = this.touchpadScrollUnit;
    settings.pinchZoomStepPercent = this.pinchZoomStep;
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
    this.touchpadScrollUnit = defaults.touchpadScrollUnit;
    this.pinchZoomStep = defaults.pinchZoomStepPercent;
    this.mainSizeEnabled = defaults.mainWindowSizeEnabled;
    this.sessionSizeEnabled = defaults.sessionWindowSizeEnabled;
    this.mainWidthText = this.sizeText(defaults.mainWindowWidth);
    this.mainHeightText = this.sizeText(defaults.mainWindowHeight);
    this.sessionWidthText = this.sizeText(defaults.sessionWindowWidth);
    this.sessionHeightText = this.sizeText(defaults.sessionWindowHeight);
    this.persist();
  }

  @Builder
  sliderItem(title: string, subtitle: string, value: number, min: number, max: number,
    step: number, unit: string, onChange: (v: number) => void) {
    Column() {
      Row() {
        Column() {
          Text(title).fontSize(14).fontColor($r('app.color.text_primary'))
          Text(subtitle).fontSize(11).fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(value.toString() + unit)
          .fontSize(14)
          .fontColor($r('app.color.accent'))
      }
      .width('100%')

      Slider({ value: value, min: min, max: max, step: step })
        .width('100%')
        .selectedColor($r('app.color.accent'))
        .trackColor($r('app.color.input_bg'))
        .margin({ top: 6 })
        .onChange((newValue: number, mode: SliderChangeMode) => {
          onChange(Math.round(newValue));
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

            this.sliderItem('工具栏触发停留时间', '鼠标停留在屏幕顶部多久后弹出工具栏',
              this.hoverDelay, 100, 1200, 50, ' ms', (v: number) => {
                this.hoverDelay = v;
                this.persist();
              })

            Divider().color($r('app.color.divider'))

            this.sliderItem('工具栏隐藏延迟', '鼠标移开后多久收起工具栏',
              this.hideDelay, 100, 1000, 50, ' ms', (v: number) => {
                this.hideDelay = v;
                this.persist();
              })

            Divider().color($r('app.color.divider'))

            Text('触控板')
              .fontSize(13)
              .fontColor($r('app.color.text_secondary'))
              .margin({ bottom: 4 })

            this.sliderItem('滚动步长', '触控板双指滑动多少位移换算为一格滚轮',
              this.touchpadScrollUnit, 10, 120, 5, ' vp', (v: number) => {
                this.touchpadScrollUnit = v;
                this.persist();
              })

            Divider().color($r('app.color.divider'))

            this.sliderItem('捏合缩放步进', '捏合比例每变化多少发一格 Ctrl+滚轮 缩放',
              this.pinchZoomStep, 2, 30, 1, ' %', (v: number) => {
                this.pinchZoomStep = v;
                this.persist();
              })

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
                  .enabled(this.sessionSizeEnabled)
                  .opacity(this.sessionSizeEnabled ? 1.0 : 0.4)
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
                  .enabled(this.sessionSizeEnabled)
                  .opacity(this.sessionSizeEnabled ? 1.0 : 0.4)
                  .onChange((value: string) => {
                    this.sessionHeightText = value;
                    this.persist();
                  })
              }
              .width('100%')
              .margin({ top: 10 })
            }
            .width('100%')
            .padding({ top: 14, bottom: 14 })
            .alignItems(HorizontalAlign.Start)
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
