/*
 * HmRdp - Full RDPGFX command-stream capture (PERF-TODO §2.5 "B0").
 *
 * The progressive-only dump (hmrdp_rfx.bin) records just the CAPROGRESSIVE
 * surface commands, which is enough to develop the tile decoder but not enough
 * to own the desktop: the GPU path (B2/B3) must also replay SolidFill,
 * SurfaceToSurface, cache and bitmap commands in order, on top of the same
 * FreeRDP surface baselines.
 *
 * This module serializes the complete client-side GFX command stream
 * (hmrdp_gfx.bin) together with per-frame BGRA surface baselines
 * (hmrdp_gfx_surface.bin), and keeps a command-type histogram so a real session
 * shows which commands must actually be implemented. It takes plain scalar /
 * blob arguments and has no FreeRDP dependency, so it can be fed from the
 * RdpgfxClientContext wrappers in hmrdp_session.cpp and read back by an offline
 * host tool.
 *
 * Record layout (little-endian):
 *
 *   hmrdp_gfx.bin
 *     u32 magic      = 'GFX1'
 *     u32 index      (1-based, matches the surface baseline below)
 *     u32 cmdId      (RDPGFX_CMDID_*)
 *     u32 surfaceId  (command target, or 0xFFFFFFFF when not applicable)
 *     u32 scalars[4] (command-specific; see the wrappers in hmrdp_session.cpp)
 *     u32 paramsLen  (bytes of command-specific parameter blob)
 *     u32 payloadLen (bytes of compressed/bitmap payload)
 *     <params> <payload>
 *
 *   hmrdp_gfx_surface.bin
 *     u32 magic      = 'GFS1'
 *     u32 index      (record index the baseline belongs to)
 *     u32 surfaceId
 *     u32 width, height, stride, format
 *     u32 reserved
 *     <BGRA, stride*height bytes>
 *
 * The command-specific meaning of scalars/params/payload is defined next to the
 * wrapper that emits it; the offline replay tool must mirror the same layout.
 */
#ifndef HMRDP_GFX_DUMP_H
#define HMRDP_GFX_DUMP_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace hmrdp {

// Enables/disables the dump and points it at `dir` (the app filesDir). Passing
// an empty dir or `enabled=false` closes the files and logs the final command
// histogram. Safe to call from the UI thread while the RDP thread is recording.
void GfxDumpConfigure(bool enabled, const std::string& dir);

// Flushes, closes and logs the histogram. Called when the capture is turned off.
void GfxDumpShutdown();

// Flushes the pending records to disk without closing (called at session
// disconnect so a crash or abrupt exit cannot lose the tail of the capture).
void GfxDumpFlush();

// Records one GFX command. `scalars` are four command-specific u32 values (the
// wrappers use at most four here and put anything larger into `params`).
// `params`/`payload` may be null when their length is 0. Returns the 1-based
// record index (0 when the dump is disabled or the size cap was reached).
uint32_t GfxDumpCommand(uint16_t cmdId, uint32_t surfaceId, const uint32_t scalars[4],
                        const void* params, uint32_t paramsLen, const void* payload,
                        uint32_t payloadLen);

// Records the BGRA surface `surfaceId` as of `recordIndex`. `data` must be
// `stride * height` bytes; a null/empty surface is ignored.
void GfxDumpSurface(uint32_t recordIndex, uint32_t surfaceId, uint32_t width, uint32_t height,
                    uint32_t stride, uint32_t format, const void* data);

// Human-readable "cmd=count" histogram of the current capture.
std::string GfxDumpHistogramSummary();

}  // namespace hmrdp

#endif  // HMRDP_GFX_DUMP_H
