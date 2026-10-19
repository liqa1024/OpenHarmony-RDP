import { deviceInfo } from '@kit.BasicServicesKit';
import { promptAction, uiMaterial } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMaterial';

// 沉浸光感（系统材质）能力探测与接入的唯一入口。API 26.0.0 起提供，且需设备实际支持。
// 探测时机很关键：在模块加载期（静态初始化）查询会误报“不支持”，必须等到 UI 就绪。
// 所以这里惰性探测——命中即固定，未命中则下次再探；调用点都在 UI 事件/构建期，晚于模块加载。
// 26.0.0 的 API 都放在 try/catch 内，满足 Code Linter 兼容性规则
// （@compatibility/api-compatibility-check；光有 apiAvailable 判断仍会告警）。
// 不支持时返回 false / 空对象，调用方回退到现有自绘样式。
// 只走官方允许的“弹窗/气泡/菜单/Toast 选项”通道，不给任意组件挂通用属性 systemMaterial。
export class MaterialSupport {
  private static latched: boolean = false;

  static isSupported(): boolean {
    if (MaterialSupport.latched) {
      return true;
    }
    let supported: boolean = false;
    try {
      supported = deviceInfo.apiAvailable('26.0.0') && uiMaterial.isImmersiveMaterialSupported();
    } catch (error) {
      supported = false;
    }
    if (supported) {
      MaterialSupport.latched = true;
      hilog.info(DOMAIN, TAG, 'immersive material available');
    }
    return supported;
  }

  // 已写入系统材质返回 true；false 时调用方走现有自绘样式。
  static applyToast(options: promptAction.ShowToastOptions): boolean {
    if (!MaterialSupport.isSupported()) {
      return false;
    }
    try {
      options.systemMaterial = new uiMaterial.ImmersiveMaterial({
        style: uiMaterial.ImmersiveStyle.THICK
      });
      return true;
    } catch (error) {
      return false;
    }
  }

  static applyMenu(options: ContextMenuOptions): boolean {
    if (!MaterialSupport.isSupported()) {
      return false;
    }
    try {
      options.systemMaterial = new uiMaterial.ImmersiveMaterial({
        style: uiMaterial.ImmersiveStyle.THICK
      });
      return true;
    } catch (error) {
      return false;
    }
  }

  static tipsOptions(): TipsOptions {
    const options: TipsOptions = {};
    if (!MaterialSupport.isSupported()) {
      return options;
    }
    try {
      options.systemMaterial = new uiMaterial.ImmersiveMaterial({
        style: uiMaterial.ImmersiveStyle.THIN
      });
    } catch (error) {
      return options;
    }
    return options;
  }
}