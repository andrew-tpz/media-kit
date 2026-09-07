#ifndef MEDIA_KIT_SCOPED_EGL_CONTEXT_H_
#define MEDIA_KIT_SCOPED_EGL_CONTEXT_H_

#include <epoxy/egl.h>
#include <epoxy/glx.h>
#include <glib.h>

void video_output_lock_egl();
void video_output_unlock_egl();

// A current GLX context must be released before libglvnd can bind EGL on X11.
// Capture before eglBindAPI, and restore both the native context and client API.
class ScopedEglContext {
 public:
  explicit ScopedEglContext(bool x11 = false) {
    video_output_lock_egl();
    api_ = eglQueryAPI();
    display_ = eglGetCurrentDisplay();
    context_ = eglGetCurrentContext();
    draw_ = eglGetCurrentSurface(EGL_DRAW);
    read_ = eglGetCurrentSurface(EGL_READ);
    if (x11 && context_ == EGL_NO_CONTEXT) {
      glx_context_ = glXGetCurrentContext();
      if (glx_context_ != nullptr) {
        glx_display_ = glXGetCurrentDisplay();
        glx_draw_ = glXGetCurrentDrawable();
        glx_read_ = glXGetCurrentReadDrawable();
      }
    }
  }

  ~ScopedEglContext() {
    if (!Restore()) {
      g_printerr("media_kit: failed to restore native GL context\n");
    }
    video_output_unlock_egl();
  }

  bool MakeCurrent(EGLDisplay display, EGLSurface surface,
                   EGLContext context) {
    if (glx_context_ != nullptr && !glx_detached_) {
      if (!glXMakeContextCurrent(glx_display_, None, None, nullptr)) {
        return false;
      }
      glx_detached_ = true;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      return false;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
      return false;
    }
    active_display_ = display;
    return true;
  }

  const char* HostContextType() const {
    return glx_context_ != nullptr ? "glx" :
           context_ != EGL_NO_CONTEXT ? "egl" : "none";
  }

  bool Restore() {
    if (active_display_ != EGL_NO_DISPLAY) {
      if (context_ != EGL_NO_CONTEXT) {
        if (!eglMakeCurrent(display_, draw_, read_, context_)) {
          return false;
        }
      } else if (!eglMakeCurrent(active_display_, EGL_NO_SURFACE,
                                 EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        return false;
      }
      active_display_ = EGL_NO_DISPLAY;
    }
    if (!eglBindAPI(api_)) {
      return false;
    }
    if (glx_detached_) {
      if (!glXMakeContextCurrent(glx_display_, glx_draw_, glx_read_,
                                glx_context_)) {
        return false;
      }
      glx_detached_ = false;
    }
    return true;
  }

  ScopedEglContext(const ScopedEglContext&) = delete;
  ScopedEglContext& operator=(const ScopedEglContext&) = delete;

 private:
  EGLenum api_;
  EGLDisplay display_;
  EGLContext context_;
  EGLSurface draw_;
  EGLSurface read_;
  EGLDisplay active_display_ = EGL_NO_DISPLAY;
  Display* glx_display_ = nullptr;
  GLXContext glx_context_ = nullptr;
  GLXDrawable glx_draw_ = None;
  GLXDrawable glx_read_ = None;
  bool glx_detached_ = false;
};

#endif  // MEDIA_KIT_SCOPED_EGL_CONTEXT_H_
