export const TOOLBAR_ROW_HEIGHT: number = 48;
export const TOOLBAR_TELEMETRY_HEIGHT: number = 24;
export const TOOLBAR_RADIUS: number = 14;
export const TOOLBAR_ZONE_NONE: number = 0;
export const TOOLBAR_ZONE_TRIGGER: number = 1;
export const TOOLBAR_ZONE_BAR: number = 2;
export const REVEAL_HINT_WIDTH: number = 96;
export const REVEAL_HINT_HEIGHT: number = 3;
export const REVEAL_HINT_FADE_MS: number = 140;
export const TOUCH_BAR_HEIGHT: number = 4;

@Component
export struct ToolbarChip {
  @Prop label: string = '';
  @Prop icon: Resource | undefined = undefined;
  @Prop hPad: number = 16;
  onTap: () => void = () => {};

  build() {
    Row() {
      if (this.icon !== undefined) {
        Image(this.icon)
          .width(16)
          .height(16)
          
          .draggable(false)
      }
      Text(this.label)
        .fontSize(13)
        
        .margin({ left: this.icon !== undefined ? 4 : 0 })
    }
    .padding({ left: this.hPad, right: this.hPad, top: 7, bottom: 7 })
    
    .borderRadius(8)
    .hoverEffect(HoverEffect.Highlight)
    .onClick(() => {
      this.onTap();
    })
  }
}

@Component
export struct ToolbarIconChip {
  @Prop icon: Resource | undefined = undefined;
  @Prop iconSize: number = 16;
  @Prop iconAngle: number = 0;
  onTap: () => void = () => {};

  build() {
    Row() {
      if (this.icon !== undefined) {
        Image(this.icon)
          .width(this.iconSize)
          .height(this.iconSize)
          
          .rotate({ angle: this.iconAngle })
          .draggable(false)
      }
    }
    .width(32)
    .height(32)
    .justifyContent(FlexAlign.Center)
    
    .borderRadius(8)
    .hoverEffect(HoverEffect.Highlight)
    .onClick(() => {
      this.onTap();
    })
  }
}

@Component
export struct SessionTitle {
  @Prop name: string = '';
  @Prop detail: string = '';

  build() {
    Row() {
      Text(this.name)
        .fontSize(14)
        
      Text('  ' + this.detail)
        .fontSize(12)
        
    }
  }
}

@Component
export struct OutlineChip {
  @Prop label: string = '';
  onTap: () => void = () => {};

  build() {
    Text(this.label)
      .fontSize(12)
      
      .padding({ left: 10, right: 10, top: 4, bottom: 4 })
      
      .borderRadius(6)
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.onTap();
      })
  }
}

@Component
export struct MetricItem {
  @Prop text: string = '';
  @Prop metricWidth: number = 0;
  @Prop leftGap: number = 0;

  build() {
    Row() {
      if (this.metricWidth > 0) {
        Text(this.text)
          .fontSize(12)
          
          .width(this.metricWidth)
          .maxLines(1)
      } else {
        Text(this.text)
          .fontSize(12)
          
          .maxLines(1)
      }
    }
    .margin({ left: this.leftGap })
  }
}

@Component
export struct ToolbarRevealHint {
  @Prop fill: number = 0;

  build() {
    Row()
      .width(REVEAL_HINT_WIDTH * this.fill)
      .height(REVEAL_HINT_HEIGHT)
      .borderRadius(REVEAL_HINT_HEIGHT / 2)
      
      .opacity(this.fill)
      .hitTestBehavior(HitTestMode.None)
  }
}

@Component
export struct ToolbarTouchBar {
  @Prop fill: number = 0;

  build() {
    Row()
      .width(REVEAL_HINT_WIDTH * this.fill)
      .height(TOUCH_BAR_HEIGHT)
      .borderRadius(TOUCH_BAR_HEIGHT / 2)
      
      .opacity(this.fill)
      .hitTestBehavior(HitTestMode.None)
  }
}

export class TelemetryField {
  text: string;
  width: number;
  gap: number;

  constructor(text: string, width: number, gap: number) {
    this.text = text;
    this.width = width;
    this.gap = gap;
  }
}

export function formatLatency(label: string, value: number): string {
  if (value < 0) {
    return label + ' --';
  }
  return value >= 1000
    ? label + ' ' + (value / 1000).toFixed(1) + 's'
    : label + ' ' + Math.round(value) + 'ms';
}

