/*
 * HmRdp - runtime device capability probes.
 *
 * Some features depend on the device's system image rather than on the remote
 * server (remote audio needs an audio output). When a capability is missing the
 * feature is disabled in the UI with an explanation instead of failing later at
 * runtime; the native layer additionally degrades gracefully so the session
 * itself keeps working.
 */
import { isAudioSupported, superResolutionSupport, vulkanAccelSupport } from 'libhmrdp.so';

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
   * GPU (Vulkan) acceleration of the frame presentation. The native layer
   * answers with what the *presenter* actually needs - a Vulkan device,
   * host-visible memory for the frame buffer and a surface + swapchain to blit
   * to (no compute queue) - plus the emulator carve-out (Vulkan acceleration is
   * real-device-only, and the emulator reports capabilities it does not honour).
   * When this is false the session runs on the GLES presenter and Vulkan is not
   * touched at all.
   */
  static hardwareAccel(): Capability {
    const capability: Capability = new Capability();
    let verdict: string = '';
    try {
      verdict = vulkanAccelSupport();
    } catch (error) {
      capability.supported = false;
      capability.reason = '无法检测本设备的图形加速能力，已关闭硬件加速';
      return capability;
    }
    if (verdict === '1') {
      return capability;
    }
    capability.supported = false;
    capability.reason = DeviceCapabilities.hardwareAccelReason(verdict);
    return capability;
  }

  /**
   * GPU super-resolution (超分) of the presented frame: the session renders at a
   * lower resolution and the Vulkan presenter upscales it back with the platform
   * XEngine spatial upscale. Needs the Vulkan presenter to be usable at all, so
   * it is a real-device-only capability like 硬件加速.
   */
  static superResolution(): Capability {
    const capability: Capability = new Capability();
    let verdict: string = '';
    try {
      verdict = superResolutionSupport();
    } catch (error) {
      capability.supported = false;
      capability.reason = '无法检测本设备的超分辨率能力';
      return capability;
    }
    if (verdict === '1') {
      return capability;
    }
    capability.supported = false;
    capability.reason = DeviceCapabilities.superResolutionReason(verdict);
    return capability;
  }

  /** Wording for the native reason token (the native side only returns codes). */
  private static superResolutionReason(verdict: string): string {
    const code: string = verdict.startsWith('0|') ? verdict.substring(2) : '';
    if (code === 'emulator' || code === 'no-vulkan' || code === 'no-instance'
      || code === 'no-device' || code === 'no-host-memory' || code === 'no-surface') {
      return '超分辨率需要可用的硬件加速（Vulkan 上屏），本设备不满足';
    }
    if (code === 'no-xengine') {
      return '本设备未提供超分辨率所需的图形加速库（libxengine）';
    }
    if (code === 'no-extension') {
      return '本设备的 GPU 不支持超分辨率所需的空域上采样特性';
    }
    return '本设备的超分辨率能力不足，超分辨率不可用';
  }

  /** Wording for the native reason token (the native side only returns codes). */
  private static hardwareAccelReason(verdict: string): string {
    const code: string = verdict.startsWith('0|') ? verdict.substring(2) : '';
    if (code === 'emulator') {
      return '模拟器不提供可信的 Vulkan，硬件加速仅在真机可用';
    }
    if (code === 'no-vulkan') {
      return '本设备没有可用的 Vulkan 驱动，硬件加速不可用（已用 GLES 上屏）';
    }
    if (code === 'no-host-memory') {
      return '本设备的 Vulkan 未提供 host-visible 内存，硬件加速不可用（已用 GLES 上屏）';
    }
    if (code === 'no-surface') {
      return '本设备的 Vulkan 不支持 XComponent 上屏所需的表面/交换链扩展';
    }
    return '本设备的图形加速能力不足，硬件加速不可用（已用 GLES 上屏）';
  }
}
