/*
 * HmRdp - global settings.
 */
import { AppSettings, SettingsStore } from '../services/SettingsStore';

@Entry
@Component
struct SettingsPage {
  @State hoverDelay: number = 350;
  @State hideDelay: number = 250;

  aboutToAppear(): void {
    const settings: AppSettings = SettingsStore.load();
    this.hoverDelay = settings.toolbarHoverDelay;
    this.hideDelay = settings.toolbarHideDelay;
  }

  private persist(): void {
    const settings: AppSettings = new AppSettings();
    settings.toolbarHoverDelay = this.hoverDelay;
    settings.toolbarHideDelay = this.hideDelay;
    SettingsStore.save(settings);
  }

  private resetDefaults(): void {
    this.hoverDelay = 350;
    this.hideDelay = 250;
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
