/*
 * HmRdp - presenter backend selection (see hmrdp_presenter.h).
 *
 * The choice is made from the *presenter* verdict of the cached Vulkan probe
 * (VulkanCapabilities::presenterSupported): presenting a frame needs a device
 * that can blit to the XComponent surface plus host-visible memory for the frame
 * buffer. A device that cannot present at all falls back to GLES.
 */
#include "hmrdp_presenter.h"

#include <atomic>

#include "hmrdp_gles_presenter.h"
#include "hmrdp_log.h"
#include "hmrdp_vk_context.h"
#include "hmrdp_vk_renderer.h"

namespace hmrdp {

namespace {
// The "硬件加速" setting (see the header). Read when a presenter is created.
std::atomic<bool> g_hardwareAccel{true};
}  // namespace

void SetHardwareAccelEnabled(bool enabled) {
  g_hardwareAccel.store(enabled);
  HMRDP_LOGI("presenter: hardware acceleration (vulkan) %{public}s", enabled ? "on" : "off");
}

bool HardwareAccelEnabled() {
  return g_hardwareAccel.load();
}

std::unique_ptr<FramePresenter> CreateFramePresenter() {
  const VulkanCapabilities& caps = GetVulkanCapabilities();
  if (!HardwareAccelEnabled()) {
    HMRDP_LOGW("presenter: gles (hardware acceleration is off)");
    return std::make_unique<GlesPresenter>();
  }
  if (caps.presenterSupported) {
    HMRDP_LOGI("presenter: vulkan (%{public}s)", caps.Describe().c_str());
    return std::make_unique<VkRenderer>();
  }
  HMRDP_LOGW("presenter: gles fallback (vulkan unusable: %{public}s)",
             caps.presenterUnsupportedCode.c_str());
  return std::make_unique<GlesPresenter>();
}

}  // namespace hmrdp
