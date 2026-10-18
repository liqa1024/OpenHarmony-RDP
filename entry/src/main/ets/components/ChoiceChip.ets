@Component
export struct ChoiceChip {
  @Prop label: string = '';
  @Prop selected: boolean = false;
  @Prop chipEnabled: boolean = true;
  onTap: () => void = () => {};

  build() {
    Text(this.label)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor(this.selected ? $r('app.color.brand_text') : $r('app.color.text_secondary'))
      .height(32)
      .textAlign(TextAlign.Center)
      .opacity(this.chipEnabled ? 1.0 : 0.4)
      .borderRadius(8)
      .backgroundColor(this.selected ? $r('app.color.brand_tint') : $r('app.color.control_bg'))
      .border({
        width: 1,
        color: this.selected ? $r('app.color.brand_tint_strong') : $r('app.color.control_border')
      })
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .responseRegion({ x: 0, y: -4, width: '100%', height: 40 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        if (this.chipEnabled) {
          this.onTap();
        }
      })
      .focusable(this.chipEnabled)
      .focusOnTouch(this.chipEnabled)
  }
}


