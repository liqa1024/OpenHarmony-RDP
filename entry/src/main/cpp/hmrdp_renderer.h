/*
 * HmRdp - HarmonyOS RDP client
 * EGL/GLES2 renderer for the FreeRDP GDI framebuffer.
 *
 * The XComponent native window is driven through EGL. Remote frames arrive as
 * BGRA32 (top-down); they are uploaded into a texture and drawn as a quad with
 * aspect-ratio preserved (letterboxed) and a BGRA->RGBA swizzle in the shader.
 */
#ifndef HMRDP_RENDERER_H
#define HMRDP_RENDERER_H

#include <cstdint>
#include <mutex>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

namespace hmrdp {

class Renderer {
public:
  Renderer();
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // Called from the ArkUI (main) thread when the XComponent surface changes.
  void SetSurface(void* nativeWindow, int width, int height);
  void ResizeSurface(int width, int height);
  void DestroySurface();

  // Eagerly initialize EGL/GLES so the surface shows black before any frame.
  void Prepare();

  // Called from the FreeRDP worker thread.
  void SetDesktopSize(int width, int height);
  void DrawFrame(const uint8_t* data, int stride, int x, int y, int width, int height);
  void Reset();

private:
  bool EnsureContext();
  void DestroyContext();
  void EnsureTexture();
  void UpdateViewport();
  void DrawQuad();

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
};

}  // namespace hmrdp

#endif  // HMRDP_RENDERER_H
