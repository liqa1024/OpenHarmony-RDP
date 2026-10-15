import { ImmersiveModifier, MaterialSurface } from '../services/SystemMaterial';

@Component
export struct MaterialCard {
  @Prop pad: number = 24;
  @BuilderParam content: () => void;
  private cardMaterial: AttributeModifier<ColumnAttribute> =
    new ImmersiveModifier<ColumnAttribute>(MaterialSurface.CARD);

  build() {
    Column() {
      this.content()
    }
    .width('100%')
    .constraintSize({ maxWidth: 720 })
    .padding(this.pad)
    .backgroundColor(Color.Transparent)
    .borderRadius(20)
    .alignItems(HorizontalAlign.Start)
    .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
    .attributeModifier(this.cardMaterial)
  }
}
