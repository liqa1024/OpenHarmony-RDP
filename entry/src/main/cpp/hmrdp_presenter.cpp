/*
 * HmRdp - presenter backend selection (see hmrdp_presenter.h).
 *
 * The choice is made from the *presenter* verdict of the cached Vulkan probe
 * (VulkanCapabilities::presenterSupported), which is intentionally looser than
 * the engine one: presenting a frame needs a device that can blit to the
 * XComponent surface, not the compute pipeline RemoteFX decoding requires. That
 * way a device whose Vulkan cannot run the engine still presents through Vulkan,
 * and only a device that cannot present at all falls back to GLES.
 */
#include "hmrdp_presenter.h"

#include "hmrdp_gles_presenter.h"
#include "hmrdp_log.h"
#include "hmrdp_vk_context.h"
#include "hmrdp_vk_renderer.h"

namespace hmrdp {

std::unique_ptr<FramePresenter> CreateFramePresenter() {
  const VulkanCapabilities& caps = GetVulkanCapabilities();
  if (caps.presenterSupported) {
    HMRDP_LOGI("presenter: vulkan (%{public}s)", caps.Describe().c_str());
    return std::make_unique<VkRenderer>();
  }
  HMRDP_LOGW("presenter: gles fallback (vulkan unusable: %{public}s)",
             caps.presenterUnsupportedCode.c_str());
  return std::make_unique<GlesPresenter>();
}

}  // namespace hmrdp
