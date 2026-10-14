import { SCALE_PRESETS } from '../services/SettingsStore';
import { isScalePreset, parsedTextValue } from '../utils/ScaleParse';
import { ChoiceChip } from './ChoiceChip';

// 缩放设置块：RDP（会话桌面 DPI 系数）与 Moonlight（基地版虚拟显示器缩放）共用。
// mode 0 = 自动、1 = 手动、2 = 不额外设置（仅 allowNone 时出现）。
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

  // 「不额外设置」不向主机发请求，能力类警告无意义。
  private showsWarning(): boolean {
    return this.warning.length > 0 && !(this.allowNone && this.mode === 2);
  }

  @Builder
  modeChip(label: string, value: number, leftMargin: number) {
    Text(label)
      .fontSize(13)
      .fontColor(this.mode === value ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.mode === value
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ left: leftMargin })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.onModeChange(value);
      })
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
      Row() {
        Column() {
          Text(this.title).fontSize(14).fontColor($r('app.color.text_primary'))
          if (this.subtitle.length > 0) {
            Text(this.subtitle)
              .fontSize(11).fontColor($r('app.color.text_tertiary')).margin({ top: 2 })
          }
          if (this.showsWarning()) {
            Text(this.warning)
              .fontSize(11).fontColor($r('app.color.danger')).margin({ top: 2 })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Row() {
          this.modeChip('自动', 0, 0)
          this.modeChip('手动', 1, 8)
          if (this.allowNone) {
            this.modeChip('不额外设置', 2, 8)
          }
        }
      }
      .width('100%')

      if (this.mode === 1) {
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(SCALE_PRESETS, (preset: number) => {
            this.presetChip(preset)
          }, (preset: number) => preset.toString())
        }
        .width('100%').margin({ top: 10 })

        if (this.allowCustomInput) {
          TextInput({ placeholder: '100', text: this.percentText })
            .type(InputType.Number).fontSize(14).height(40).width('100%')
            .backgroundColor($r('app.color.input_bg')).borderRadius(10)
            .onChange((value: string) => {
              this.onPercentTextChange(value);
            })

          if (this.parsedPercent() > 0 && !isScalePreset(this.parsedPercent())) {
            Text(this.customWarning)
              .fontSize(11).fontColor($r('app.color.danger'))
              .width('100%').margin({ top: 6 })
          }
        }
      } else if (this.mode === 0) {
        Text('推荐 ' + this.autoPercent + '%')
          .fontSize(11).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 10 })
      } else if (this.allowNone && this.mode === 2 && this.noneHint.length > 0) {
        Text(this.noneHint)
          .fontSize(11).fontColor($r('app.color.text_tertiary'))
          .width('100%').margin({ top: 10 })
      }
    }
    .width('100%').alignItems(HorizontalAlign.Start)
  }
}
