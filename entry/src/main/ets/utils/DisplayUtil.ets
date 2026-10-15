import { display } from '@kit.ArkUI';

export function defaultDisplay(): display.Display | undefined {
  try {
    return display.getDefaultDisplaySync();
  } catch (error) {
    return undefined;
  }
}

// Highest refresh rate the panel can run at, 0 when the platform does not report the list. A stream
// asking for more than this cannot be shown, whatever the decoder manages.
export function maxDisplayRefreshRate(): number {
  try {
    const rates: number[] | undefined = defaultDisplay()?.supportedRefreshRates;
    if (rates === undefined) {
      return 0;
    }
    let max: number = 0;
    for (let i = 0; i < rates.length; i++) {
      if (rates[i] > max) {
        max = rates[i];
      }
    }
    return max;
  } catch (error) {
    return 0;
  }
}
