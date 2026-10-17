import { SR_BACKEND_FSR } from '../model/Profiles';
import { srRatioLabel } from '../services/SettingsStore';

// Session title tags are shared by both protocols: the resolution is followed by one bracket whose
// contents are comma-separated tokens (超分, negotiated codec, runtime warnings).
export function srTag(backend: number, ratioPercent: number): string {
  if (ratioPercent <= 100) {
    return '';
  }
  const name: string = backend === SR_BACKEND_FSR ? 'FSR' : 'XEngine';
  return name + ' ' + srRatioLabel(ratioPercent);
}

export function joinSessionTags(tokens: string[]): string {
  const parts: string[] = [];
  for (let i = 0; i < tokens.length; i++) {
    if (tokens[i].length > 0) {
      parts.push(tokens[i]);
    }
  }
  return parts.length > 0 ? ' (' + parts.join(', ') + ')' : '';
}
