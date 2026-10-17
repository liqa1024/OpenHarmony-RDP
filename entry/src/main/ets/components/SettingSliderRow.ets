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

  build() {
    Column() {
      Row() {
        Column() {
          Text(this.title)
            .fontSize(14)
            
          Text(this.subtitle)
            .fontSize(11)
            
            .margin({ top: 2 })
          if (!this.switchEnabled && this.disabledReason.length > 0) {
            Text(this.disabledReason)
              .fontSize(11)
              
              .margin({ top: 2 })
          }
        }
        .alignItems(HorizontalAlign.Start)
        .layoutWeight(1)

        Text(this.value.toFixed(this.precision) + this.unit)
          .fontSize(14)
          
      }
      .width('100%')

      Slider({ value: this.value, min: this.min, max: this.max, step: this.step })
        .enabled(this.switchEnabled)
        .width('100%')
        
        
        
        .margin({ top: 6 })
        .onChange((newValue: number, mode: SliderChangeMode) => {
          const factor: number = Math.pow(10, this.precision);
          this.onChange(Math.round(newValue * factor) / factor);
        })
    }
    .width('100%')
    .padding({ top: 14, bottom: 14 })
  }
}


