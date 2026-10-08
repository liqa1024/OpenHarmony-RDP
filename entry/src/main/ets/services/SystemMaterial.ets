/*
 * HmRdp - immersive system material (沉浸光感) support; see
 * doc_agent/arkts-conventions.md §6 for the rules and the surface list.
 *
 * uiMaterial (API 26) is newer than the declared compatibleSdkVersion, so the
 * capability is probed before the module is touched: without it every accessor
 * answers undefined and each surface keeps the flat background it declares. The
 * app only chooses which surfaces carry a material; how strong it is belongs to
 * the system. A material resamples what is behind it every frame, so it must
 * never sit on continuously changing content (the remote desktop image).
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
  OVERLAY,
  /** Small floating controls laid over a picture (e.g. the app-poster actions). */
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
    } else if (surface === MaterialSurface.CONTROL) {
      // Small floating controls over a picture: the official guide recommends
      // THIN / ULTRA_THIN for floating buttons. No materialColor: the material
      // follows the system and its own frosted layer supplies the contrast; a
      // tint would only flatten it.
      style = uiMaterial.ImmersiveStyle.THIN;
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
let control: uiMaterial.Material | undefined = undefined;

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

/**
 * Applies a material through the systemMaterial universal attribute. The
 * attribute only exists from API 26, so it is reached through a modifier rather
 * than written inline: an unimplemented attribute would abort the build of the
 * component it is written on.
 *
 * `fallbackBlur` is the flat-path substitute on systems without a material: a
 * component-level frosted style. It is applied *only* when there is no material,
 * because backgroundBlurStyle sits above the material layer and would hide it
 * (see the immersive-light-sense FAQ) — setting it to BlurStyle.NONE is not
 * enough, it must not be set at all.
 */
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
