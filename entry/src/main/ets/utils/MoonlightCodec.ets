/*
 * HmRdp - Moonlight video codec capability classification.
 *
 * The editor and the settings page both map an ML_CODEC_* choice to its local
 * decoder support (CODEC_SUPPORT_*), with "auto" taking the best any codec
 * offers. Shared so the two pages cannot drift; the caller passes its cached
 * per-codec support values.
 */
import { moonlightVideoCodec, moonlightVideoWarning } from 'libhmrdp.so';
import { ML_CODEC_AV1, ML_CODEC_H264, ML_CODEC_HEVC } from '../model/RdpModels';
import { CODEC_SUPPORT_NONE, CODEC_SUPPORT_SOFTWARE } from '../services/DeviceCapabilities';

/** Decoder support of an ML_CODEC_* choice; auto = the best of the three. */
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

/** Whether a codec choice can run on this device (auto included). */
export function moonlightCodecUsable(codec: number, h264: number, hevc: number,
  av1: number): boolean {
  return moonlightCodecSupport(codec, h264, hevc, av1) > CODEC_SUPPORT_NONE;
}

/** Whether a codec choice would decode in software only. */
export function moonlightCodecSoftware(codec: number, h264: number, hevc: number,
  av1: number): boolean {
  return moonlightCodecSupport(codec, h264, hevc, av1) === CODEC_SUPPORT_SOFTWARE;
}

/** Display name of a negotiated codec token ("h264" -> "H.264"); "" passes through. */
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

/**
 * Toolbar decode indicator for a live Moonlight stream, e.g. " (H.264, CPU)" or
 * " (HEVC)": the codec the host actually negotiated plus "CPU" when this device
 * has no hardware decoder for it. Empty before the decoder reports its codec.
 */
export function moonlightDecodeIndicator(): string {
  const token: string = moonlightVideoCodec();
  if (token.length === 0) {
    return '';
  }
  const software: boolean = moonlightVideoWarning().startsWith('software-decode');
  return ' (' + moonlightCodecTokenName(token) + (software ? ', CPU' : '') + ')';
}
