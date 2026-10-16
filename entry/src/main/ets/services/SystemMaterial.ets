import { deviceInfo } from '@kit.BasicServicesKit';
import { uiMaterial } from '@kit.ArkUI';

const IMMERSIVE_MATERIAL_MIN_API: number = 26;
const PROBE_RETRY_MS: number = 200;
// The toolbar sits over the unknown remote image, so its material is tinted dark instead of following the
// system (palette: components/SessionChrome.ets).
const FLOATING_TINT: string = '#B31B1B1D';

export enum MaterialSurface {
  FLOATING,
  CARD,
  OVERLAY,
  DIALOG,
  CONTROL
}

function querySupported(): boolean {
  if (deviceInfo.sdkApiVersion < IMMERSIVE_MATERIAL_MIN_API) {
    // deviceInfo.apiAvailable() is itself newer than this system, so it is not
    // reachable there; the base API level is all that can be asked.
    return false;
  }
  // The version is spelled out rather than taken from a constant: the compiler
  // only recognises a literal here and validates (and silences) the guarded
  // calls below against it.
  if (deviceInfo.apiAvailable('26.0.0')) {
    try {
      return uiMaterial.isImmersiveMaterialSupported();
    } catch (error) {
      return false;
    }
  }
  return false;
}

// The capability query only answers truthfully once the system is up: at module-evaluation
// time it reports unsupported + SMOOTH, so the answer is asked for at first use, kept only
// when positive and re-asked (throttled) while negative (see arkts-conventions.md §6).
let resolved: boolean = false;
let lastQueryMs: number = 0;

function supportedNow(): boolean {
  if (resolved) {
    return true;
  }
  const now: number = Date.now();
  if (lastQueryMs !== 0 && now - lastQueryMs < PROBE_RETRY_MS) {
    return false;
  }
  lastQueryMs = now;
  resolved = querySupported();
  return resolved;
}

// The style is what decides how much a surface obscures what is behind it, so every surface names its own
// instead of inheriting one (mapping and criteria: arkts-conventions.md §6).
function build(surface: MaterialSurface): uiMaterial.Material | undefined {
  if (deviceInfo.apiAvailable('26.0.0')) {
    let style: uiMaterial.ImmersiveStyle = uiMaterial.ImmersiveStyle.REGULAR;
    let applyShadow: boolean = true;
    let materialColor: ResourceColor | undefined = undefined;
    if (surface === MaterialSurface.FLOATING) {
      style = uiMaterial.ImmersiveStyle.THIN;
      materialColor = FLOATING_TINT;
    } else if (surface === MaterialSurface.CARD) {
      style = uiMaterial.ImmersiveStyle.REGULAR;
      applyShadow = false;
    } else if (surface === MaterialSurface.CONTROL) {
      style = uiMaterial.ImmersiveStyle.THIN;
    } else if (surface === MaterialSurface.OVERLAY) {
      style = uiMaterial.ImmersiveStyle.THICK;
    } else if (surface === MaterialSurface.DIALOG) {
      style = uiMaterial.ImmersiveStyle.ULTRA_THICK;
    } else {
      return undefined;
    }
    return new uiMaterial.ImmersiveMaterial({
      style: style,
      materialColor: materialColor,
      applyShadow: applyShadow
    });
  }
  return undefined;
}

// Mirrors the material thickness, so a surface without a material still reads as glass: component styles
// carry their own theme-aware fill.
function fallbackBlur(surface: MaterialSurface): BlurStyle {
  if (surface === MaterialSurface.CARD) {
    return BlurStyle.COMPONENT_REGULAR;
  }
  if (surface === MaterialSurface.OVERLAY) {
    return BlurStyle.COMPONENT_THICK;
  }
  if (surface === MaterialSurface.DIALOG) {
    return BlurStyle.COMPONENT_ULTRA_THICK;
  }
  return BlurStyle.COMPONENT_THIN;
}

// Built once per surface and reused: rebuilding a material keeps its layer dirty.
const MATERIALS: Map<MaterialSurface, uiMaterial.Material> = new Map();

export class SystemMaterial {
  static of(surface: MaterialSurface): uiMaterial.Material | undefined {
    if (!supportedNow()) {
      return undefined;
    }
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

// Applied through a modifier because the attribute only exists from API 26 (an inline one would abort the
// build on older systems). The branches are exclusive — backgroundBlurStyle sits above the material layer —
// and the base colour belongs to the branch, not to the call site: a material is drawn below the
// background, so only the branch without a material may be opaque.
export class ImmersiveModifier<T extends CommonMethod<T>> implements AttributeModifier<T> {
  private readonly surface: MaterialSurface;
  private readonly fallbackBase: ResourceColor;

  constructor(surface: MaterialSurface, fallbackBase: ResourceColor = Color.Transparent) {
    this.surface = surface;
    this.fallbackBase = fallbackBase;
  }

  applyNormalAttribute(instance: T): void {
    const material: uiMaterial.Material | undefined = SystemMaterial.of(this.surface);
    if (material === undefined) {
      instance.backgroundColor(this.fallbackBase);
      if (this.surface === MaterialSurface.FLOATING) {
        // Without a material the bar keeps its dark tone, so the frosted style is pinned to dark mode
        // instead of following the system.
        instance.backgroundBlurStyle(fallbackBlur(this.surface), { colorMode: ThemeColorMode.DARK });
      } else {
        instance.backgroundBlurStyle(fallbackBlur(this.surface));
      }
      return;
    }
    instance.backgroundColor(Color.Transparent);
    if (deviceInfo.apiAvailable('26.0.0')) {
      instance.systemMaterial(material);
    }
  }
}
