import { TextAction } from './TextAction';

export enum PageHeaderVariant {
  Standard = 0,
  Brand = 1
}

@Component
export struct PageHeader {
  @Prop variant: number = PageHeaderVariant.Standard;
  @Prop title: ResourceStr = '';
  @Prop subtitle: string = '';
  @Prop leftLabel: string = '';
  @Prop icon: Resource = $r('app.media.app_icon');
  onLeft: () => void = () => {};
  @BuilderParam trailing: () => void;

  @Builder
  brandLeading() {
    Row() {
      Image(this.icon)
        .width(44)
        .height(44)
        .borderRadius(12)
        .clip(true)
        .draggable(false)
      Column() {
        Text(this.title)
          .fontSize(20)
          .fontWeight(600)
          .fontColor($r('app.color.text_primary'))
        if (this.subtitle.length > 0) {
          Text(this.subtitle)
            .fontSize(12)
            .fontColor($r('app.color.text_tertiary'))
            .margin({ top: 2 })
        }
      }
      .alignItems(HorizontalAlign.Start)
      .margin({ left: 12 })
    }
  }

  @Builder
  standardLeading() {
    Row() {
      if (this.leftLabel.length > 0) {
        TextAction({
          label: this.leftLabel,
          onTap: () => {
            this.onLeft();
          }
        })
      }
      Text(this.title)
        .fontSize(18)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .margin({ left: this.leftLabel.length > 0 ? 16 : 0 })
    }
  }

  build() {
    Column() {
      Row() {
        if (this.variant === PageHeaderVariant.Brand) {
          this.brandLeading()
        } else {
          this.standardLeading()
        }
        Blank()
        this.trailing()
      }
      .width('100%')
      .padding(this.variant === PageHeaderVariant.Brand
        ? { left: 24, right: 24, top: 20, bottom: 16 }
        : { left: 24, right: 24, top: 10, bottom: 8 })

      Divider()
        .strokeWidth(1)
        .color($r('app.color.divider'))
    }
    .width('100%')
    .backgroundColor($r('app.color.header_glass'))
    .backgroundBlurStyle(BlurStyle.COMPONENT_THICK)
  }
}
