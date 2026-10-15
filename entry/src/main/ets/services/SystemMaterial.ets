import { deviceInfo } from '@kit.BasicServicesKit';
import { uiMaterial } from '@kit.ArkUI';

const IMMERSIVE_MATERIAL_MIN_API: number = 26;
const PROBE_RETRY_MS: number = 200;

export enum MaterialSurface {
  FLOATING,
  CARD,
  OVERLAY,
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

function build(surface: MaterialSurface): uiMaterial.Material | undefined {
  if (deviceInfo.apiAvailable('26.0.0')) {
    let style: uiMaterial.ImmersiveStyle = uiMaterial.ImmersiveStyle.THIN;
    let applyShadow: boolean = true;
    let materialColor: ResourceColor | undefined = undefined;
    if (surface === MaterialSurface.CARD) {
      style = uiMaterial.ImmersiveStyle.REGULAR;
      applyShadow = false;
    } else if (surface === MaterialSurface.OVERLAY) {
      style = uiMaterial.ImmersiveStyle.THICK;
    } else if (surface === MaterialSurface.CONTROL) {
      style = uiMaterial.ImmersiveStyle.THIN;
    } else {
      materialColor = '#B31B1B1D';
    }
    return new uiMaterial.ImmersiveMaterial({
      style: style,
      materialColor: materialColor,
      applyShadow: applyShadow
    });
  }
  return undefined;
}

// Fallback thickness mirrors the material one, so a system without a material still gets a
// frosted surface instead of a flat colour; the component styles carry their own theme-aware
// fill, which is why only the fixed-dark surface keeps an explicit background of its own.
function fallbackBlur(surface: MaterialSurface): BlurStyle {
  if (surface === MaterialSurface.CARD) {
    return BlurStyle.COMPONENT_REGULAR;
  }
  if (surface === MaterialSurface.OVERLAY) {
    return BlurStyle.COMPONENT_THICK;
  }
  return BlurStyle.COMPONENT_THIN;
}

let floating: uiMaterial.Material | undefined = undefined;
let card: uiMaterial.Material | undefined = undefined;
let overlay: uiMaterial.Material | undefined = undefined;
let control: uiMaterial.Material | undefined = undefined;

export class SystemMaterial {
  static of(surface: MaterialSurface): uiMaterial.Material | undefined {
    if (!supportedNow()) {
      return undefined;
    }
    if (surface === MaterialSurface.CARD) {
      if (card === undefined) {
        card = build(surface);
      }
      return card;
    }
    if (surface === MaterialSurface.OVERLAY) {
      if (overlay === undefined) {
        overlay = build(surface);
      }
      return overlay;
    }
    if (surface === MaterialSurface.CONTROL) {
      if (control === undefined) {
        control = build(surface);
      }
      return control;
    }
    if (floating === undefined) {
      floating = build(surface);
    }
    return floating;
  }
}

// The systemMaterial attribute only exists from API 26, so it is applied through a modifier (an unimplemented inline attribute would abort the component build). A surface without a material gets the matching component frosted style instead: backgroundBlurStyle sits above the material layer, so the two are never set together.
export class ImmersiveModifier<T extends CommonMethod<T>> implements AttributeModifier<T> {
  private readonly surface: MaterialSurface;

  constructor(surface: MaterialSurface) {
    this.surface = surface;
  }

  applyNormalAttribute(instance: T): void {
    const material: uiMaterial.Material | undefined = SystemMaterial.of(this.surface);
    if (material === undefined) {
      if (this.surface === MaterialSurface.FLOATING) {
        // The bar floats over the remote image, so it keeps a dark tone instead of following the system.
        instance.backgroundBlurStyle(fallbackBlur(this.surface), { colorMode: ThemeColorMode.DARK });
      } else {
        instance.backgroundBlurStyle(fallbackBlur(this.surface));
      }
      return;
    }
    if (deviceInfo.apiAvailable('26.0.0')) {
      instance.systemMaterial(material);
    }
  }
}
