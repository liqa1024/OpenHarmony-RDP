/*
 * HmRdp - runtime device capability probes.
 *
 * Some features depend on the device's system image rather than on the remote
 * server (remote audio needs an audio output). When a capability is missing the
 * feature is disabled in the UI with an explanation instead of failing later at
 * runtime; the native layer additionally degrades gracefully so the session
 * itself keeps working.
 */
import { isAudioSupported } from 'libhmrdp.so';

/** Support state of one optional feature. */
export class Capability {
  supported: boolean = true;
  reason: string = '';
}

export class DeviceCapabilities {
  /**
   * Remote audio playback. The native layer probes libohaudio.so with dlopen,
   * so this is authoritative for the OHAudio-based player (no ArkTS audio API
   * is used for playback).
   */
  static audio(): Capability {
    const capability: Capability = new Capability();
    try {
      capability.supported = isAudioSupported();
    } catch (error) {
      capability.supported = false;
    }
    if (!capability.supported) {
      capability.reason = '本设备未提供音频输出能力，无法播放远端声音';
    }
    return capability;
  }
}
