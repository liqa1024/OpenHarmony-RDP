/*
 * HmRdp - runtime device capability probes.
 *
 * Some features depend on the device's system image rather than on the remote
 * server (remote audio needs an audio output). When a capability is missing the
 * feature is disabled in the UI with an explanation instead of failing later at
 * runtime; the native layer additionally degrades gracefully so the session
 * itself keeps working.
 */
import { isAudioSupported, vulkanEngineSupport } from 'libhmrdp.so';

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

  /**
   * GPU (Vulkan) hardware decoding of the RemoteFX/Progressive streams. The
   * native layer answers with what the engine actually needs - a Vulkan device,
   * a queue family that can run the decode compute dispatches, host-visible
   * memory for the surface buffers and a surface + swapchain to present to -
   * plus the emulator carve-out (GPU acceleration is real-device-only, and the
   * emulator reports capabilities it does not honour).
   */
  static hardwareDecode(): Capability {
    const capability: Capability = new Capability();
    let verdict: string = '';
    try {
      verdict = vulkanEngineSupport();
    } catch (error) {
      capability.supported = false;
      capability.reason = '无法检测本设备的图形加速能力，已关闭硬件解码';
      return capability;
    }
    if (verdict === '1') {
      return capability;
    }
    capability.supported = false;
    capability.reason = DeviceCapabilities.hardwareDecodeReason(verdict);
    return capability;
  }

  /** Wording for the native reason token (the native side only returns codes). */
  private static hardwareDecodeReason(verdict: string): string {
    const code: string = verdict.startsWith('0|') ? verdict.substring(2) : '';
    if (code === 'emulator') {
      return '模拟器不提供可信的图形加速，硬件解码仅在真机可用';
    }
    if (code === 'no-vulkan') {
      return '本设备没有可用的 Vulkan 驱动，硬件解码不可用';
    }
    if (code === 'no-compute') {
      return '本设备的 Vulkan 不支持计算队列，无法加速解码';
    }
    if (code === 'no-host-memory') {
      return '本设备的 Vulkan 未提供 host-visible 内存，无法加速解码';
    }
    if (code === 'no-surface') {
      return '本设备的 Vulkan 不支持 XComponent 上屏所需的表面/交换链扩展';
    }
    return '本设备的图形加速能力不足，硬件解码不可用';
  }
}
