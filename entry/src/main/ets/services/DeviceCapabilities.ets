import { audio } from '@kit.AudioKit';
import { SuppressWarnings, SuppressWarningsType } from '@kit.BasicServicesKit';
import {
  moonlightVideoCodecSupport, RdpSrBackends, RdpVerdict,
  superResolutionBackends, superResolutionSupport, vulkanAccelSupport
} from 'libhmrdp.so';

export class Capability {
  supported: boolean = true;
  reason: string = '';
}

// Mirrors the native codec-support enum.
export const CODEC_SUPPORT_NONE: number = 0;
export const CODEC_SUPPORT_SOFTWARE: number = 1;
export const CODEC_SUPPORT_HARDWARE: number = 2;

@SuppressWarnings({ rules: [SuppressWarningsType.SYSCAP] })
export class DeviceCapabilities {
  static audio(): Capability {
    const capability: Capability = new Capability();
    try {
      const devices: audio.AudioDeviceDescriptors = audio.getAudioManager()
        .getRoutingManager().getDevicesSync(audio.DeviceFlag.OUTPUT_DEVICES_FLAG);
      capability.supported = devices.length > 0;
    } catch (error) {
      capability.supported = false;
    }
    if (!capability.supported) {
      capability.reason = '本设备未提供音频输出能力，无法播放远端声音';
    }
    return capability;
  }

  // Real-device-only: the emulator reports Vulkan capabilities it does not honour (native verdict).
  static hardwareAccel(): Capability {
    const capability: Capability = new Capability();
    let verdict: RdpVerdict;
    try {
      verdict = vulkanAccelSupport();
    } catch (error) {
      capability.supported = false;
      capability.reason = '无法检测本设备的图形加速能力，已关闭硬件加速';
      return capability;
    }
    if (verdict.supported) {
      return capability;
    }
    capability.supported = false;
    capability.reason = DeviceCapabilities.hardwareAccelReason(verdict.code);
    return capability;
  }

  static superResolution(): Capability {
    const capability: Capability = new Capability();
    let verdict: RdpVerdict;
    try {
      verdict = superResolutionSupport();
    } catch (error) {
      capability.supported = false;
      capability.reason = '无法检测本设备的超分辨率能力';
      return capability;
    }
    if (verdict.supported) {
      return capability;
    }
    capability.supported = false;
    capability.reason = DeviceCapabilities.superResolutionReason(verdict.code);
    return capability;
  }

  static codecSupport(codec: number): number {
    try {
      return moonlightVideoCodecSupport(codec);
    } catch (error) {
      return CODEC_SUPPORT_NONE;
    }
  }

  // The Moonlight 超分 path additionally needs the device to import the decoder's NV12 buffer
  // into the presenter (VK_OHOS_external_memory + a YCbCr sampler).
  static srVideoImport(): boolean {
    let verdict: RdpSrBackends;
    try {
      verdict = superResolutionBackends();
    } catch (error) {
      return false;
    }
    return verdict.videoImport;
  }

  static srBackends(): string[] {
    let verdict: RdpSrBackends;
    try {
      verdict = superResolutionBackends();
    } catch (error) {
      return [];
    }
    const backends: string[] = [];
    if (verdict.xengine) {
      backends.push('xengine');
    }
    if (verdict.fsr) {
      backends.push('fsr');
    }
    return backends;
  }

  // The native side only returns reason codes; the wording lives here.
  private static superResolutionReason(code: string): string {
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

  // The native side only returns reason codes; the wording lives here.
  private static hardwareAccelReason(code: string): string {
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
