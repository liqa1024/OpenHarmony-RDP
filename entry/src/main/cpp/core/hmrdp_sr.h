#ifndef HMRDP_SR_H
#define HMRDP_SR_H

namespace hmrdp {

// 超分: session resolution for an output size and upscale ratio, rounded to an even pixel count
// (some hosts render odd sizes badly). Both protocols take the same route: the stream/desktop is
// sized to output / ratio and the presenter upscales every frame back to the output size.
// Mirrors SettingsStore.srSessionSize, which the ArkTS session pages use for the picture geometry
// and the input reference size.
int SuperResolutionSessionSize(int output, int ratioPercent);

}

#endif
