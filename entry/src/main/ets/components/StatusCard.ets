/*
 * HmRdp - session connection status card.
 *
 * The modal card shown over the remote image while a session is connecting,
 * disconnecting, failed or idle. Shared by the RDP and Moonlight session pages
 * so both offer the same card, buttons and material; each page passes its own
 * labels and handlers, and an optional leading icon.
 */
import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

@Component
export struct StatusCard {
  @Prop title: string = '';
  @Prop subtitle: string = '';
  // Button labels: an empty one renders no button (e.g. "connecting" offers only
  // the primary cancel).
  @Prop primary: string = '';
  @Prop secondary: string = '';
  // A spinner replaces the icon while a connection attempt is in flight.
  @Prop loading: boolean = false;
  @Prop icon: Resource | undefined = undefined;
  onPrimary: () => void = () => {};
  onSecondary: () => void = () => {};
  private cardMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.CARD));

  build() {
    Column() {
      if (this.loading) {
        LoadingProgress().width(44).height(44).color($r('app.color.accent'))
      } else if (this.icon !== undefined) {
        Image(this.icon)
          .width(52)
          .height(52)
          .borderRadius(14)
          .draggable(false)
      }
      Text(this.title)
        .fontSize(17)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .margin({ top: 18 })
      Text(this.subtitle)
        .fontSize(13)
        .fontColor($r('app.color.text_secondary'))
        .margin({ top: 8 })
        .textAlign(TextAlign.Center)
        .constraintSize({ maxWidth: 320 })
      Row() {
        if (this.primary.length > 0) {
          Button(this.primary)
            .height(42)
            .fontSize(14)
            .backgroundColor($r('app.color.accent'))
            .borderRadius(10)
            .onClick(() => {
              this.onPrimary();
            })
        }
        if (this.secondary.length > 0) {
          Button(this.secondary)
            .height(42)
            .fontSize(14)
            .fontColor($r('app.color.text_primary'))
            .backgroundColor($r('app.color.input_bg'))
            .borderRadius(10)
            .margin({ left: 12 })
            .onClick(() => {
              this.onSecondary();
            })
        }
      }
      .margin({ top: 24 })
    }
    .width(360)
    .padding({ top: 32, bottom: 28, left: 28, right: 28 })
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : $r('app.color.card_bg'))
    .borderRadius(20)
    .shadow({ radius: 30, color: '#33000000', offsetX: 0, offsetY: 10 })
    .attributeModifier(this.cardMaterial)
    .alignItems(HorizontalAlign.Center)
  }
}
