import { SCALE_PRESETS } from '../services/SettingsStore';

export const MAX_DESKTOP_SIZE: number = 16384;

export function parseSizeValue(text: string, fallback: number,
  max: number = MAX_DESKTOP_SIZE): number {
  const value: number = Number.parseInt(text, 10);
  if (Number.isNaN(value) || value <= 0) {
    return fallback;
  }
  return Math.min(value, max);
}

export function parseScaleValue(text: string, fallback: number): number {
  const value: number = Number.parseInt(text, 10);
  if (Number.isNaN(value) || value < 100) {
    return fallback;
  }
  return Math.min(value, 500);
}

export function parsedTextValue(text: string): number {
  const value: number = Number.parseInt(text, 10);
  return Number.isNaN(value) ? 0 : value;
}

export function isScalePreset(value: number): boolean {
  return SCALE_PRESETS.indexOf(value) >= 0;
}
