@Component
export struct StatusCard {
  @Prop title: string = '';
  @Prop subtitle: string = '';
  @Prop primary: string = '';
  @Prop secondary: string = '';
  @Prop loading: boolean = false;
  @Prop icon: Resource | undefined = undefined;
  onPrimary: () => void = () => {};
  onSecondary: () => void = () => {};

  build() {
    Column() {
      if (this.loading) {
        LoadingProgress()
          .width(44)
          .height(44)
          .color($r('app.color.chrome_accent'))
      } else if (this.icon !== undefined) {
        Image(this.icon)
          .width(52)
          .height(52)
          .backgroundColor($r('app.color.chrome_hover'))
          .border({ width: 1, color: $r('app.color.chrome_border') })
          .borderRadius(16)
          .clip(true)
          .draggable(false)
      }
      Text(this.title)
        .fontSize(20)
        .fontWeight(600)
        .fontColor($r('app.color.chrome_text_primary'))
        .margin({ top: 18 })
      Text(this.subtitle)
        .fontSize(12)
        .lineHeight(18)
        .fontColor($r('app.color.chrome_text_secondary'))
        .margin({ top: 8 })
        .textAlign(TextAlign.Center)
        .constraintSize({ maxWidth: 320 })
      Row() {
        if (this.primary.length > 0) {
          Button(this.primary)
            .height(44)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.on_brand'))
            .backgroundColor($r('app.color.brand_fill'))
            .borderRadius(12)
            .onClick(() => {
              this.onPrimary();
            })
        }
        if (this.secondary.length > 0) {
          Button(this.secondary)
            .height(44)
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.chrome_text_primary'))
            .backgroundColor($r('app.color.chrome_hover'))
            .borderRadius(12)
            .margin({ left: 12 })
            .onClick(() => {
              this.onSecondary();
            })
        }
      }
      .margin({ top: 24 })
    }
    .width('90%')
    .constraintSize({ maxWidth: 360 })
    .padding({ top: 32, bottom: 28, left: 28, right: 28 })
    .borderRadius(20)
    .backgroundColor($r('app.color.chrome_bg'))
    .backgroundBlurStyle(BlurStyle.COMPONENT_THICK)
    .border({ width: 1, color: $r('app.color.chrome_border') })
    .shadow({ radius: 16, color: $r('app.color.shadow_2'), offsetX: 0, offsetY: 4 })
    .alignItems(HorizontalAlign.Center)
  }
}
