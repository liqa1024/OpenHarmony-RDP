export enum StateViewKind {
  Loading = 0,
  Empty = 1,
  Error = 2
}

@Component
export struct StateView {
  @Prop kind: number = StateViewKind.Empty;
  @Prop title: string = '';
  @Prop message: string = '';
  @Prop icon: Resource | undefined = undefined;
  @Prop primaryLabel: string = '';
  @Prop secondaryLabel: string = '';
  onPrimary: () => void = () => {};
  onSecondary: () => void = () => {};

  build() {
    Column() {
      Column() {
        if (this.kind === StateViewKind.Loading) {
          LoadingProgress()
            .width(40)
            .height(40)
            .color($r('app.color.brand_fill'))
        } else if (this.icon !== undefined) {
          Image(this.icon)
            .width(52)
            .height(52)
            .backgroundColor($r('app.color.surface_subtle'))
            .border({ width: 1, color: $r('app.color.control_border') })
            .borderRadius(16)
            .clip(true)
            .draggable(false)
        }

        if (this.title.length > 0) {
          Text(this.title)
            .fontSize(18)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .margin({ top: this.kind === StateViewKind.Loading ? 16 : 18 })
        }

        if (this.message.length > 0) {
          Text(this.message)
            .fontSize(12)
            .lineHeight(18)
            .fontColor(this.kind === StateViewKind.Error
              ? $r('app.color.text_secondary') : $r('app.color.text_tertiary'))
            .textAlign(TextAlign.Center)
            .constraintSize({ maxWidth: 320 })
            .margin({ top: 8 })
        }

        if (this.primaryLabel.length > 0 || this.secondaryLabel.length > 0) {
          Row() {
            if (this.primaryLabel.length > 0) {
              Button(this.primaryLabel)
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
            if (this.secondaryLabel.length > 0) {
              Button(this.secondaryLabel)
                .height(44)
                .fontSize(16)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_primary'))
                .backgroundColor($r('app.color.control_bg'))
                .border({ width: 1, color: $r('app.color.control_border') })
                .borderRadius(12)
                .margin({ left: this.primaryLabel.length > 0 ? 12 : 0 })
                .onClick(() => {
                  this.onSecondary();
                })
            }
          }
          .margin({ top: 24 })
        }
      }
      .width('90%')
      .constraintSize({ maxWidth: 360 })
      .padding(24)
      .backgroundColor($r('app.color.surface'))
      .borderRadius(20)
      .border({ width: 1, color: $r('app.color.border') })
      .shadow({ radius: 8, color: $r('app.color.shadow_1'), offsetX: 0, offsetY: 2 })
      .alignItems(HorizontalAlign.Center)
    }
    .width('100%')
    .height('100%')
    .padding({ left: 24, right: 24 })
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
  }
}
