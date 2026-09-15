/*
 * HmRdp - HarmonyOS RDP client
 * EGL/GLES2 renderer implementation.
 */
#include "hmrdp_renderer.h"

#include "hmrdp_egl.h"
#include "hmrdp_log.h"

namespace hmrdp {
namespace {

const char* kVertexShader =
    "attribute vec4 aPos;\n"
    "attribute vec2 aTex;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "  gl_Position = aPos;\n"
    "  vTex = aTex;\n"
    "}\n";

const char* kFragmentShader =
    "precision mediump float;\n"
    "varying vec2 vTex;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  vec4 c = texture2D(uTex, vTex);\n"
    "  gl_FragColor = vec4(c.bgr, 1.0);\n"
    "}\n";

GLuint CompileShader(GLenum type, const char* source) {
  GLuint shader = glCreateShader(type);
  if (shader == 0) {
    return 0;
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[512] = {0};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    HMRDP_LOGE("shader compile failed: %{public}s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

}  // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() {
  Reset();
}

void Renderer::SetSurface(void* nativeWindow, int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingWindow_ = nativeWindow;
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  surfaceDirty_ = true;
}

void Renderer::ResizeSurface(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
    eglMakeCurrent(display_, surface_, surface_, context_);
    UpdateViewport();
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
}

void Renderer::DestroySurface() {
  std::lock_guard<std::mutex> lock(mutex_);
  DestroyContext();
  pendingWindow_ = nullptr;
  surfaceDirty_ = true;
}

void Renderer::SetDesktopSize(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  desktopWidth_ = width;
  desktopHeight_ = height;
}

void Renderer::Prepare() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureContext()) {
    return;
  }
  if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    return;
  }
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(display_, surface_);
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  HMRDP_LOGI("EGL prepared on %{public}dx%{public}d", surfaceWidth_, surfaceHeight_);
}

void Renderer::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  DestroyContext();
  pendingWindow_ = nullptr;
  desktopWidth_ = 0;
  desktopHeight_ = 0;
}

bool Renderer::EnsureContext() {
  if (pendingWindow_ == nullptr) {
    return false;
  }
  if (!surfaceDirty_ && display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
    return true;
  }

  DestroyContext();

  display_ = SharedEglDisplay();
  if (display_ == EGL_NO_DISPLAY) {
    return false;
  }

  const EGLint configAttribs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE};
  EGLint numConfigs = 0;
  if (eglChooseConfig(display_, configAttribs, &config_, 1, &numConfigs) != EGL_TRUE ||
      numConfigs < 1) {
    HMRDP_LOGE("eglChooseConfig failed: 0x%{public}x", eglGetError());
    DestroyContext();
    return false;
  }

  surface_ = eglCreateWindowSurface(display_, config_,
                                    reinterpret_cast<EGLNativeWindowType>(pendingWindow_),
                                    nullptr);
  if (surface_ == EGL_NO_SURFACE) {
    HMRDP_LOGE("eglCreateWindowSurface failed: 0x%{public}x", eglGetError());
    DestroyContext();
    return false;
  }

  // ES3 is required for GL_UNPACK_ROW_LENGTH, used to upload a sub-rectangle
  // whose source rows are pitched at the full desktop stride. The GLSL ES 1.00
  // shaders below remain valid in an ES3 context.
  //
  // The context joins the process-wide share group (SharedEglAnchorContext) so
  // PresentTexture can sample a texture created by the GPU desktop engine
  // without a CPU round-trip (doc_agent/gfx-engine.md §1).
  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context_ =
      eglCreateContext(display_, config_, SharedEglAnchorContext(), contextAttribs);
  if (context_ == EGL_NO_CONTEXT) {
    HMRDP_LOGE("eglCreateContext failed: 0x%{public}x", eglGetError());
    DestroyContext();
    return false;
  }

  if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    HMRDP_LOGE("eglMakeCurrent failed: 0x%{public}x", eglGetError());
    DestroyContext();
    return false;
  }

  GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (vs == 0 || fs == 0) {
    DestroyContext();
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = GL_FALSE;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    char log[512] = {0};
    glGetProgramInfoLog(program_, sizeof(log) - 1, nullptr, log);
    HMRDP_LOGE("program link failed: %{public}s", log);
    DestroyContext();
    return false;
  }

  attrPos_ = glGetAttribLocation(program_, "aPos");
  attrTex_ = glGetAttribLocation(program_, "aTex");
  uniTex_ = glGetUniformLocation(program_, "uTex");

  texture_ = 0;
  textureWidth_ = 0;
  textureHeight_ = 0;
  forceFullUpload_ = true;

  surfaceDirty_ = false;
  HMRDP_LOGI("EGL surface ready %{public}dx%{public}d", surfaceWidth_, surfaceHeight_);
  return true;
}

void Renderer::DestroyContext() {
  if (display_ == EGL_NO_DISPLAY) {
    return;
  }
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (texture_ != 0) {
    glDeleteTextures(1, &texture_);
    texture_ = 0;
  }
  if (program_ != 0) {
    glDeleteProgram(program_);
    program_ = 0;
  }
  if (context_ != EGL_NO_CONTEXT) {
    eglDestroyContext(display_, context_);
    context_ = EGL_NO_CONTEXT;
  }
  if (surface_ != EGL_NO_SURFACE) {
    eglDestroySurface(display_, surface_);
    surface_ = EGL_NO_SURFACE;
  }
  // The display is shared across all sessions; never terminate it here.
  display_ = EGL_NO_DISPLAY;
  textureWidth_ = 0;
  textureHeight_ = 0;
  forceFullUpload_ = true;
}

