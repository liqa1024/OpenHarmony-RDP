import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

// Fixed dark palette on purpose: the bar floats over the remote desktop image
// of unknown colours, so it never follows the app theme ($r('app.color.*')).
export const TOOLBAR_BG: string = '#B31B1B1D';
export const TOOLBAR_FG: string = '#FFFFFF';
export const TOOLBAR_FG_SECONDARY: string = '#B8BCC4';
export const TOOLBAR_CHIP_BG: string = '#33FFFFFF';
export const TOOLBAR_ACCENT_CHIP_BG: string = '#4D3B82F6';
export const TOOLBAR_NEUTRAL_CHIP_BG: string = '#996E737B';
export const TOOLBAR_DANGER_CHIP_BG: string = '#99F05A40';
export const TOOLBAR_CHIP_BORDER: string = '#59FFFFFF';
export const TOOLBAR_SHADOW: ShadowOptions = {
  radius: 18,
  color: '#4D000000',
  offsetX: 0,
  offsetY: 6
};
export const TOOLBAR_PROGRESS: string = '#5B9CF8';
export const TOOLBAR_ROW_HEIGHT: number = 48;
export const TOOLBAR_TELEMETRY_HEIGHT: number = 24;
export const TOOLBAR_RADIUS: number = 14;
export const METRIC_GOOD: string = '#4ADE80';
export const METRIC_WARN: string = '#FBBF24';
export const METRIC_BAD: string = '#F87171';
export const TOOLBAR_ZONE_NONE: number = 0;
export const TOOLBAR_ZONE_TRIGGER: number = 1;
export const TOOLBAR_ZONE_BAR: number = 2;
export const REVEAL_HINT_WIDTH: number = 96;
export const REVEAL_HINT_HEIGHT: number = 3;
export const REVEAL_HINT_FADE_MS: number = 140;
// The touch pull-down handle is the hover line's twin: same slot, same width, same expanding fill —
// only thicker, which is what tells a handle from a progress line.
export const TOUCH_BAR_HEIGHT: number = 4;

@Component
export struct ToolbarChip {
  @Prop label: string = '';
  @Prop chipBg: ResourceColor = TOOLBAR_CHIP_BG;
  @Prop fgColor: ResourceColor = TOOLBAR_FG;
  @Prop icon: Resource | undefined = undefined;
  @Prop hPad: number = 16;
  onTap: () => void = () => {};

  build() {
    Row() {
      if (this.icon !== undefined) {
        Image(this.icon)
          .width(16)
          .height(16)
          .fillColor(this.fgColor)
          .draggable(false)
      }
      Text(this.label)
        .fontSize(13)
        .fontColor(this.fgColor)
        .margin({ left: this.icon !== undefined ? 4 : 0 })
    }
    .padding({ left: this.hPad, right: this.hPad, top: 7, bottom: 7 })
    .backgroundColor(this.chipBg)
    .borderRadius(8)
    .hoverEffect(HoverEffect.Highlight)
    .onClick(() => {
      this.onTap();
    })
  }
}

// Icon-only chip for stateful toolbar actions (the pin): the same fill vocabulary as
// ToolbarChip, so an active state reads as a filled accent rather than a different control.
@Component
export struct ToolbarIconChip {
  @Prop icon: Resource | undefined = undefined;
  @Prop chipBg: ResourceColor = TOOLBAR_CHIP_BG;
  @Prop fgColor: ResourceColor = TOOLBAR_FG;
  @Prop iconSize: number = 16;
  @Prop iconAngle: number = 0;
  onTap: () => void = () => {};

  build() {
    Row() {
      if (this.icon !== undefined) {
        Image(this.icon)
          .width(this.iconSize)
          .height(this.iconSize)
          .fillColor(this.fgColor)
          .rotate({ angle: this.iconAngle })
          .draggable(false)
      }
    }
    .width(32)
    .height(32)
    .justifyContent(FlexAlign.Center)
    .backgroundColor(this.chipBg)
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
        .fontColor(TOOLBAR_FG)
      Text('  ' + this.detail)
        .fontSize(12)
        .fontColor(TOOLBAR_FG_SECONDARY)
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
      .fontColor(TOOLBAR_FG_SECONDARY)
      .padding({ left: 10, right: 10, top: 4, bottom: 4 })
      .border({ width: 1, color: TOOLBAR_CHIP_BORDER })
      .borderRadius(6)
      .hoverEffect(HoverEffect.Highlight)
      .onClick(() => {
        this.onTap();
      })
  }
}

// Fixed-width text so a changing digit count never reflows the row;
// `metricWidth` 0 means content width (the bandwidth field, whose text varies).
@Component
export struct MetricItem {
  @Prop text: string = '';
  @Prop metricWidth: number = 0;
  @Prop fgColor: ResourceColor = TOOLBAR_FG_SECONDARY;
  @Prop leftGap: number = 0;

