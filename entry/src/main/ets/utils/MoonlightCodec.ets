import { moonlightVideoCodec, moonlightVideoWarning } from 'libhmrdp.so';
import { ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC } from '../moonlight/MoonlightModels';
import { CODEC_SUPPORT_NONE, CODEC_SUPPORT_SOFTWARE } from '../services/DeviceCapabilities';

// SCM_* family masks the host advertises in ServerCodecModeSupport (Limelight.h).
const SCM_MASK_HEVC: number = 0x00180300;
const SCM_MASK_AV1: number = 0x00630000;

export function moonlightCodecSupport(codec: number, h264: number, hevc: number,
  av1: number): number {
  if (codec === ML_CODEC_H264) {
    return h264;
  }
  if (codec === ML_CODEC_HEVC) {
    return hevc;
  }
  if (codec === ML_CODEC_AV1) {
    return av1;
  }
  return Math.max(h264, hevc, av1);
}

export function moonlightCodecUsable(codec: number, h264: number, hevc: number,
  av1: number): boolean {
  return moonlightCodecSupport(codec, h264, hevc, av1) > CODEC_SUPPORT_NONE;
}

export function moonlightCodecSoftware(codec: number, h264: number, hevc: number,
  av1: number): boolean {
  return moonlightCodecSupport(codec, h264, hevc, av1) === CODEC_SUPPORT_SOFTWARE;
}

export function moonlightCodecTokenName(token: string): string {
  if (token === 'h264') {
    return 'H.264';
  }
  if (token === 'hevc') {
    return 'HEVC';
  }
  if (token === 'av1') {
    return 'AV1';
  }
  return token;
}

// Host-side codecs, read from the ServerCodecModeSupport mask: H.264 is the
// library's unconditional fallback, HEVC / AV1 need their SCM_* family bit.
export function moonlightHostCodecNames(serverCodecModeSupport: number): string {
  const names: string[] = ['H.264'];
  if ((serverCodecModeSupport & SCM_MASK_HEVC) !== 0) {
    names.push('HEVC');
  }
  if ((serverCodecModeSupport & SCM_MASK_AV1) !== 0) {
    names.push('AV1');
  }
  return names.join(' / ');
}

// Tokens for the session title; the caller merges them with the other session tags into one bracket.
// The warning is a comma-separated list (a codec warning, an unavailable/inactive 超分 and a presentation
// failure are independent facts and may all be present).
export function moonlightDecodeTokens(): string[] {
  const token: string = moonlightVideoCodec();
  const warning: string = moonlightVideoWarning();
  const tokens: string[] = [];
  if (token.length > 0) {
    tokens.push(moonlightCodecTokenName(token));
  }
  let software: boolean = false;
  let srUnavailable: boolean = false;
  let srInactive: boolean = false;
  let videoError: boolean = false;
  const parts: string[] = warning.length > 0 ? warning.split(',') : [];
  for (let i = 0; i < parts.length; i++) {
    const part: string = parts[i];
    if (part.startsWith('software-decode')) {
      software = true;
    } else if (part.startsWith('sr-unavailable')) {
      srUnavailable = true;
    } else if (part.startsWith('sr-inactive')) {
      srInactive = true;
    } else if (part.startsWith('video-present')) {
      videoError = true;
    }
  }
  if (software) {
    tokens.push('CPU');
  }
  // A requested 超分 that is not running must not keep claiming to be on in the title.
  if (srUnavailable || srInactive) {
    tokens.push('未超分');
  }
  if (videoError) {
    tokens.push('视频异常');
  }
  return tokens;
}

export function moonlightVideoStatus(): string {
  return moonlightVideoWarning();
}
