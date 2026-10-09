@Component
export struct SettingToggleRow {
  @Prop title: string = '';
  @Prop subtitle: string = '';
  @Prop value: boolean = false;
  @Prop switchEnabled: boolean = true;
  @Prop disabledReason: string = '';
  onToggle: (v: boolean) => void = (v: boolean) => {};

  build() {
    Row() {
      Column() {
        Text(this.title)
          .fontSize(14)
          .fontColor(this.switchEnabled ? $r('app.color.text_primary') : $r('app.color.text_tertiary'))
        if (this.subtitle.length > 0) {
          Text(this.subtitle)
            .fontSize(11)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
        if (!this.switchEnabled && this.disabledReason.length > 0) {
          Text(this.disabledReason)
            .fontSize(11)
            .fontColor($r('app.color.danger'))
            .margin({ top: 2 })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Toggle({ type: ToggleType.Switch, isOn: this.value })
        .selectedColor($r('app.color.accent'))
        .enabled(this.switchEnabled)
        .onChange((isOn: boolean) => {
          if (!this.switchEnabled) {
            return;
          }
          this.onToggle(isOn);
        })
    }
    .width('100%')
    .height((this.subtitle.length > 0 ? 58 : 48)
      + (!this.switchEnabled && this.disabledReason.length > 0 ? 20 : 0))
  }
}
