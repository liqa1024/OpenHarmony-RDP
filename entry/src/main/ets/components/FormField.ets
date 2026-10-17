@Component
export struct FormField {
  @Prop label: string = '';
  @Prop hint: string = '';
  @Prop error: string = '';
  @Prop required: boolean = false;
  @Prop marginBottom: number = 0;
  @BuilderParam content: () => void;

  build() {
    Column() {
      if (this.label.length > 0) {
        Text(this.required ? this.label + ' *' : this.label)
          .fontSize(12)
          .margin({ bottom: 6 })
      }

      this.content()

      if (this.error.length > 0) {
        Text(this.error)
          .fontSize(11)
          .margin({ top: 4 })
      } else if (this.hint.length > 0) {
        Text(this.hint)
          .fontSize(11)
          .margin({ top: 4 })
      }
    }
    .width('100%')
    .padding({ bottom: this.marginBottom })
    .alignItems(HorizontalAlign.Start)
  }
}
