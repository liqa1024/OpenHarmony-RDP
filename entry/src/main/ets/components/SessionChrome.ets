import { REVEAL_HINT_HEIGHT, REVEAL_HINT_WIDTH, TOOLBAR_TELEMETRY_HEIGHT,
  TOUCH_BAR_HEIGHT } from '../services/ToolbarContract';

@Component
export struct ToolbarChip {
  @Prop label: string = '';
  @Prop icon: Resource | undefined = undefined;
  @Prop hPad: number = 14;
  @Prop danger: boolean = false;
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;

  build() {
    Row() {
      if (this.icon !== undefined) {
        Image(this.icon)
          .width(16)
          .height(16)
          .fillColor(this.danger
            ? $r('app.color.chrome_danger') : $r('app.color.chrome_text_primary'))
          .draggable(false)
      }
      Text(this.label)
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor(this.danger
          ? $r('app.color.chrome_danger') : $r('app.color.chrome_text_primary'))
        .margin({ left: this.icon !== undefined ? 4 : 0 })
    }
    .height(32)
    .padding({ left: this.hPad, right: this.hPad })
    .backgroundColor(this.danger
      ? (this.hovered || this.pressed
        ? $r('app.color.chrome_danger_hover') : $r('app.color.chrome_danger_bg'))
      : (this.pressed
        ? $r('app.color.chrome_control_pressed')
        : (this.hovered ? $r('app.color.chrome_control_hover') : $r('app.color.chrome_control_bg'))))
    .border({
      width: 1,
      color: this.danger
        ? $r('app.color.chrome_danger_border') : $r('app.color.chrome_control_border')
    })
    .borderRadius(8)
    .responseRegion({ x: 0, y: -4, width: '100%', height: 40 })
    .hoverEffect(HoverEffect.None)
    .onHover((isHover: boolean) => {
      this.hovered = isHover;
      if (!isHover) {
        this.pressed = false;
      }
    })
    .onMouse((event: MouseEvent) => {
      if (event.action === MouseAction.Press) {
        this.pressed = true;
      } else if (event.action === MouseAction.Release) {
        this.pressed = false;
      }
    })
    .onTouch((event: TouchEvent) => {
      if (event.type === TouchType.Down) {
        this.pressed = true;
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        this.pressed = false;
      }
    })
    .onClick(() => {
      this.onTap();
    })
    .focusable(true)
    .focusOnTouch(true)
  }
}

@Component
export struct ToolbarIconChip {
  @Prop icon: Resource | undefined = undefined;
  @Prop iconSize: number = 20;
  @Prop iconAngle: number = 0;
  @Prop active: boolean = false;
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;

  build() {
    Row() {
      if (this.icon !== undefined) {
        Image(this.icon)
          .width(this.iconSize)
          .height(this.iconSize)
          .fillColor(this.active
            ? $r('app.color.chrome_accent') : $r('app.color.chrome_text_primary'))
          .rotate({ angle: this.iconAngle })
          .draggable(false)
      }
    }
    .width(32)
    .height(32)
    .justifyContent(FlexAlign.Center)
    .backgroundColor(this.active
      ? $r('app.color.chrome_accent_bg')
      : (this.pressed
        ? $r('app.color.chrome_control_pressed')
        : (this.hovered ? $r('app.color.chrome_control_hover') : $r('app.color.chrome_control_bg'))))
    .border({
      width: 1,
      color: this.active
        ? $r('app.color.chrome_accent_border') : $r('app.color.chrome_control_border')
    })
    .borderRadius(8)
    .responseRegion({ x: -4, y: -4, width: 40, height: 40 })
    .hoverEffect(HoverEffect.None)
    .onHover((isHover: boolean) => {
      this.hovered = isHover;
      if (!isHover) {
        this.pressed = false;
      }
    })
    .onMouse((event: MouseEvent) => {
      if (event.action === MouseAction.Press) {
        this.pressed = true;
      } else if (event.action === MouseAction.Release) {
        this.pressed = false;
      }
    })
    .onTouch((event: TouchEvent) => {
      if (event.type === TouchType.Down) {
        this.pressed = true;
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        this.pressed = false;
      }
    })
    .onClick(() => {
      this.onTap();
    })
    .focusable(true)
    .focusOnTouch(true)
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
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.chrome_text_primary'))
      Text('  ' + this.detail)
        .fontSize(12)
        .fontColor($r('app.color.chrome_text_secondary'))
    }
    .alignItems(VerticalAlign.Center)
  }
}

