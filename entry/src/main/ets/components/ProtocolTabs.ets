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

  @Builder
  tab(protocol: number, label: string) {
    Column() {
      Text(label)
        .fontSize(14)
        .fontWeight(this.protocol === protocol ? FontWeight.Medium : FontWeight.Normal)
        
      Divider()
        .strokeWidth(2)
        .width(36)
        
        .margin({ top: 5 })
    }
    .alignItems(HorizontalAlign.Center)
    .padding({ left: 12, right: 12, top: 4 })
    .opacity(this.locked && this.protocol !== protocol ? 0.4 : 1.0)
    .onClick(() => {
      if (this.locked || this.protocol === protocol) {
        return;
      }
      this.onChange(protocol);
    })
  }
}