void Renderer::EnsureTexture() {
  if (desktopWidth_ <= 0 || desktopHeight_ <= 0) {
    return;
  }
  if (texture_ != 0 && textureWidth_ == desktopWidth_ && textureHeight_ == desktopHeight_) {
    return;
  }
  if (texture_ != 0) {
    glDeleteTextures(1, &texture_);
    texture_ = 0;
  }
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, desktopWidth_, desktopHeight_, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  textureWidth_ = desktopWidth_;
  textureHeight_ = desktopHeight_;
  // A new texture starts empty; the next frame must cover the whole desktop.
  forceFullUpload_ = true;
  HMRDP_LOGI("texture %{public}dx%{public}d", textureWidth_, textureHeight_);
}

void Renderer::UpdateViewport() {
  if (surfaceWidth_ <= 0 || surfaceHeight_ <= 0 || desktopWidth_ <= 0 ||
      desktopHeight_ <= 0) {
    glViewport(0, 0, surfaceWidth_, surfaceHeight_);
    return;
  }
  const double surfaceAspect =
      static_cast<double>(surfaceWidth_) / static_cast<double>(surfaceHeight_);
  const double desktopAspect =
      static_cast<double>(desktopWidth_) / static_cast<double>(desktopHeight_);
  int vpWidth = surfaceWidth_;
  int vpHeight = surfaceHeight_;
  if (desktopAspect > surfaceAspect) {
    vpHeight = static_cast<int>(surfaceWidth_ / desktopAspect);
  } else {
    vpWidth = static_cast<int>(surfaceHeight_ * desktopAspect);
  }
  const int vpX = (surfaceWidth_ - vpWidth) / 2;
  const int vpY = (surfaceHeight_ - vpHeight) / 2;
  glViewport(vpX, vpY, vpWidth, vpHeight);
}

void Renderer::DrawQuad(GLuint texture) {
  static const GLfloat positions[] = {
      -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
  };
  static const GLfloat texcoords[] = {
      0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
  };
  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glUniform1i(uniTex_, 0);
  glEnableVertexAttribArray(attrPos_);
  glEnableVertexAttribArray(attrTex_);
  glVertexAttribPointer(attrPos_, 2, GL_FLOAT, GL_FALSE, 0, positions);
  glVertexAttribPointer(attrTex_, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(attrPos_);
  glDisableVertexAttribArray(attrTex_);
}

bool Renderer::DrawFrame(const uint8_t* data, int stride, int x, int y, int width,
                         int height) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureContext()) {
    return false;
  }
  if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    return false;
  }
  if (desktopWidth_ <= 0 || desktopHeight_ <= 0) {
    return false;
  }
  EnsureTexture();
  if (texture_ == 0) {
    return false;
  }
  // Row pitch of the source framebuffer; usually desktopWidth*4, but honour the
  // server's stride in case it pads rows.
  const int pitch = stride > 0 ? stride : desktopWidth_ * 4;

  int clampedX = x < 0 ? 0 : x;
  int clampedY = y < 0 ? 0 : y;
  const int maxW = desktopWidth_ - clampedX;
  const int maxH = desktopHeight_ - clampedY;
  int uploadW = width > maxW ? maxW : width;
  int uploadH = height > maxH ? maxH : height;
  if (forceFullUpload_) {
    // A fresh / recreated texture must be fully repopulated even when the
    // server's first dirty rectangle only covers part of the desktop; otherwise
    // the rest of the texture would stay blank.
    clampedX = 0;
    clampedY = 0;
    uploadW = desktopWidth_;
    uploadH = desktopHeight_;
  } else if (uploadW <= 0 || uploadH <= 0) {
    return false;
  }
  forceFullUpload_ = false;

  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  // The source is a sub-rectangle of a buffer whose rows are pitched at the full
  // desktop stride; GL_UNPACK_ROW_LENGTH makes glTexSubImage2D honour that pitch
  // instead of assuming uploadW. Without it rows skew and the image tears.
  glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  const uint8_t* src = data + static_cast<size_t>(clampedY) * pitch +
                       static_cast<size_t>(clampedX) * 4;
  glTexSubImage2D(GL_TEXTURE_2D, 0, clampedX, clampedY, uploadW, uploadH, GL_RGBA,
                  GL_UNSIGNED_BYTE, src);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  glDisable(GL_BLEND);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  UpdateViewport();
  DrawQuad(texture_);
  static int drawCount = 0;
  if (drawCount < 12) {
    HMRDP_LOGI("draw #%{public}d rect %{public}d,%{public}d %{public}dx%{public}d tex=%{public}dx%{public}d desk=%{public}dx%{public}d surf=%{public}dx%{public}d glerr=0x%{public}x",
               drawCount, clampedX, clampedY, uploadW, uploadH, textureWidth_,
               textureHeight_, desktopWidth_, desktopHeight_, surfaceWidth_,
               surfaceHeight_, glGetError());
    drawCount++;
  }
  eglSwapBuffers(display_, surface_);
  // Release the context (see PresentTexture): frames can arrive on different
  // threads, and a context left current elsewhere cannot be re-made current.
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return true;
}

bool Renderer::PresentTexture(GLuint texture, int width, int height) {
  if (texture == 0 || width <= 0 || height <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureContext()) {
    return false;
  }
  if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    return false;
  }
  // The texture is created and populated by the GPU desktop engine in the same
  // EGL share group, so it can be sampled directly here; only the viewport math
  // needs the desktop dimensions.
  desktopWidth_ = width;
  desktopHeight_ = height;
  glDisable(GL_BLEND);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  UpdateViewport();
  DrawQuad(texture);
  eglSwapBuffers(display_, surface_);
  // Release the context: frames may be presented from different threads, and a
  // context that is left current on another thread cannot be re-made current.
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return true;
}

}  // namespace hmrdp
