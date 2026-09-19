/*
 * HmRdp - immersive system material (沉浸光感) support.
 *
 * uiMaterial (API 26) is newer than the compatibleSdkVersion the app declares,
 * so the capability is probed before the module is touched at all: on a system
 * without it every accessor answers undefined, the universal attribute is never
 * applied and each surface keeps the flat background it declares.
 *
 * How strong the effect is - and whether it is applied at all - belongs to the
 * system. A material follows the device's compute tier and the user's 沉浸光感
 * setting, and nothing in the app can read or set that strength, so nothing
 * here tries to: the app only chooses which surfaces carry a material.
 *
 * Materials are applied in two shapes, matching how the component exposes the
 * attribute:
 *  - universal attribute (systemMaterial) -> ImmersiveModifier, so the call is
 *    only made on a system that has the attribute;
 *  - an options object (toast, tips, context menu, alert dialog) -> the
 *    material is passed as a field, which an older system simply ignores.
 *
 * Placement rule that drives the surface list: a material resamples whatever is
 * behind it every frame, so it must never sit on the remote desktop image or
 * any other continuously changing content.
 */
import { deviceInfo } from '@kit.BasicServicesKit';
import { uiMaterial } from '@kit.ArkUI';

/** First API level that provides uiMaterial (see @ohos.arkui.uiMaterial). */
const IMMERSIVE_MATERIAL_MIN_API: number = 26;

/** A surface that can carry a material; each maps to one material style. */
export enum MaterialSurface {
  /** Floating chrome laid over live content - the session toolbar. */
  FLOATING,
  /** Cards and other regular content containers. */
  CARD,
  /** Menus, tips and toasts, which have to obscure what is behind them. */
  OVERLAY
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

/**
 * Built once per surface and reused: the system recomputes a material whenever
 * its parameters change, so rebuilding one on every render would keep the
 * material layer dirty.
 */
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
      // The cards keep their own shadow; a material shadow on top of it would be
      // a second, conflicting one.
      applyShadow = false;
    } else if (surface === MaterialSurface.OVERLAY) {
      style = uiMaterial.ImmersiveStyle.THICK;
    } else {
      // The session toolbar floats over the remote desktop image, whose colours
      // are unknown, so it stays dark chrome in both system light and dark mode
      // (see SessionPage's toolbar palette). The tint pins the material to the
      // app's dark surface instead of letting it follow the system, while a
      // translucent value still leaves the material filter visible.
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

export class SystemMaterial {
  /** Whether this system renders immersive system material at all. */
  static readonly supported: boolean = SUPPORTED;

  /**
   * Material for a surface, or undefined where the system cannot render one.
   * Call sites pair it with a flat fallback background, so both branches are
   * written out:
   *
   *   .backgroundColor(SystemMaterial.supported ? Color.Transparent : '#CC1A1A1A')
   *   .attributeModifier(this.toolbarMaterial)
   */
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
    if (floating === undefined) {
      floating = build(surface);
    }
    return floating;
  }
}

/**
 * Applies a material through the systemMaterial universal attribute. The
 * attribute only exists from API 26, so it is reached through a modifier rather
 * than written inline: an unimplemented attribute would abort the build of the
 * component it is written on.
 */
export class ImmersiveModifier<T extends CommonMethod<T>> implements AttributeModifier<T> {
  private readonly material: uiMaterial.Material | undefined;

  constructor(material: uiMaterial.Material | undefined) {
    this.material = material;
  }

  applyNormalAttribute(instance: T): void {
    // A material only exists when the system has the capability, so this is the
    // fallback path and the attribute is never reached there.
    if (this.material === undefined) {
      return;
    }
    if (deviceInfo.apiAvailable('26.0.0')) {
      instance.systemMaterial(this.material);
    }
  }
}
