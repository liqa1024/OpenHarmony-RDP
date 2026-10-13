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
  // Blocked part of the presents since the last call: eglSwapBuffers can block on the compositor/vsync.
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
  // A (re)created texture starts empty, so the next frame must cover the whole desktop even
  // when the dirty rectangle is only partial.
  bool forceFullUpload_ = true;
  uint64_t presentWaitUs_ = 0;
};

}

#endif
