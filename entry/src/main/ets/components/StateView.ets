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
      if (this.kind === StateViewKind.Loading) {
        LoadingProgress()
          .width(40)
          .height(40)
      } else if (this.icon !== undefined) {
        Image(this.icon)
          .width(52)
          .height(52)
          .borderRadius(14)
          .draggable(false)
      }

      if (this.title.length > 0) {
        Text(this.title)
          .fontSize(17)
          .fontWeight(FontWeight.Medium)
          .margin({ top: this.kind === StateViewKind.Loading ? 16 : 18 })
      }

      if (this.message.length > 0) {
        Text(this.message)
          .fontSize(13)
          .textAlign(TextAlign.Center)
          .constraintSize({ maxWidth: 320 })
          .margin({ top: 8 })
      }

      if (this.primaryLabel.length > 0 || this.secondaryLabel.length > 0) {
        Row() {
          if (this.primaryLabel.length > 0) {
            Button(this.primaryLabel)
              .height(42)
              .fontSize(14)
              .borderRadius(10)
              .onClick(() => {
                this.onPrimary();
              })
          }
          if (this.secondaryLabel.length > 0) {
            Button(this.secondaryLabel)
              .height(42)
              .fontSize(14)
              .borderRadius(10)
              .margin({ left: this.primaryLabel.length > 0 ? 12 : 0 })
              .onClick(() => {
                this.onSecondary();
              })
          }
        }
        .margin({ top: 24 })
      }
    }
    .width('100%')
    .height('100%')
    .padding({ left: 24, right: 24 })
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
  }
}
