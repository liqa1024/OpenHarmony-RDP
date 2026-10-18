@Component
export struct ContentCard {
  @Prop pad: number = 24;
  @BuilderParam content: () => void;

  build() {
    Row() {
      Column() {
        this.content()
      }
      .width('100%')
      .constraintSize({ maxWidth: 720 })
      .padding(this.pad)
      .borderRadius(20)
      .backgroundColor($r('app.color.surface'))
      .border({ width: 1, color: $r('app.color.border') })
      .shadow({ radius: 8, color: $r('app.color.shadow_1'), offsetX: 0, offsetY: 2 })
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
    .justifyContent(FlexAlign.Center)
  }
}
