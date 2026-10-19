@Component
export struct PromptDialog {
  @Prop title: string = '';
  @Prop message: string = '';
  @Prop cancelLabel: string = '';
  @Prop confirmLabel: string = '';
  @Prop danger: boolean = false;
  onCancel: () => void = () => {};
  onConfirm: () => void = () => {};

  @Builder
  emptyContent() {
  }

  @BuilderParam content: () => void = this.emptyContent;

  build() {
    Column() {
      if (this.title.length > 0) {
        Text(this.title)
          .fontSize(20)
          .fontWeight(600)
          .fontColor($r('app.color.text_primary'))
          .textAlign(TextAlign.Center)
          .width('100%')
      }
      if (this.message.length > 0) {
        Text(this.message)
          .fontSize(14)
          .lineHeight(20)
          .fontColor($r('app.color.text_secondary'))
          .textAlign(TextAlign.Center)
          .width('100%')
          .margin({ top: 8 })
      }

      this.content()

      if (this.cancelLabel.length > 0 || this.confirmLabel.length > 0) {
        Row({ space: 12 }) {
          if (this.cancelLabel.length > 0) {
            Button(this.cancelLabel)
              .layoutWeight(1)
              .height(44)
              .fontSize(16)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
              .backgroundColor($r('app.color.control_bg'))
              .border({ width: 1, color: $r('app.color.control_border') })
              .borderRadius(12)
              .onClick(() => {
                this.onCancel();
              })
          }
          if (this.confirmLabel.length > 0) {
            Button(this.confirmLabel)
              .layoutWeight(1)
              .height(44)
              .fontSize(16)
              .fontWeight(FontWeight.Medium)
              .fontColor(this.danger
                ? $r('app.color.danger') : $r('app.color.on_brand'))
              .backgroundColor(this.danger
                ? $r('app.color.danger_tint') : $r('app.color.brand_fill'))
              .borderRadius(12)
              .onClick(() => {
                this.onConfirm();
              })
          }
        }
        .width('100%')
        .margin({ top: 24 })
      }
    }
    .width('90%')
    .constraintSize({ maxWidth: 360 })
    .padding(24)
    .backgroundColor($r('app.color.surface'))
    .border({ width: 1, color: $r('app.color.border') })
    .shadow({ radius: 16, color: $r('app.color.shadow_2'), offsetX: 0, offsetY: 4 })
    .borderRadius(20)
    .alignItems(HorizontalAlign.Center)
  }
}
