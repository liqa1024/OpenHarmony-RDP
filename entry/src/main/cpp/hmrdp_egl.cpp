/*
 * HmRdp - process-wide EGL display + share anchor (see hmrdp_egl.h).
 */
#include "hmrdp_egl.h"

#include <mutex>

#include "hmrdp_log.h"

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#endif

namespace hmrdp {

EGLDisplay SharedEglDisplay() {
  static std::once_flag once;
  static EGLDisplay display = EGL_NO_DISPLAY;
  std::call_once(once, []() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
      HMRDP_LOGE("egl: eglGetDisplay failed");
      return;
    }
    if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
      HMRDP_LOGE("egl: eglInitialize failed: 0x%{public}x", eglGetError());
      display = EGL_NO_DISPLAY;
    }
  });
  return display;
}

EGLContext SharedEglAnchorContext() {
  static std::once_flag once;
  static EGLContext anchor = EGL_NO_CONTEXT;
  std::call_once(once, []() {
    EGLDisplay display = SharedEglDisplay();
    if (display == EGL_NO_DISPLAY) {
      return;
    }
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) != EGL_TRUE ||
        numConfigs < 1) {
      HMRDP_LOGE("egl: anchor eglChooseConfig failed: 0x%{public}x", eglGetError());
      return;
    }
    const EGLint ctx31[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                            EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    anchor = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx31);
    if (anchor == EGL_NO_CONTEXT) {
      const EGLint ctx3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
      anchor = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx3);
    }
    if (anchor == EGL_NO_CONTEXT) {
      HMRDP_LOGE("egl: anchor eglCreateContext failed: 0x%{public}x", eglGetError());
    } else {
      HMRDP_LOGI("egl: share anchor context ready");
    }
  });
  return anchor;
}

}  // namespace hmrdp
