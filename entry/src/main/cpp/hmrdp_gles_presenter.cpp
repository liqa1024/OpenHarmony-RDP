/*
 * HmRdp - GLES/EGL fallback presenter (see hmrdp_gles_presenter.h).
 */
#include "hmrdp_gles_presenter.h"

#include <mutex>

#include "hmrdp_log.h"

namespace hmrdp {
namespace {

// The EGL display is process-wide and initialised once; it is never terminated
// (the same rule the old EGL helper followed).
EGLDisplay SharedDisplay() {
  static std::once_flag once;
  static EGLDisplay display = EGL_NO_DISPLAY;
  std::call_once(once, []() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
      HMRDP_LOGE("gles present: eglGetDisplay failed");
      return;
    }
    if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
      HMRDP_LOGE("gles present: eglInitialize failed: 0x%{public}x", eglGetError());
      display = EGL_NO_DISPLAY;
    }
  });
  return display;
}

const char* kVertexShader =
    "attribute vec4 aPos;\n"
    "attribute vec2 aTex;\n"
    "varying vec2 vTex;\n"
    "void main() {\n"
    "  gl_Position = aPos;\n"
    "  vTex = aTex;\n"
    "}\n";

// The screen surface is RGBA-ordered while FreeRDP hands us BGRA, so the swizzle
// lives here rather than in a CPU pass over the frame.
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
    HMRDP_LOGE("gles present: shader compile failed: %{public}s", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

}  // namespace

GlesPresenter::~GlesPresenter() {
  Reset();
}

void GlesPresenter::SetSurface(void* nativeWindow, int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  pendingWindow_ = nativeWindow;
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  surfaceDirty_ = true;
}

void GlesPresenter::ResizeSurface(int width, int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  surfaceWidth_ = width;
  surfaceHeight_ = height;
  if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
    eglMakeCurrent(display_, surface_, surface_, context_);
    UpdateViewport();
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
}

void GlesPresenter::DestroySurface() {
  std::lock_guard<std::mutex> lock(mutex_);
  DestroyContext();
  pendingWindow_ = nullptr;
  surfaceDirty_ = true;
}

bool GlesPresenter::Prepare() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureContext() ||
      eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    return false;
  }
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(display_, surface_);
  HMRDP_LOGI("gles present: prepared %{public}dx%{public}d", surfaceWidth_, surfaceHeight_);
  return true;
}

void GlesPresenter::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  DestroyContext();
  pendingWindow_ = nullptr;
  desktopWidth_ = 0;
  desktopHeight_ = 0;
}

bool GlesPresenter::EnsureContext() {
  if (pendingWindow_ == nullptr) {
    return false;
  }
  if (!surfaceDirty_ && display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
    return true;
  }
  DestroyContext();

  display_ = SharedDisplay();
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
    HMRDP_LOGE("gles present: eglChooseConfig failed: 0x%{public}x", eglGetError());
    DestroyContext();
    return false;
  }

  surface_ = eglCreateWindowSurface(display_, config_,
                                    reinterpret_cast<EGLNativeWindowType>(pendingWindow_),
                                    nullptr);
  if (surface_ == EGL_NO_SURFACE) {
    HMRDP_LOGE("gles present: eglCreateWindowSurface failed: 0x%{public}x", eglGetError());
    DestroyContext();
    return false;
  }

  // ES3 for GL_UNPACK_ROW_LENGTH (dirty-rect upload of a full-stride source). The
  // GLSL ES 1.00 shaders below stay valid in an ES3 context.
  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
  if (context_ == EGL_NO_CONTEXT) {
    HMRDP_LOGE("gles present: eglCreateContext failed: 0x%{public}x", eglGetError());
    DestroyContext();
    return false;
  }
  if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    HMRDP_LOGE("gles present: eglMakeCurrent failed: 0x%{public}x", eglGetError());
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
    HMRDP_LOGE("gles present: program link failed: %{public}s", log);
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
  HMRDP_LOGI("gles present: surface ready %{public}dx%{public}d (renderer=%{public}s)",
             surfaceWidth_, surfaceHeight_,
             reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
  return true;
}

void GlesPresenter::DestroyContext() {
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
  // The display is process-wide; never terminate it here.
  display_ = EGL_NO_DISPLAY;
  textureWidth_ = 0;
  textureHeight_ = 0;
  forceFullUpload_ = true;
}

void GlesPresenter::EnsureTexture(int desktopWidth, int desktopHeight) {
  if (desktopWidth <= 0 || desktopHeight <= 0) {
    return;
  }
  if (texture_ != 0 && textureWidth_ == desktopWidth && textureHeight_ == desktopHeight) {
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, desktopWidth, desktopHeight, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  textureWidth_ = desktopWidth;
  textureHeight_ = desktopHeight;
  forceFullUpload_ = true;
  HMRDP_LOGI("gles present: texture %{public}dx%{public}d", textureWidth_, textureHeight_);
}

void GlesPresenter::UpdateViewport() {
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

void GlesPresenter::DrawQuad(GLuint texture) {
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

bool GlesPresenter::PresentBgra(const uint8_t* data, int srcStride, int desktopWidth,
                               int desktopHeight, int x, int y, int width, int height) {
  if (data == nullptr || width <= 0 || height <= 0 || desktopWidth <= 0 ||
      desktopHeight <= 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!EnsureContext() ||
      eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
    return false;
  }
  desktopWidth_ = desktopWidth;
  desktopHeight_ = desktopHeight;
  EnsureTexture(desktopWidth, desktopHeight);
  if (texture_ == 0) {
    return false;
  }

  // Row pitch of the source frame; usually desktopWidth*4, but honour the
  // server's stride in case it pads rows.
  const int pitch = srcStride > 0 ? srcStride : desktopWidth * 4;
  int clampedX = x < 0 ? 0 : x;
  int clampedY = y < 0 ? 0 : y;
  const int maxW = desktopWidth - clampedX;
  const int maxH = desktopHeight - clampedY;
  int uploadW = width > maxW ? maxW : width;
  int uploadH = height > maxH ? maxH : height;
  if (forceFullUpload_) {
    clampedX = 0;
    clampedY = 0;
    uploadW = desktopWidth;
    uploadH = desktopHeight;
  } else if (uploadW <= 0 || uploadH <= 0) {
    return false;
  }
  forceFullUpload_ = false;

  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  // The source is a sub-rectangle of a buffer whose rows are pitched at the full
  // desktop stride; GL_UNPACK_ROW_LENGTH makes glTexSubImage2D honour that pitch
  // instead of assuming uploadW (without it rows skew and the picture tears).
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
  eglSwapBuffers(display_, surface_);
  return true;
}

}  // namespace hmrdp
