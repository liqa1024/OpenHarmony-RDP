/*
 * HmRdp - selected/unselected choice chip.
 *
 * The one chip shape used by every preset group (scale / super-resolution /
 * Moonlight stream options / pinch mode): accent + white text when selected,
 * muted on the input background otherwise. The caller owns the selection state
 * and the options; this only renders one choice.
 */
@Component
export struct ChoiceChip {
  @Prop label: string = '';
  @Prop selected: boolean = false;
  @Prop chipEnabled: boolean = true;
  onTap: () => void = () => {};

  build() {
    Text(this.label)
      .fontSize(13)
      .fontColor(this.selected ? '#FFFFFF' : $r('app.color.text_secondary'))
      .backgroundColor(this.selected
        ? $r('app.color.accent') : $r('app.color.input_bg'))
      .opacity(this.chipEnabled ? 1.0 : 0.4)
      .borderRadius(8)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .margin({ right: 8, bottom: 8 })
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        if (this.chipEnabled) {
          this.onTap();
        }
      })
  }
}
