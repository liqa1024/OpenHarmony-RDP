import { FrostedModifier, FrostedSurface } from '../services/SystemMaterial';

@Component
export struct FrostedCard {
  @Prop pad: number = 24;
  @BuilderParam content: () => void;
  private cardFrost: AttributeModifier<ColumnAttribute> =
    new FrostedModifier<ColumnAttribute>(FrostedSurface.CARD);

  build() {
    Column() {
      this.content()
    }
    .width('100%')
    .constraintSize({ maxWidth: 720 })
    .padding(this.pad)
    .borderRadius(20)
    .alignItems(HorizontalAlign.Start)
    .shadow({ radius: 24, color: '#14000000', offsetX: 0, offsetY: 8 })
    .attributeModifier(this.cardFrost)
  }
}
