import { ImmersiveModifier, MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

@Component
export struct MaterialCard {
  @Prop pad: number = 24;
  @BuilderParam content: () => void;
  private cardMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(SystemMaterial.of(MaterialSurface.CARD));

  build() {
    Column() {
      this.content()
    }
    .width('100%')
    .constraintSize({ maxWidth: 720 })
    .padding(this.pad)
    .backgroundColor(SystemMaterial.supported ? Color.Transparent : $r('app.color.card_bg'))
    .borderRadius(20)
    .alignItems(HorizontalAlign.Start)
    .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
    .attributeModifier(this.cardMaterial)
  }
}
