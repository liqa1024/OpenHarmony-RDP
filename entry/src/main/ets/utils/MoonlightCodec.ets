import { moonlightVideoCodec, moonlightVideoWarning } from 'libhmrdp.so';
import { ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC } from '../model/RdpModels';
import { CODEC_SUPPORT_NONE, CODEC_SUPPORT_SOFTWARE } from '../services/DeviceCapabilities';

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

export function moonlightDecodeIndicator(): string {
  const token: string = moonlightVideoCodec();
  if (token.length === 0) {
    return '';
  }
  const software: boolean = moonlightVideoWarning().startsWith('software-decode');
  return ' (' + moonlightCodecTokenName(token) + (software ? ', CPU' : '') + ')';
}
