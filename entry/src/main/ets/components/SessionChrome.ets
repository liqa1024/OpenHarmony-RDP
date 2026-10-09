import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

// Fixed dark palette on purpose: the bar floats over the remote desktop image
// of unknown colours, so it never follows the app theme ($r('app.color.*')).
export const TOOLBAR_BG: string = '#CC1A1A1A';
export const TOOLBAR_FG: string = '#FFFFFF';
export const TOOLBAR_FG_SECONDARY: string = '#B8BCC4';
export const TOOLBAR_CHIP_BG: string = '#33FFFFFF';
export const TOOLBAR_ACCENT_CHIP_BG: string = '#4D3B82F6';
export const TOOLBAR_NEUTRAL_CHIP_BG: string = '#6E737B';
export const TOOLBAR_CHIP_BORDER: string = '#59FFFFFF';
export const TOOLBAR_PROGRESS: string = '#5B9CF8';
export const TOOLBAR_ROW_HEIGHT: number = 48;
export const TOOLBAR_TELEMETRY_HEIGHT: number = 24;
export const METRIC_GOOD: string = '#4ADE80';
export const METRIC_WARN: string = '#FBBF24';
export const METRIC_BAD: string = '#F87171';
export const TOOLBAR_ZONE_NONE: number = 0;
export const TOOLBAR_ZONE_TRIGGER: number = 1;
export const TOOLBAR_ZONE_BAR: number = 2;
export const REVEAL_HINT_WIDTH: number = 96;
export const REVEAL_HINT_HEIGHT: number = 3;
export const REVEAL_HINT_FADE_MS: number = 140;

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
  @Prop tip: string = '';
  private tipOptions: TipsOptions = { systemMaterial: SystemMaterial.of(MaterialSurface.OVERLAY) };

  build() {
    Row() {
      if (this.tip.length > 0 && this.metricWidth > 0) {
        Text(this.text)
          .fontSize(12)
          .fontColor(this.fgColor)
          .width(this.metricWidth)
          .maxLines(1)
          .bindTips(this.tip, this.tipOptions)
      } else if (this.tip.length > 0) {
        Text(this.text)
          .fontSize(12)
          .fontColor(this.fgColor)
          .maxLines(1)
          .bindTips(this.tip, this.tipOptions)
      } else if (this.metricWidth > 0) {
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
