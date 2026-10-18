@Component
export struct SettingSliderRow {
  @Prop title: string = '';
  @Prop subtitle: string = '';
  @Prop value: number = 0;
  @Prop min: number = 0;
  @Prop max: number = 100;
  @Prop step: number = 1;
  @Prop unit: string = '';
  @Prop precision: number = 0;
  @Prop switchEnabled: boolean = true;
  @Prop disabledReason: string = '';
  onChange: (v: number) => void = (v: number) => {};
  onCommit: () => void = () => {};

  build() {
    Column() {
      Row() {
        Column() {
          Text(this.title)
            .fontSize(14)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
          Text(this.subtitle)
            .fontSize(12)
            .lineHeight(16)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
          if (!this.switchEnabled && this.disabledReason.length > 0) {
            Text(this.disabledReason)
              .fontSize(12)
              .lineHeight(16)
              .fontColor($r('app.color.text_tertiary'))
              .margin({ top: 2 })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(this.value.toFixed(this.precision) + this.unit)
          .fontSize(14)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .fontFeature('"tnum" 1')
          .textAlign(TextAlign.End)
          .constraintSize({ minWidth: 56 })
      }
      .width('100%')

      Slider({ value: this.value, min: this.min, max: this.max, step: this.step })
        .enabled(this.switchEnabled)
        .selectedColor($r('app.color.brand_fill'))
        .trackColor($r('app.color.surface_sunken'))
        .blockColor($r('app.color.surface'))
        .opacity(this.switchEnabled ? 1.0 : 0.4)
        .width('100%')
        .margin({ top: 6 })
        .onChange((newValue: number, mode: SliderChangeMode) => {
          const factor: number = Math.pow(10, this.precision);
          this.onChange(Math.round(newValue * factor) / factor);
          if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
            this.onCommit();
          }
        })
    }
    .width('100%')
    .padding({ top: 14, bottom: 14 })
  }
}


