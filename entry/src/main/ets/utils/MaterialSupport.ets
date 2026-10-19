import { deviceInfo } from '@kit.BasicServicesKit';
import { promptAction, uiMaterial } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN: number = 0xD001;
const TAG: string = 'HmRdpMaterial';

// 材质厚度档位。默认 = 跟随官方默认（不指定 style，各落点官方默认不同：
// Toast THICK、弹窗 ULTRA_THICK、气泡/菜单 undefined）。0..4 对应 ImmersiveStyle。
export const MATERIAL_STYLE_DEFAULT: number = -1;
export const MATERIAL_STYLE_MIN: number = -1;
export const MATERIAL_STYLE_MAX: number = 4;

// 沉浸光感（系统材质）能力探测与接入的唯一入口。API 26.0.0 起提供，且需设备实际支持。
// 探测时机很关键：在模块加载期（静态初始化）查询会误报“不支持”，必须等到 UI 就绪。
// 所以这里惰性探测——命中即固定，未命中则下次再探；调用点都在 UI 事件/构建期，晚于模块加载。
// 26.0.0 的 API 都放在 try/catch 内，满足 Code Linter 兼容性规则
// （@compatibility/api-compatibility-check；光有 apiAvailable 判断仍会告警）。
// 不支持 / 用户关闭时一律返回 false / 空对象，调用方回退到现有自绘样式。
// 只走官方允许的“弹窗/气泡/菜单/Toast 选项”通道，不给任意组件挂通用属性 systemMaterial。
export class MaterialSupport {
  private static latched: boolean = false;
  private static enabled: boolean = true;
  private static style: number = MATERIAL_STYLE_DEFAULT;

  // 由 SettingsStore 在每次落库/启动时同步（避免此处反向依赖设置存储）。
  static configure(enabled: boolean, style: number): void {
    MaterialSupport.enabled = enabled;
    MaterialSupport.style = style;
  }

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

  // 开关打开且设备支持：该落点应使用系统材质。
  static materialOn(): boolean {
    return MaterialSupport.enabled && MaterialSupport.isSupported();
  }

  // 显式档位；undefined 表示“跟随官方默认”，此时一律不设 systemMaterial。
  private static explicitStyle(): uiMaterial.ImmersiveStyle | undefined {
    try {
      const style: number = MaterialSupport.style;
      if (style === 0) {
        return uiMaterial.ImmersiveStyle.ULTRA_THIN;
      }
      if (style === 1) {
        return uiMaterial.ImmersiveStyle.THIN;
      }
      if (style === 2) {
        return uiMaterial.ImmersiveStyle.REGULAR;
      }
      if (style === 3) {
        return uiMaterial.ImmersiveStyle.THICK;
      }
      if (style === 4) {
        return uiMaterial.ImmersiveStyle.ULTRA_THICK;
      }
      return undefined;
    } catch (error) {
      return undefined;
    }
  }

  // 返回 true = 底色由材质负责，调用方不要写自绘回退。
  static applyToast(options: promptAction.ShowToastOptions): boolean {
    if (!MaterialSupport.materialOn()) {
      return false;
    }
    const style: uiMaterial.ImmersiveStyle | undefined = MaterialSupport.explicitStyle();
    if (style === undefined) {
      // 跟随官方默认：背景色/模糊/阴影都不设，自动开启 THICK 材质。
      return true;
    }
    try {
      options.systemMaterial = new uiMaterial.ImmersiveMaterial({ style: style });
      return true;
    } catch (error) {
      return false;
    }
  }

  static applyMenu(options: ContextMenuOptions): boolean {
    if (!MaterialSupport.materialOn()) {
      return false;
    }
    const style: uiMaterial.ImmersiveStyle | undefined = MaterialSupport.explicitStyle();
    if (style === undefined) {
      // 官方默认 = 不设材质，菜单保持系统默认样式。
      return true;
    }
    try {
      options.systemMaterial = new uiMaterial.ImmersiveMaterial({ style: style });
      return true;
    } catch (error) {
      return false;
    }
  }

  // 弹窗容器是否会有材质——决定内容层是否透明，必须与 dialogMaterial 同源判定。
  static dialogGlass(): boolean {
    return MaterialSupport.materialOn();
  }

  // undefined = 跟随官方默认（容器自动开启 ULTRA_THICK）。
  static dialogMaterial(): SystemUiMaterial | undefined {
    if (!MaterialSupport.materialOn()) {
      return undefined;
    }
    const style: uiMaterial.ImmersiveStyle | undefined = MaterialSupport.explicitStyle();
    if (style === undefined) {
      return undefined;
    }
    try {
      return new uiMaterial.ImmersiveMaterial({ style: style });
    } catch (error) {
      return undefined;
    }
  }

  static tipsOptions(): TipsOptions {
    const options: TipsOptions = {};
    if (!MaterialSupport.materialOn()) {
      return options;
    }
    const style: uiMaterial.ImmersiveStyle | undefined = MaterialSupport.explicitStyle();
    if (style === undefined) {
      // 官方默认 = 不设材质。
      return options;
    }
    try {
      options.systemMaterial = new uiMaterial.ImmersiveMaterial({ style: style });
    } catch (error) {
      return options;
    }
    return options;
  }
}