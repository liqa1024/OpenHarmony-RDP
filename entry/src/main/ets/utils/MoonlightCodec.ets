import { moonlightVideoCodec, moonlightVideoWarning } from 'libhmrdp.so';
import { ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC } from '../model/Connections';
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

export function moonlightDecodeIndicator(): string {
  const token: string = moonlightVideoCodec();
  if (token.length === 0) {
    return '';
  }
  const software: boolean = moonlightVideoWarning().startsWith('software-decode');
  return ' (' + moonlightCodecTokenName(token) + (software ? ', CPU' : '') + ')';
}
