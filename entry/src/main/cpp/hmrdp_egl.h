/*
 * HmRdp - process-wide EGL display + share anchor.
 *
 * The GPU desktop engine (offscreen pbuffer context) and the Renderer (window
 * context) live in the same EGL share group so GL objects (textures, buffers)
 * are visible across them. This is what lets the Renderer sample the engine's
 * composed screen texture directly instead of a CPU readback + upload
 * (doc_agent/gfx-engine.md §1).
 *
 * The display is initialised once and never terminated; the anchor context is
 * created lazily and outlives every client context that shares it.
 */
#ifndef HMRDP_EGL_H
#define HMRDP_EGL_H

#include <EGL/egl.h>

namespace hmrdp {

// Process-wide EGL display (EGL_NO_DISPLAY when EGL is unavailable).
EGLDisplay SharedEglDisplay();

// Lazily created ES3 context on SharedEglDisplay with no surface of its own. It
// is only ever used as the `share_context` argument for every other context
// (Renderer, GPU engine, capability probe) so they all join one share group.
// EGL_NO_CONTEXT when EGL is unavailable.
EGLContext SharedEglAnchorContext();

}  // namespace hmrdp

#endif  // HMRDP_EGL_H