  build() {
    Row() {
      if (this.metricWidth > 0) {
        Text(this.text)
          .fontSize(12)
          .fontColor(this.fgColor)
          .width(this.metricWidth)
          .maxLines(1)
      } else {
        Text(this.text)
          .fontSize(12)
          .fontColor(this.fgColor)
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
      .backgroundColor(TOOLBAR_FG)
      .opacity(this.fill)
      // A hint only: the pointer events the reveal band and the bar depend on
      // must pass straight through it.
      .hitTestBehavior(HitTestMode.None)
  }
}

// The touch pull-down handle. The text that teaches it is a system tip, not part of the handle:
// a label riding along with the bar read as one slab of chrome.
@Component
export struct ToolbarTouchBar {
  @Prop fill: number = 0;

  build() {
    Row()
      .width(REVEAL_HINT_WIDTH * this.fill)
      .height(TOUCH_BAR_HEIGHT)
      .borderRadius(TOUCH_BAR_HEIGHT / 2)
      .backgroundColor(TOOLBAR_FG)
      .opacity(this.fill)
      // A hint only: the second pull has to reach the picture through it.
      .hitTestBehavior(HitTestMode.None)
  }
}

// One telemetry field of the developer row; `width` 0 means content width (for
// variable-length text like the RDP bandwidth field).
export class TelemetryField {
  text: string;
  width: number;
  color: string;
  gap: number;

  constructor(text: string, width: number, color: string, gap: number) {
    this.text = text;
    this.width = width;
    this.color = color;
    this.gap = gap;
  }
}

// Latency text for a fixed-width field: a long millisecond count would overflow it,
// so past a second it degrades to seconds. Negative means "no reading yet".
export function formatLatency(label: string, value: number): string {
  if (value < 0) {
    return label + ' --';
  }
  return value >= 1000
    ? label + ' ' + (value / 1000).toFixed(1) + 's'
    : label + ' ' + Math.round(value) + 'ms';
}

// Latency colour by threshold; negative means "no reading yet".
export function metricColor(value: number, warnMs: number, badMs: number): string {
  if (value < 0) {
    return TOOLBAR_FG_SECONDARY;
  }
  if (value <= warnMs) {
    return METRIC_GOOD;
  }
  if (value <= badMs) {
    return METRIC_WARN;
  }
  return METRIC_BAD;
}

// The developer telemetry rows. The field set is small and positional, so every slot is laid out
// explicitly instead of looped: a ForEach key has to carry the field values (an unchanged key never
// updates its item), and a key that carries them changes on every sample — which rebuilds the node
// on every refresh.
@Component
export struct TelemetryRow {
  @Prop fields: TelemetryField[] = [];

  build() {
    Row() {
      if (this.fields.length > 0) {
        MetricItem({ text: this.fields[0].text, metricWidth: this.fields[0].width,
          fgColor: this.fields[0].color, leftGap: this.fields[0].gap })
      }
      if (this.fields.length > 1) {
        MetricItem({ text: this.fields[1].text, metricWidth: this.fields[1].width,
          fgColor: this.fields[1].color, leftGap: this.fields[1].gap })
      }
      if (this.fields.length > 2) {
        MetricItem({ text: this.fields[2].text, metricWidth: this.fields[2].width,
          fgColor: this.fields[2].color, leftGap: this.fields[2].gap })
      }
      if (this.fields.length > 3) {
        MetricItem({ text: this.fields[3].text, metricWidth: this.fields[3].width,
          fgColor: this.fields[3].color, leftGap: this.fields[3].gap })
      }
      if (this.fields.length > 4) {
        MetricItem({ text: this.fields[4].text, metricWidth: this.fields[4].width,
          fgColor: this.fields[4].color, leftGap: this.fields[4].gap })
      }
      if (this.fields.length > 5) {
        MetricItem({ text: this.fields[5].text, metricWidth: this.fields[5].width,
          fgColor: this.fields[5].color, leftGap: this.fields[5].gap })
      }
      if (this.fields.length > 6) {
        MetricItem({ text: this.fields[6].text, metricWidth: this.fields[6].width,
          fgColor: this.fields[6].color, leftGap: this.fields[6].gap })
      }
      if (this.fields.length > 7) {
        MetricItem({ text: this.fields[7].text, metricWidth: this.fields[7].width,
          fgColor: this.fields[7].color, leftGap: this.fields[7].gap })
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
          .color(TOOLBAR_FG)
        Text('在远端粘贴开始传输')
          .fontSize(12)
          .fontColor(TOOLBAR_FG)
          .margin({ left: 6 })
          .maxLines(1)
      } else {
        Text(this.text)
          .fontSize(12)
          .fontColor(TOOLBAR_FG)
          .maxLines(1)
        if (this.fileCount > 1 && this.fileIndex > 0) {
          Text(`${this.fileIndex}/${this.fileCount}`)
            .fontSize(12)
            .fontColor(TOOLBAR_FG)
            .margin({ left: 6 })
            .maxLines(1)
        }
        Text(`${this.percent}%`)
          .fontSize(12)
          .fontColor(TOOLBAR_FG)
          .margin({ left: 6 })
          .maxLines(1)
        if (this.speed.length > 0) {
          Text(this.speed)
            .fontSize(12)
            .fontColor(TOOLBAR_FG)
            .margin({ left: 6 })
            .maxLines(1)
        }
        Progress({ value: this.percent, total: 100, type: ProgressType.Linear })
          .width(140)
          .color(TOOLBAR_PROGRESS)
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

// The paste side is a system PasteButton: clicking it grants a one-time read
// without the READ_PASTEBOARD permission.
@Component
export struct ClipboardButtons {
  onCopy: () => void = () => {};
  pasteCallback: PasteButtonCallback = () => {};
  @Prop copyTip: string = '从远端复制到本机剪贴板';
  @Prop pasteTip: string = '把本机剪贴板粘贴到远端';
  private tipsOptions: TipsOptions = { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };

  build() {
    Row() {
      ToolbarChip({
        label: '复制',
        chipBg: TOOLBAR_ACCENT_CHIP_BG,
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
          .iconColor(TOOLBAR_FG)
          .fontSize(13)
          .fontColor(TOOLBAR_FG)
          .backgroundColor(TOOLBAR_ACCENT_CHIP_BG)
          .borderRadius(8)
          .padding({ left: 14, right: 14, top: 7, bottom: 7 })
          .onClick(this.pasteCallback)
      }
      .margin({ left: 8 })
      .bindTips(this.pasteTip, this.tipsOptions)
    }
  }
}
