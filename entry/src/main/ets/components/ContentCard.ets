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
    
    
    .alignItems(HorizontalAlign.Start)
  }
}