@Component
export struct OutlineChip {
  @Prop label: string = '';
  onTap: () => void = () => {};
  @State hovered: boolean = false;
  @State pressed: boolean = false;

  build() {
    Text(this.label)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor($r('app.color.chrome_text_primary'))
      .height(28)
      .padding({ left: 10, right: 10 })
      .textAlign(TextAlign.Center)
      .backgroundColor(this.pressed
        ? $r('app.color.chrome_control_pressed')
        : (this.hovered ? $r('app.color.chrome_control_hover') : $r('app.color.chrome_control_bg')))
      .border({ width: 1, color: $r('app.color.chrome_control_border') })
      .borderRadius(8)
      .responseRegion({ x: 0, y: -6, width: '100%', height: 40 })
      .hoverEffect(HoverEffect.None)
      .onHover((isHover: boolean) => {
        this.hovered = isHover;
        if (!isHover) {
          this.pressed = false;
        }
      })
      .onMouse((event: MouseEvent) => {
        if (event.action === MouseAction.Press) {
          this.pressed = true;
        } else if (event.action === MouseAction.Release) {
          this.pressed = false;
        }
      })
      .onTouch((event: TouchEvent) => {
        if (event.type === TouchType.Down) {
          this.pressed = true;
        } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
          this.pressed = false;
        }
      })
      .onClick(() => {
        this.onTap();
      })
      .focusable(true)
      .focusOnTouch(true)
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
          .fontColor($r('app.color.chrome_text_secondary'))
          .fontFeature('"tnum" 1')
          .width(this.metricWidth)
          .maxLines(1)
      } else {
        Text(this.text)
          .fontSize(12)
          .fontColor($r('app.color.chrome_text_secondary'))
          .fontFeature('"tnum" 1')
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
      .backgroundColor($r('app.color.chrome_accent'))
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
      .backgroundColor($r('app.color.chrome_accent'))
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
          .color($r('app.color.chrome_accent'))
        Text('在远端粘贴开始传输')
          .fontSize(12)
          .fontColor($r('app.color.chrome_text_secondary'))
          .margin({ left: 6 })
          .maxLines(1)
      } else {
        Text(this.text)
          .fontSize(12)
          .fontColor($r('app.color.chrome_text_secondary'))
          .maxLines(1)
        if (this.fileCount > 1 && this.fileIndex > 0) {
          Text(`${this.fileIndex}/${this.fileCount}`)
            .fontSize(12)
            .fontColor($r('app.color.chrome_text_secondary'))
            .fontFeature('"tnum" 1')
            .margin({ left: 6 })
            .maxLines(1)
        }
        Text(`${this.percent}%`)
          .fontSize(12)
          .fontColor($r('app.color.chrome_text_secondary'))
          .fontFeature('"tnum" 1')
          .margin({ left: 6 })
          .maxLines(1)
        if (this.speed.length > 0) {
          Text(this.speed)
            .fontSize(12)
            .fontColor($r('app.color.chrome_text_secondary'))
            .fontFeature('"tnum" 1')
            .margin({ left: 6 })
            .maxLines(1)
        }
        Progress({ value: this.percent, total: 100, type: ProgressType.Linear })
          .width(140)
          .height(3)
          .color($r('app.color.chrome_accent'))
          .backgroundColor($r('app.color.chrome_progress_track'))
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
        // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
        .onTouch(() => {})
        .bindTips(this.copyTip, this.tipsOptions)
      Row() {
        PasteButton({
          icon: PasteIconStyle.LINES,
          text: PasteDescription.PASTE,
          buttonType: ButtonType.Normal
        })
          .iconSize(16)
          .fontSize(12)
          .fontColor($r('app.color.chrome_text_primary'))
          .backgroundColor($r('app.color.chrome_control_bg'))
          .borderWidth(1)
          .borderColor($r('app.color.chrome_control_border'))
          .borderStyle(BorderStyle.Solid)
          .borderRadius(8)
          .padding({ left: 14, right: 14, top: 7, bottom: 7 })
          .onClick(this.pasteCallback)
      }
      .margin({ left: 8 })
      // bindTips 宿主需自身注册触摸（见 doc_agent/ui-feedback.md §4.7）
      .onTouch(() => {})
      .bindTips(this.pasteTip, this.tipsOptions)
    }
  }
}
