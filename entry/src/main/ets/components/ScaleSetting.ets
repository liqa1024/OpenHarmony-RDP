import { SCALE_PRESETS } from '../services/SettingsStore';
import { isScalePreset, parsedTextValue } from '../utils/ScaleParse';
import { ChoiceChip } from './ChoiceChip';

@Component
export struct ScaleSetting {
  @Prop mode: number = 0;
  @Prop allowNone: boolean = false;
  @Prop allowCustomInput: boolean = true;
  @Prop percentText: string = '100';
  @Prop autoPercent: number = 100;
  @Prop title: string = '缩放比例';
  @Prop subtitle: string = '';
  @Prop noneHint: string = '';
  @Prop warning: string = '';
  @Prop customWarning: string = '自定义缩放比例可能导致部分应用显示模糊，建议使用上方固定档位';
  onModeChange: (mode: number) => void = (mode: number) => {};
  onPercentTextChange: (text: string) => void = (text: string) => {};

  private parsedPercent(): number {
    return parsedTextValue(this.percentText);
  }

  private showsWarning(): boolean {
    return this.warning.length > 0 && !(this.allowNone && this.mode === 2);
  }

  @Builder
  modeChip(label: string, value: number, leftMargin: number) {
    Text(label)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor(this.mode === value ? $r('app.color.brand_text') : $r('app.color.text_secondary'))
      .height(32)
      .textAlign(TextAlign.Center)
      .backgroundColor(this.mode === value ? $r('app.color.brand_tint') : $r('app.color.control_bg'))
      .border({
        width: 1,
        color: this.mode === value
          ? $r('app.color.brand_tint_strong') : $r('app.color.control_border')
      })
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .responseRegion({ x: 0, y: -4, width: '100%', height: 40 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.onModeChange(value);
      })
      .focusable(true)
      .focusOnTouch(true)
  }

  @Builder
  presetChip(preset: number) {
    ChoiceChip({
      label: preset.toString() + '%',
      selected: this.parsedPercent() === preset,
      onTap: () => {
        this.onPercentTextChange(preset.toString());
      }
    })
  }

  build() {
    Column() {
      Column() {
        Text(this.title)
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
        if (this.subtitle.length > 0) {
          Text(this.subtitle)
            .fontSize(12)
            .lineHeight(16)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
        if (this.showsWarning()) {
          Text(this.warning)
            .fontSize(12)
            .lineHeight(16)
            .fontColor($r('app.color.warning'))
            .margin({ top: 2 })
        }

        Flex({ wrap: FlexWrap.Wrap }) {
          this.modeChip('自动', 0, 0)
          this.modeChip('手动', 1, 8)
          if (this.allowNone) {
            this.modeChip('不额外设置', 2, 8)
          }
        }
        .width('100%')
        .margin({ top: 10 })
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      if (this.mode === 1) {
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(SCALE_PRESETS, (preset: number) => {
            this.presetChip(preset)
          }, (preset: number) => preset.toString())
        }
        .width('100%').margin({ top: 10 })

        if (this.allowCustomInput) {
          TextInput({ placeholder: '100', text: this.percentText })
            .type(InputType.Number)
            .fontSize(14)
            .fontColor($r('app.color.text_primary'))
            .placeholderColor($r('app.color.text_tertiary'))
            .height(44)
            .width('100%')
            .backgroundColor($r('app.color.surface_subtle'))
            .borderRadius(10)
            .onChange((value: string) => {
              this.onPercentTextChange(value);
            })

          if (this.parsedPercent() > 0 && !isScalePreset(this.parsedPercent())) {
            Text(this.customWarning)
              .fontSize(12)
              .lineHeight(16)
              .fontColor($r('app.color.warning'))
              .width('100%')
              .margin({ top: 6 })
          }
        }
      } else if (this.mode === 0) {
        Text('推荐 ' + this.autoPercent + '%')
          .fontSize(12)
          .fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 10 })
      } else if (this.allowNone && this.mode === 2 && this.noneHint.length > 0) {
        Text(this.noneHint)
          .fontSize(12)
          .lineHeight(16)
          .fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 10 })
      }
    }
    .width('100%').alignItems(HorizontalAlign.Start)
  }
}


