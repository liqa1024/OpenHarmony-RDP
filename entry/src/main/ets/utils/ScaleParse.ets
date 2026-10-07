/*
 * HmRdp - manual resolution / scale field parsing.
 *
 * The editor and the settings page expose the same logical fields (manual
 * width/height, scale percentage), so their parsing bounds live here; the pages
 * only bind their own text fields.
 */
import { SCALE_PRESETS } from '../services/SettingsStore';

/** Upper bound for a remote desktop dimension. */
export const MAX_DESKTOP_SIZE: number = 16384;

/** Parses a positive pixel count, clamped to [1, max]; the fallback when invalid. */
export function parseSizeValue(text: string, fallback: number,
  max: number = MAX_DESKTOP_SIZE): number {
  const value: number = Number.parseInt(text, 10);
  if (Number.isNaN(value) || value <= 0) {
    return fallback;
  }
  return Math.min(value, max);
}

/** Parses a scale percentage, clamped to [100, 500]; the fallback when invalid. */
export function parseScaleValue(text: string, fallback: number): number {
  const value: number = Number.parseInt(text, 10);
  if (Number.isNaN(value) || value < 100) {
    return fallback;
  }
  return Math.min(value, 500);
}

/** The numeric value of a text field; 0 when it does not parse. */
export function parsedTextValue(text: string): number {
  const value: number = Number.parseInt(text, 10);
  return Number.isNaN(value) ? 0 : value;
}

/** Whether a scale value is one of the presets. */
export function isScalePreset(value: number): boolean {
  return SCALE_PRESETS.indexOf(value) >= 0;
}
