@Component
export struct ContentCard {
  @Prop pad: number = 24;
  @BuilderParam content: () => void;

  build() {
    Column() {
      this.content()
    }
    .width('100%')
    .constraintSize({ maxWidth: 720 })
    .padding(this.pad)
    .borderRadius(20)
    .backgroundColor($r('app.color.surface_bg'))
    .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
    .alignItems(HorizontalAlign.Start)
  }
}
