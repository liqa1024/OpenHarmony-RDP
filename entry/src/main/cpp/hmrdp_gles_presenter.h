/*
 * HmRdp - GLES/EGL fallback presenter for CPU (gdi) frames.
 *
 * Restored from the EGL/GLES presenter that served this path before the Vulkan
 * work, but trimmed to the one job it still has: put a BGRA desktop frame on the
 * XComponent surface, with the letterbox done by the GPU. It exists for devices
 * whose Vulkan cannot present (in practice the emulator), so it is deliberately
 * small and has no engine, no share group and no present-on-change logic.
 *
 * Per frame: upload only the dirty rectangle into a desktop-sized RGBA texture
 * (ES3 + GL_UNPACK_ROW_LENGTH, because the source rows are pitched at the full
 * desktop stride), then clear the surface and draw one letterboxed quad (the
 * BGRA->RGBA swizzle is in the fragment shader) and eglSwapBuffers. The frame is
 * therefore handed over once, to the driver; the CPU never writes display memory.
 */
#ifndef HMRDP_GLES_PRESENTER_H
#define HMRDP_GLES_PRESENTER_H

#include <mutex>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "hmrdp_presenter.h"

namespace hmrdp {

class GlesPresenter : public FramePresenter {
 public:
  GlesPresenter() = default;
  ~GlesPresenter() override;

  GlesPresenter(const GlesPresenter&) = delete;
  GlesPresenter& operator=(const GlesPresenter&) = delete;

  void SetSurface(void* nativeWindow, int width, int height) override;
  void ResizeSurface(int width, int height) override;
  void DestroySurface() override;
  bool Prepare() override;
  bool PresentBgra(const uint8_t* data, int srcStride, int desktopWidth, int desktopHeight,
                   const PresentRect* rects, int rectCount) override;
  void Reset() override;
  // Blocked part of the presents since the last call: the time eglSwapBuffers
  // spent waiting for the display (it can block on the compositor/vsync). See
  // hmrdp_presenter.h.
  uint64_t TakePresentWaitUs() override;

 private:
  bool EnsureContext();
  void DestroyContext();
  void EnsureTexture(int desktopWidth, int desktopHeight);
  void UpdateViewport();
  void DrawQuad(GLuint texture);

  std::mutex mutex_;
  void* pendingWindow_ = nullptr;
  int surfaceWidth_ = 0;
  int surfaceHeight_ = 0;
  bool surfaceDirty_ = true;

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLSurface surface_ = EGL_NO_SURFACE;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLConfig config_ = nullptr;

  GLuint program_ = 0;
  GLuint texture_ = 0;
  GLint attrPos_ = -1;
  GLint attrTex_ = -1;
  GLint uniTex_ = -1;

  int desktopWidth_ = 0;
  int desktopHeight_ = 0;
  int textureWidth_ = 0;
  int textureHeight_ = 0;
  // A (re)created texture starts empty: the next frame must cover the whole
  // desktop even when the server's dirty rectangle only covers part of it.
  bool forceFullUpload_ = true;
  // Blocked part (eglSwapBuffers) of the presents since the last
  // TakePresentWaitUs. Guarded by mutex_ like the rest of the surface state.
  uint64_t presentWaitUs_ = 0;
};

}  // namespace hmrdp

#endif  // HMRDP_GLES_PRESENTER_H
