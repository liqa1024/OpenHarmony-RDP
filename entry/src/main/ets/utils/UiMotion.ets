/*
 * HmRdp - shared interaction motion (hover / press / appear / disappear).
 *
 * One place for the interaction timings and curves and for the small
 * state-driven helpers, so every control animates the same way (see
 * doc_agent/ui-design.md §1.3). ArkUI has no shared declarative hover / press
 * effect: each component keeps its own @State, but reads its numeric targets and
 * transition effects from here so the feel does not drift between controls.
 *
 * Appear / disappear is a component transition: the caller inserts / removes the
 * subtree with an `if`, and the effect carries its own animation params, so no
 * animateTo is needed (see the component-transition guide).
 */

/** Hover / press feedback, shared by the FAB and round on-surface controls. */
export const MOTION_HOVER_SCALE: number = 1.06;
export const MOTION_PRESS_SCALE: number = 0.9;
export const MOTION_HOVER_PRESS_MS: number = 140;
export const MOTION_CURVE: Curve = Curve.FastOutSlowIn;

/**
 * The hover / press scale for a control, from its own @State flags. Press wins
 * over hover so a click inside a hovered control still reads as a press.
 */
export function hoverPressScale(hovered: boolean, pressed: boolean): ScaleOptions {
  const scale: number = pressed ? MOTION_PRESS_SCALE
    : (hovered ? MOTION_HOVER_SCALE : 1.0);
  const options: ScaleOptions = { x: scale, y: scale };
  return options;
}

const MOTION_APPEAR_MS: number = 180;
const MOTION_DISAPPEAR_MS: number = 140;
// Scale a control grows from (appear) / shrinks to (disappear): a small "pop"
// that reads as the control being added to / taken off the surface.
const MOTION_POP_SCALE: number = 0.6;

function pop(durationMs: number): TransitionEffect {
  const scale: ScaleOptions = { x: MOTION_POP_SCALE, y: MOTION_POP_SCALE };
  return TransitionEffect.OPACITY
    .animation({ duration: durationMs, curve: MOTION_CURVE })
    .combine(TransitionEffect.scale(scale));
}

/**
 * The appear / disappear transition for a control: fade + pop on insert, fade +
 * shrink on removal. Attach it to the root of an `if`-toggled control subtree.
 */
export function popTransition(): TransitionEffect {
  return TransitionEffect.asymmetric(pop(MOTION_APPEAR_MS), pop(MOTION_DISAPPEAR_MS));
}

function fade(durationMs: number): TransitionEffect {
  return TransitionEffect.OPACITY.animation({ duration: durationMs, curve: MOTION_CURVE });
}

/**
 * A fade-only appear / disappear transition, for a group whose members already
 * pop individually: the group root only has to hold the subtree long enough for
 * the whole removal to play out.
 */
export function fadeTransition(): TransitionEffect {
  return TransitionEffect.asymmetric(fade(MOTION_APPEAR_MS), fade(MOTION_DISAPPEAR_MS));
}
