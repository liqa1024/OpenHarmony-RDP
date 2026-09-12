/*
 * HmRdp - shared GFX engine command + present pipeline (PERF-TODO §4).
 *
 * The live session and the debug replay harness go through this same code, so a
 * fault found while replaying a recording is a fault in the integrated path.
 *
 * These are templates over the engine type on purpose: production passes the GPU
 * engine (GfxGpuDesktop), while the debug replay may pass either the GPU engine
 * or the CPU reference (GfxDesktop). There is NO production CPU/GPU switch - the
 * production soft/hard choice is FreeRDP's own "hardware decode" setting.
 */
#ifndef HMRDP_GFX_PIPELINE_H
#define HMRDP_GFX_PIPELINE_H

#include <cstdint>
#include <vector>

#include "hmrdp_renderer.h"

namespace hmrdp {

// Command ids as delivered to the engine (mirrors hmrdp_gfx_dump.h and
// hmrdp_rfx_gpu.cpp). A capture stream carries raw ids; this enum documents the
// ones the engine implements.
enum GpuCmd : uint16_t {
  kGpuCmdWireToSurface = 0x0001,
  kGpuCmdSolidFill = 0x0004,
  kGpuCmdSurfaceToSurface = 0x0005,
  kGpuCmdSurfaceToCache = 0x0006,
  kGpuCmdCacheToSurface = 0x0007,
  kGpuCmdEvictCacheEntry = 0x0008,
  kGpuCmdCreateSurface = 0x0009,
  kGpuCmdDeleteSurface = 0x000A,
  kGpuCmdStartFrame = 0x000B,
  kGpuCmdEndFrame = 0x000C,
  kGpuCmdResetGraphics = 0x000E,
  kGpuCmdMapSurfaceToOutput = 0x000F,
  kGpuCmdMapSurfaceToScaledOutput = 0x0017,
};

// Applies one command to the engine (`engine` may be null for a no-op).
template <typename Engine>
void GpuApplyCommand(Engine* engine, uint16_t cmdId, uint32_t surfaceId,
                     const uint32_t scalars[4], const uint8_t* params, uint32_t paramsLen,
                     const uint8_t* payload, uint32_t payloadLen) {
  if (engine == nullptr) {
    return;
  }
  engine->ApplyCommand(cmdId, surfaceId, scalars, params, paramsLen, payload, payloadLen);
}

// Composes the engine screen and presents it, then clears the screen dirty gate.
// A shared GPU texture is sampled directly when the backend has one; otherwise
// (CPU reference) the composed screen is read back and uploaded. Returns true
// when a frame reached the screen.
template <typename Engine>
bool GpuPresentComposed(Engine* engine, Renderer* renderer) {
  if (engine == nullptr || renderer == nullptr) {
    return false;
  }
  if (!engine->Compose()) {
    return false;  // static frame: nothing dirty, no present (FPS stays 0)
  }
  bool presented = false;
  if (engine->screenTexture() == 0u) {
    const int sw = engine->screenWidth();
    const int sh = engine->screenHeight();
    std::vector<uint8_t> screen;
    if (sw > 0 && sh > 0 && engine->ReadScreen(&screen) &&
        screen.size() == static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4) {
      renderer->SetDesktopSize(sw, sh);
      presented = renderer->DrawFrame(screen.data(), sw * 4, 0, 0, sw, sh);
    }
  } else {
    presented = renderer->PresentTexture(engine->screenTexture(), engine->screenWidth(),
                                         engine->screenHeight());
  }
  if (presented) {
    engine->ClearScreenDirty();
  }
  return presented;
}

}  // namespace hmrdp

#endif  // HMRDP_GFX_PIPELINE_H
