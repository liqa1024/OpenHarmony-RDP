import { PROTOCOL_MOONLIGHT, PROTOCOL_RDP } from '../model/Connections';

export const TAB_COMMON: number = 2;

@Component
export struct ProtocolTabs {
  @Prop protocol: number = PROTOCOL_RDP;
  @Prop locked: boolean = false;
  @Prop showCommon: boolean = false;
  onChange: (protocol: number) => void = (protocol: number) => {};

  build() {
    Row() {
      if (this.showCommon) {
        this.tab(TAB_COMMON, '通用')
      }
      this.tab(PROTOCOL_RDP, 'RDP')
      this.tab(PROTOCOL_MOONLIGHT, 'Moonlight')
    }
  }

  private accentColor(protocol: number): Resource {
    if (protocol === PROTOCOL_MOONLIGHT) {
      return $r('app.color.protocol_moonlight');
    }
    if (protocol === PROTOCOL_RDP) {
      return $r('app.color.protocol_rdp');
    }
    return $r('app.color.brand_text');
  }

  @Builder
  tab(protocol: number, label: string) {
    Column() {
      Text(label)
        .fontSize(14)
        .fontWeight(this.protocol === protocol ? FontWeight.Medium : FontWeight.Regular)
        .fontColor(this.protocol === protocol
          ? $r('app.color.text_primary') : $r('app.color.text_secondary'))
      Divider()
        .strokeWidth(2)
        .width(36)
        .color(this.protocol === protocol
          ? this.accentColor(protocol) : $r('app.color.transparent'))
        .margin({ top: 5 })
    }
    .alignItems(HorizontalAlign.Center)
    .padding({ left: 12, right: 12, top: 4 })
    .responseRegion({ x: 0, y: -4, width: '100%', height: 40 })
    .opacity(this.locked && this.protocol !== protocol ? 0.4 : 1.0)
    .onClick(() => {
      if (this.locked || this.protocol === protocol) {
        return;
      }
      this.onChange(protocol);
    })
    .focusable(!this.locked)
    .focusOnTouch(!this.locked)
  }
}


