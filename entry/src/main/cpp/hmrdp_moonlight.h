/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight (GameStream) client wrapper.
 *
 * M1 of MOONLIGHT_PLAN.md lays the native foundation only: the
 * moonlight-common-c static archives (plus its bundled enet and nanors) are
 * linked into this module and the probe below proves the link resolves. Pairing,
 * the host app list and the stream session are built on top of it in later
 * milestones.
 */
#ifndef HMRDP_MOONLIGHT_H
#define HMRDP_MOONLIGHT_H

#include <string>

namespace hmrdp {

// "1" when this build has the Moonlight client linked, else "0|<code>" with a
// stable code (the UI layer owns the wording), mirroring the capability probes
// in hmrdp_vk_context. The code is "no-moonlight" when the archives were not
// built into this module.
std::string MoonlightProbe();

}  // namespace hmrdp

#endif  // HMRDP_MOONLIGHT_H
