
export const MOTION_HOVER_SCALE: number = 1.06;
export const MOTION_PRESS_SCALE: number = 0.9;
export const MOTION_HOVER_PRESS_MS: number = 140;
export const MOTION_CURVE: Curve = Curve.FastOutSlowIn;

export function hoverPressScale(hovered: boolean, pressed: boolean): ScaleOptions {
  const scale: number = pressed ? MOTION_PRESS_SCALE
    : (hovered ? MOTION_HOVER_SCALE : 1.0);
  const options: ScaleOptions = { x: scale, y: scale };
  return options;
}

const MOTION_APPEAR_MS: number = 300;
const MOTION_DISAPPEAR_MS: number = 240;
const MOTION_POP_SCALE: number = 0.6;

function pop(durationMs: number): TransitionEffect {
  const scale: ScaleOptions = { x: MOTION_POP_SCALE, y: MOTION_POP_SCALE };
  return TransitionEffect.OPACITY
    .animation({ duration: durationMs, curve: MOTION_CURVE })
    .combine(TransitionEffect.scale(scale));
}

export function popTransition(): TransitionEffect {
  return TransitionEffect.asymmetric(pop(MOTION_APPEAR_MS), pop(MOTION_DISAPPEAR_MS));
}

function fade(durationMs: number): TransitionEffect {
  return TransitionEffect.OPACITY.animation({ duration: durationMs, curve: MOTION_CURVE });
}

export function fadeTransition(): TransitionEffect {
  return TransitionEffect.asymmetric(fade(MOTION_APPEAR_MS), fade(MOTION_DISAPPEAR_MS));
}


