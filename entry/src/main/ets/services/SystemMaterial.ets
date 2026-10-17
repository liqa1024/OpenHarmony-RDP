import { deviceInfo } from '@kit.BasicServicesKit';
import { uiMaterial } from '@kit.ArkUI';

const IMMERSIVE_MATERIAL_MIN_API: number = 26;

// Content surfaces never take a system material: the platform only renders one
// in popups and in a Navigation title bar / bottom TabBar. Frosting is reserved
// for small controls and the floating session bar; large content surfaces stay
// plain transparent so a wallpaper reads through them.
export enum FrostedSurface {
  FLOATING,
  CARD,
  CONTROL
}

// The popup surfaces are the only place that keeps the system material.
export enum MaterialSurface {
  OVERLAY,
  DIALOG
}

// Popup materials mirror the system default thickness for their kind; they still
// follow the system's light/dark and immersive-strength settings. The capability
// query only gates the content fallback (a transparent dialog needs a real
// backplate), never a content surface's style.
function build(surface: MaterialSurface): uiMaterial.Material | undefined {
  if (deviceInfo.sdkApiVersion < IMMERSIVE_MATERIAL_MIN_API) {
    // deviceInfo.apiAvailable() is itself newer than this system, so it is not
    // reachable there; the base API level is all that can be asked.
    return undefined;
  }
  // The version is spelled out rather than taken from a constant: the compiler
  // only recognises a literal here and validates (and silences) the guarded
  // calls below against it.
  if (deviceInfo.apiAvailable('26.0.0')) {
    try {
      if (!uiMaterial.isImmersiveMaterialSupported()) {
        return undefined;
      }
    } catch (error) {
      return undefined;
    }
    const style: uiMaterial.ImmersiveStyle = surface === MaterialSurface.DIALOG
      ? uiMaterial.ImmersiveStyle.ULTRA_THICK
      : uiMaterial.ImmersiveStyle.THICK;
    return new uiMaterial.ImmersiveMaterial({ style: style });
  }
  return undefined;
}

// Built once per surface and reused: rebuilding a material keeps its layer dirty.
const MATERIALS: Map<MaterialSurface, uiMaterial.Material> = new Map();

export class SystemMaterial {
  static of(surface: MaterialSurface): uiMaterial.Material | undefined {
    const built: uiMaterial.Material | undefined = MATERIALS.get(surface);
    if (built !== undefined) {
      return built;
    }
    const material: uiMaterial.Material | undefined = build(surface);
    if (material !== undefined) {
      MATERIALS.set(surface, material);
    }
    return material;
  }
}

// `CARD` is the session status panel only: it sits on a dark scrim over the remote
// picture and needs a surface of its own for contrast. Wallpaper pages keep their
// large surfaces transparent and frost only the small controls (`CONTROL`).
export class FrostedModifier<T extends CommonMethod<T>> implements AttributeModifier<T> {
  private readonly surface: FrostedSurface;

  constructor(surface: FrostedSurface) {
    this.surface = surface;
  }

  applyNormalAttribute(instance: T): void {
    if (this.surface === FrostedSurface.FLOATING) {
      instance.backgroundBlurStyle(BlurStyle.COMPONENT_THIN, { colorMode: ThemeColorMode.DARK });
      return;
    }
    instance.backgroundBlurStyle(this.surface === FrostedSurface.CONTROL
      ? BlurStyle.COMPONENT_THIN : BlurStyle.COMPONENT_REGULAR);
  }
}