@Component
export struct TelemetryRow {
  @Prop fields: TelemetryField[] = [];

  build() {
    Row() {
      if (this.fields.length > 0) {
        MetricItem({ text: this.fields[0].text, metricWidth: this.fields[0].width,
          leftGap: this.fields[0].gap })
      }
      if (this.fields.length > 1) {
        MetricItem({ text: this.fields[1].text, metricWidth: this.fields[1].width,
          leftGap: this.fields[1].gap })
      }
      if (this.fields.length > 2) {
        MetricItem({ text: this.fields[2].text, metricWidth: this.fields[2].width,
          leftGap: this.fields[2].gap })
      }
      if (this.fields.length > 3) {
        MetricItem({ text: this.fields[3].text, metricWidth: this.fields[3].width,
          leftGap: this.fields[3].gap })
      }
      if (this.fields.length > 4) {
        MetricItem({ text: this.fields[4].text, metricWidth: this.fields[4].width,
          leftGap: this.fields[4].gap })
      }
      if (this.fields.length > 5) {
        MetricItem({ text: this.fields[5].text, metricWidth: this.fields[5].width,
          leftGap: this.fields[5].gap })
      }
      if (this.fields.length > 6) {
        MetricItem({ text: this.fields[6].text, metricWidth: this.fields[6].width,
          leftGap: this.fields[6].gap })
      }
      if (this.fields.length > 7) {
        MetricItem({ text: this.fields[7].text, metricWidth: this.fields[7].width,
          leftGap: this.fields[7].gap })
      }
    }
    .width('100%')
    .height(TOOLBAR_TELEMETRY_HEIGHT)
    .padding({ left: 16, right: 16 })
  }
}

@Component
export struct ClipboardTransferStatus {
  @Prop waiting: boolean = false;
  @Prop percent: number = 0;
  @Prop text: string = '';
  @Prop speed: string = '';
  @Prop fileIndex: number = 0;
  @Prop fileCount: number = 0;
  onCancel: () => void = () => {};

  build() {
    Row() {
      if (this.waiting) {
        LoadingProgress()
          .width(14)
          .height(14)
          
        Text('在远端粘贴开始传输')
          .fontSize(12)
          
          .margin({ left: 6 })
          .maxLines(1)
      } else {
        Text(this.text)
          .fontSize(12)
          
          .maxLines(1)
        if (this.fileCount > 1 && this.fileIndex > 0) {
          Text(`${this.fileIndex}/${this.fileCount}`)
            .fontSize(12)
            
            .margin({ left: 6 })
            .maxLines(1)
        }
        Text(`${this.percent}%`)
          .fontSize(12)
          
          .margin({ left: 6 })
          .maxLines(1)
        if (this.speed.length > 0) {
          Text(this.speed)
            .fontSize(12)
            
            .margin({ left: 6 })
            .maxLines(1)
        }
        Progress({ value: this.percent, total: 100, type: ProgressType.Linear })
          .width(140)
          
          .margin({ left: 8 })
      }
      OutlineChip({
        label: '取消',
        onTap: () => {
          this.onCancel();
        }
      })
        .margin({ left: 8 })
    }
    .margin({ right: 12 })
  }
}

@Component
export struct ClipboardButtons {
  onCopy: () => void = () => {};
  pasteCallback: PasteButtonCallback = () => {};
  @Prop copyTip: string = '从远端复制到本机剪贴板';
  @Prop pasteTip: string = '把本机剪贴板粘贴到远端';
  private tipsOptions: TipsOptions = {};

  build() {
    Row() {
      ToolbarChip({
        label: '复制',
        icon: $r('sys.media.ohos_ic_public_copy'),
        hPad: 14,
        onTap: () => {
          this.onCopy();
        }
      })
        .bindTips(this.copyTip, this.tipsOptions)
      Row() {
        PasteButton({
          icon: PasteIconStyle.LINES,
          text: PasteDescription.PASTE,
          buttonType: ButtonType.Normal
        })
          .iconSize(16)
          
          .fontSize(13)
          
          
          .borderRadius(8)
          .padding({ left: 14, right: 14, top: 7, bottom: 7 })
          .onClick(this.pasteCallback)
      }
      .margin({ left: 8 })
      .bindTips(this.pasteTip, this.tipsOptions)
    }
  }
}


