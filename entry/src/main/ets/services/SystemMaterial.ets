// uiMaterial (API 26) is newer than the declared compatibleSdkVersion, so the capability is probed before the module is touched; a material resamples what is behind it every frame, so it must never sit on continuously changing content (the remote desktop image).
import { deviceInfo } from '@kit.BasicServicesKit';
import { uiMaterial } from '@kit.ArkUI';

const IMMERSIVE_MATERIAL_MIN_API: number = 26;

export enum MaterialSurface {
  FLOATING,
  CARD,
  OVERLAY,
  CONTROL
}

function probe(): boolean {
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

const SUPPORTED: boolean = probe();

// Built once per surface and reused: the system recomputes a material whenever its parameters change, so rebuilding per render would keep the layer dirty.
function build(surface: MaterialSurface): uiMaterial.Material | undefined {
  if (!SUPPORTED) {
    return undefined;
  }
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

let floating: uiMaterial.Material | undefined = undefined;
let card: uiMaterial.Material | undefined = undefined;
let overlay: uiMaterial.Material | undefined = undefined;
let control: uiMaterial.Material | undefined = undefined;

export class SystemMaterial {
  static readonly supported: boolean = SUPPORTED;

  static of(surface: MaterialSurface): uiMaterial.Material | undefined {
    if (!SUPPORTED) {
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

// The systemMaterial attribute only exists from API 26, so it is applied through a modifier (an unimplemented inline attribute would abort the component build). fallbackBlur is used only when there is no material, because backgroundBlurStyle sits above the material and would hide it.
export class ImmersiveModifier<T extends CommonMethod<T>> implements AttributeModifier<T> {
  private readonly material: uiMaterial.Material | undefined;
  private readonly fallbackBlur: BlurStyle | undefined;

  constructor(material: uiMaterial.Material | undefined, fallbackBlur?: BlurStyle) {
    this.material = material;
    this.fallbackBlur = fallbackBlur;
  }

  applyNormalAttribute(instance: T): void {
    if (this.material === undefined) {
      if (this.fallbackBlur !== undefined) {
        instance.backgroundBlurStyle(this.fallbackBlur);
      }
      return;
    }
    if (deviceInfo.apiAvailable('26.0.0')) {
      instance.systemMaterial(this.material);
    }
  }
}
