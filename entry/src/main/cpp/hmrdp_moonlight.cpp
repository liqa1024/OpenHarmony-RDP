/*
 * HmRdp - HarmonyOS RDP client
 * Moonlight (GameStream) client wrapper - see hmrdp_moonlight.h.
 */
#include "hmrdp_moonlight.h"

#include "hmrdp_log.h"

#ifdef HMRDP_HAVE_MOONLIGHT
#include <Limelight.h>
#endif

namespace hmrdp {

std::string MoonlightProbe() {
#ifdef HMRDP_HAVE_MOONLIGHT
  // Touch a real entry point (a pure field initializer: no sockets, threads or
  // global state). It is what proves the static archives actually resolved - an
  // unresolved moonlight/enet/OpenSSL symbol fails the module link, not this call.
  STREAM_CONFIGURATION config = {};
  LiInitializeStreamConfiguration(&config);
  HMRDP_LOGI("moonlight probe: client linked (config %dx%d)", config.width, config.height);
  return "1";
#else
  HMRDP_LOGI("moonlight probe: not built into this module");
  return "0|no-moonlight";
#endif
}

}  // namespace hmrdp
