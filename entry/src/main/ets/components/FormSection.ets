import { TextAction } from './TextAction';

@Component
export struct FormSection {
  @Prop title: string = '';
  @Prop description: string = '';
  @Prop actionLabel: string = '';
  @Prop showDivider: boolean = true;
  @Prop padTop: number = 14;
  @Prop padBottom: number = 14;
  onAction: () => void = () => {};
  @BuilderParam content: () => void;

  build() {
    Column() {
      if (this.title.length > 0 || this.description.length > 0 || this.actionLabel.length > 0) {
        Row() {
          Column() {
            if (this.title.length > 0) {
              Text(this.title)
                .fontSize(14)
                .fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_primary'))
            }
            if (this.description.length > 0) {
              Text(this.description)
                .fontSize(12)
                .fontColor($r('app.color.text_tertiary'))
                .margin({ top: 2 })
            }
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)

          if (this.actionLabel.length > 0) {
            TextAction({
              label: this.actionLabel,
              muted: true,
              onTap: () => {
                this.onAction();
              }
            })
          }
        }
        .width('100%')
      }

      this.content()

      if (this.showDivider) {
        Divider()
          .strokeWidth(1)
          .color($r('app.color.divider'))
          .margin({ top: 14 })
      }
    }
    .width('100%')
    .padding({ top: this.padTop, bottom: this.padBottom })
    .alignItems(HorizontalAlign.Start)
  }
}
