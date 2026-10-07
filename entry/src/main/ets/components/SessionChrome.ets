/*
 * HmRdp - session toolbar chrome.
 *
 * The session toolbar floats over the remote desktop image, whose colours are
 * unknown, so its palette is deliberately *fixed dark* instead of following the
 * app theme (see doc_agent/ui-design.md §1.2). This module owns that palette and
 * the small chip / metric pieces both session pages share; the bar layout itself
 * stays in the page, since the RDP page also carries transfer progress and
 * telemetry.
 */
import { MaterialSurface, SystemMaterial } from '../services/SystemMaterial';

// Fixed dark toolbar palette (never $r('app.color.*') — see the header note).
export const TOOLBAR_BG: string = '#CC1A1A1A';
export const TOOLBAR_FG: string = '#FFFFFF';
export const TOOLBAR_FG_SECONDARY: string = '#B8BCC4';
export const TOOLBAR_CHIP_BG: string = '#33FFFFFF';
export const TOOLBAR_ACCENT_CHIP_BG: string = '#4D3B82F6';
export const TOOLBAR_CHIP_BORDER: string = '#59FFFFFF';
export const TOOLBAR_PROGRESS: string = '#5B9CF8';
export const TOOLBAR_ROW_HEIGHT: number = 48;
export const TOOLBAR_TELEMETRY_HEIGHT: number = 24;
// Telemetry threshold colours, tuned against the fixed dark bar.
export const METRIC_GOOD: string = '#4ADE80';
export const METRIC_WARN: string = '#FBBF24';
export const METRIC_BAD: string = '#F87171';

/** A filled toolbar chip: optional leading icon + label. */
@Component
export struct ToolbarChip {
  @Prop label: string = '';
  @Prop chipBg: ResourceColor = TOOLBAR_CHIP_BG;
  @Prop fgColor: ResourceColor = TOOLBAR_FG;
  @Prop icon: Resource | undefined = undefined;
  // Horizontal padding: the tinted clipboard chips are a touch tighter than the
  // window-action chips.
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

/** An outlined (one step below filled) toolbar action, e.g. "取消". */
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

/**
 * One telemetry field: fixed-width text so a changing digit count never reflows
 * the row, with an optional tooltip breakdown for the fields that have one.
 * `metricWidth` 0 means content width (the bandwidth field, whose text varies).
 */
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
