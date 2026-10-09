import { TextAction } from './TextAction';

@Component
export struct HeaderBar {
  @Prop title: string = '';
  @Prop leftLabel: string = '';
  onLeft: () => void = () => {};
  @BuilderParam trailing: () => void;

  build() {
    Column() {
      Row() {
        TextAction({
          label: this.leftLabel,
          onTap: () => {
            this.onLeft();
          }
        })
        Text(this.title)
          .fontSize(17)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .margin({ left: 16 })
        Blank()
        this.trailing()
      }
      .width('100%')
      .padding({ left: 24, right: 24, top: 10, bottom: 8 })

      Divider().color($r('app.color.divider'))
    }
    .width('100%')
  }
}
