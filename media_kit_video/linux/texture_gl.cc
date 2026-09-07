// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/texture_gl.h"
#include "include/media_kit_video/scoped_egl_context.h"

#include <gdk/gdkx.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <cstdarg>

// EGLImage extension function pointers
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImageKHR image);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum target, GLeglImageOES image);

static PFNEGLCREATESYNCKHRPROC media_kit_egl_create_sync_khr = NULL;
static PFNEGLDESTROYSYNCKHRPROC media_kit_egl_destroy_sync_khr = NULL;
static PFNEGLCLIENTWAITSYNCKHRPROC media_kit_egl_client_wait_sync_khr = NULL;
static PFNEGLWAITSYNCKHRPROC media_kit_egl_wait_sync_khr = NULL;

// Define the extension functions
#ifndef eglCreateImageKHR
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
#endif
#ifndef eglDestroyImageKHR
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
#endif
#ifndef glEGLImageTargetTexture2DOES
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
#endif

static void init_egl_image_extensions() {
  static gboolean initialized = FALSE;
  if (!initialized) {
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    initialized = TRUE;
  }
}

static void init_egl_sync_extensions() {
  static gboolean initialized = FALSE;
  if (!initialized) {
    media_kit_egl_create_sync_khr =
        (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    media_kit_egl_destroy_sync_khr =
        (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    media_kit_egl_client_wait_sync_khr =
        (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    media_kit_egl_wait_sync_khr =
        (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    initialized = TRUE;
  }
}

static EGLSyncKHR texture_gl_create_frame_sync_or_finish(EGLDisplay display,
                                                         const gchar** mode) {
  init_egl_sync_extensions();
  if (media_kit_egl_create_sync_khr != NULL &&
      media_kit_egl_destroy_sync_khr != NULL &&
      (media_kit_egl_wait_sync_khr != NULL ||
       media_kit_egl_client_wait_sync_khr != NULL)) {
    EGLSyncKHR sync =
        media_kit_egl_create_sync_khr(display, EGL_SYNC_FENCE_KHR, NULL);
    if (sync != EGL_NO_SYNC_KHR) {
      glFlush();
      *mode = "egl-fence";
      return sync;
    }
  }

  glFinish();
  *mode = "gl-finish";
  return EGL_NO_SYNC_KHR;
}

static gboolean texture_gl_wait_frame_sync(EGLDisplay display,
                                           EGLSyncKHR sync,
                                           EGLint* wait_result,
                                           EGLint* wait_error) {
  if (sync == EGL_NO_SYNC_KHR) {
    if (wait_result != NULL) {
      *wait_result = EGL_CONDITION_SATISFIED_KHR;
    }
    if (wait_error != NULL) {
      *wait_error = EGL_SUCCESS;
    }
    return TRUE;
  }

  EGLint result = EGL_FALSE;
  gboolean waited = FALSE;
  if (media_kit_egl_wait_sync_khr != NULL &&
      eglGetCurrentDisplay() == display) {
    result = media_kit_egl_wait_sync_khr(display, sync, 0);
    waited = result != EGL_FALSE;
  }
  if (!waited && media_kit_egl_client_wait_sync_khr != NULL) {
    result = media_kit_egl_client_wait_sync_khr(
        display, sync, 0, EGL_FOREVER_KHR);
    waited = result == EGL_CONDITION_SATISFIED_KHR;
  }
  if (wait_result != NULL) {
    *wait_result = result;
  }
  if (wait_error != NULL) {
    *wait_error = waited ? EGL_SUCCESS : eglGetError();
  }

  media_kit_egl_destroy_sync_khr(display, sync);
  return waited;
}

gboolean texture_gl_is_supported() {
  init_egl_image_extensions();
  const gboolean supported =
      eglCreateImageKHR != NULL &&
      eglDestroyImageKHR != NULL &&
      glEGLImageTargetTexture2DOES != NULL;
  static gboolean logged = FALSE;
  if (!supported && !logged) {
    g_printerr(
        "media_kit: TextureGL: EGLImage interop missing: "
        "eglCreateImageKHR=%s eglDestroyImageKHR=%s "
        "glEGLImageTargetTexture2DOES=%s\n",
        eglCreateImageKHR == NULL ? "missing" : "ok",
        eglDestroyImageKHR == NULL ? "missing" : "ok",
        glEGLImageTargetTexture2DOES == NULL ? "missing" : "ok");
    logged = TRUE;
  }
  return supported;
}

struct _TextureGL {
  FlTextureGL parent_instance;
  guint32 name;              // Flutter's texture name
  guint32 fbo;               // mpv's FBO
  guint32 mpv_texture;       // mpv's texture
  EGLImageKHR egl_image;     // EGLImage for sharing between contexts
  guint32 current_width;
  guint32 current_height;
  guint32 failure_logs;
  gboolean frame_sync_logged;
  VideoOutput* video_output;
};

G_DEFINE_TYPE(TextureGL, texture_gl, fl_texture_gl_get_type())

static void texture_gl_init(TextureGL* self) {
  self->name = 0;
  self->fbo = 0;
  self->mpv_texture = 0;
  self->egl_image = EGL_NO_IMAGE_KHR;
  self->current_width = 1;
  self->current_height = 1;
  self->failure_logs = 0;
  self->frame_sync_logged = FALSE;
  self->video_output = NULL;
}

static void texture_gl_log_frame_failure(TextureGL* self,
                                         const gchar* stage,
                                         const gchar* reason) {
  if (self == NULL || self->failure_logs >= 3) {
    return;
  }
  VideoOutput* video_output = self->video_output;
  void* player =
      video_output == NULL ? NULL : (void*)video_output_get_handle(video_output);
  g_printerr(
      "media_kit: TextureGL: GPU frame failed player=%p stage=%s "
      "reason=\"%s\"\n",
      player,
      stage,
      reason);
  self->failure_logs++;
  if (self->failure_logs == 3) {
    g_printerr(
        "media_kit: TextureGL: further GPU frame failures suppressed "
        "player=%p\n",
        player);
  }
}

static gboolean texture_gl_fail(TextureGL* self,
                                GError** error,
                                gint code,
                                const gchar* stage,
                                const gchar* format,
                                ...) {
  va_list args;
  va_start(args, format);
  gchar* message = g_strdup_vprintf(format, args);
  va_end(args);

  g_set_error_literal(error,
                      g_quark_from_static_string("media-kit-video"),
                      code,
                      message);
  texture_gl_log_frame_failure(self, stage, message);
  g_free(message);
  return FALSE;
}

static void texture_gl_log_frame_sync(TextureGL* self, const gchar* mode) {
  if (self == NULL || self->frame_sync_logged) {
    return;
  }
  VideoOutput* video_output = self->video_output;
  void* player =
      video_output == NULL ? NULL : (void*)video_output_get_handle(video_output);
  g_print("media_kit: TextureGL: HW frame-sync=%s player=%p\n", mode, player);
  self->frame_sync_logged = TRUE;
}

static void texture_gl_dispose(GObject* object) {
  TextureGL* self = TEXTURE_GL(object);
  VideoOutput* video_output = self->video_output;

  ScopedEglContext egl_scope(
      GDK_IS_X11_DISPLAY(gdk_display_get_default()));

  // Clean up mpv's OpenGL resources (in mpv's isolated context)
  if (video_output != NULL) {
    EGLDisplay egl_display = video_output_get_egl_display(video_output);
    EGLContext egl_context = video_output_get_egl_context(video_output);
    EGLSurface egl_surface = video_output_get_egl_surface(video_output);

    if (egl_display != EGL_NO_DISPLAY &&
        egl_context != EGL_NO_CONTEXT &&
        egl_surface != EGL_NO_SURFACE &&
        egl_scope.MakeCurrent(egl_display, egl_surface, egl_context)) {
      if (self->mpv_texture != 0) {
        glDeleteTextures(1, &self->mpv_texture);
        self->mpv_texture = 0;
      }
      if (self->fbo != 0) {
        glDeleteFramebuffers(1, &self->fbo);
        self->fbo = 0;
      }

      if (self->egl_image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR != NULL) {
        eglDestroyImageKHR(egl_display, self->egl_image);
        self->egl_image = EGL_NO_IMAGE_KHR;
      }
    } else {
      g_printerr(
          "media_kit: TextureGL: skipped GL resource deletion because mpv EGL "
          "context is not current: 0x%x\n",
          eglGetError());
      self->mpv_texture = 0;
      self->fbo = 0;
      self->egl_image = EGL_NO_IMAGE_KHR;
    }
  }

  // |name| belongs to Flutter's render context. It is deleted during resize
  // while that context is current; final cleanup is left to context teardown.
  self->name = 0;
  self->current_width = 1;
  self->current_height = 1;
  self->video_output = NULL;
  G_OBJECT_CLASS(texture_gl_parent_class)->dispose(object);
}

static void texture_gl_class_init(TextureGLClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = texture_gl_populate_texture;
  G_OBJECT_CLASS(klass)->dispose = texture_gl_dispose;
}

TextureGL* texture_gl_new(VideoOutput* video_output) {
  texture_gl_is_supported();
  TextureGL* self = TEXTURE_GL(g_object_new(texture_gl_get_type(), NULL));
  self->video_output = video_output;
  return self;
}

gboolean texture_gl_populate_texture(FlTextureGL* texture,
                                     guint32* target,
                                     guint32* name,
                                     guint32* width,
                                     guint32* height,
                                     GError** error) {
  TextureGL* self = TEXTURE_GL(texture);
  VideoOutput* video_output = self->video_output;

  if (!texture_gl_is_supported()) {
    return texture_gl_fail(self,
                           error,
                           1,
                           "init",
                           "EGLImage interop extensions are unavailable");
  }
  
  gint32 required_width = (guint32)video_output_get_width(video_output);
  gint32 required_height = (guint32)video_output_get_height(video_output);
  
  gboolean notify_texture_update = FALSE;

  if (required_width > 0 && required_height > 0) {
    ScopedEglContext egl_scope;
    EGLDisplay flutter_display = eglGetCurrentDisplay();
    if (flutter_display == EGL_NO_DISPLAY ||
        eglGetCurrentContext() == EGL_NO_CONTEXT) {
      return texture_gl_fail(self, error, 1, "init",
                             "Flutter EGL context is not current in populate");
    }
    EGLDisplay egl_display = video_output_get_egl_display(video_output);
    EGLContext egl_context = video_output_get_egl_context(video_output);
    EGLSurface egl_surface = video_output_get_egl_surface(video_output);

    // Finish Flutter's previous reads before overwriting the shared image.
    // The opposite (mpv -> Flutter) fence is submitted after rendering below.
    const gchar* read_sync_mode = "unknown";
    EGLSyncKHR read_sync =
        texture_gl_create_frame_sync_or_finish(flutter_display, &read_sync_mode);
    if (!egl_scope.MakeCurrent(egl_display, egl_surface, egl_context)) {
      EGLint bind_error = eglGetError();
      if (read_sync != EGL_NO_SYNC_KHR) {
        media_kit_egl_destroy_sync_khr(flutter_display, read_sync);
      }
      return texture_gl_fail(self, error, 6, "render",
                             "failed to bind mpv EGL context: 0x%x", bind_error);
    }
    if (!texture_gl_wait_frame_sync(flutter_display, read_sync, NULL, NULL)) {
      return texture_gl_fail(self, error, 8, "render",
                             "failed to wait for Flutter texture reads");
    }
    gboolean first_frame = self->name == 0 || self->fbo == 0 || self->mpv_texture == 0;
    gboolean resize = self->current_width != required_width ||
                      self->current_height != required_height;
    
    if (first_frame || resize) {
      // Free previous resources in mpv's context
      if (!first_frame) {
        glDeleteTextures(1, &self->mpv_texture);
        glDeleteFramebuffers(1, &self->fbo);
        if (self->egl_image != EGL_NO_IMAGE_KHR) {
          eglDestroyImageKHR(egl_display, self->egl_image);
        }
      }
      
      // Create mpv's FBO and texture
      glGenFramebuffers(1, &self->fbo);
      glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
      
      glGenTextures(1, &self->mpv_texture);
      glBindTexture(GL_TEXTURE_2D, self->mpv_texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, required_width, required_height,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      
      // Attach mpv's texture to FBO
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, self->mpv_texture, 0);

      GLenum framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
        gboolean failed = texture_gl_fail(self,
                                          error,
                                          3,
                                          "resize",
                                          "mpv framebuffer is incomplete: 0x%x",
                                          framebuffer_status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (self->mpv_texture != 0) {
          glDeleteTextures(1, &self->mpv_texture);
          self->mpv_texture = 0;
        }
        if (self->fbo != 0) {
          glDeleteFramebuffers(1, &self->fbo);
          self->fbo = 0;
        }
        return failed;
      }
      
      // Create EGLImage from mpv's texture
      EGLint egl_image_attribs[] = { EGL_NONE };
      self->egl_image = eglCreateImageKHR(
          egl_display,
          egl_context,
          EGL_GL_TEXTURE_2D_KHR,
          (EGLClientBuffer)(guintptr)self->mpv_texture,
          egl_image_attribs);

      if (self->egl_image == EGL_NO_IMAGE_KHR) {
        gboolean failed = texture_gl_fail(
            self,
            error,
            4,
            "resize",
            "eglCreateImageKHR failed for mpv texture: 0x%x",
            eglGetError());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (self->mpv_texture != 0) {
          glDeleteTextures(1, &self->mpv_texture);
          self->mpv_texture = 0;
        }
        if (self->fbo != 0) {
          glDeleteFramebuffers(1, &self->fbo);
          self->fbo = 0;
        }
        return failed;
      }
      
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glBindTexture(GL_TEXTURE_2D, 0);
      
      // Flush to ensure mpv's texture is ready
      glFlush();
      
      // Switch back to Flutter's context to create/update Flutter's texture
      if (!egl_scope.Restore()) {
        return texture_gl_fail(self, error, 7, "resize",
                               "failed to restore Flutter EGL context: 0x%x",
                               eglGetError());
      }
      
      GLint previous_texture = 0;
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
      // Free previous Flutter texture (including the initial dummy texture).
      if (self->name != 0) {
        if (previous_texture == static_cast<GLint>(self->name)) {
          previous_texture = 0;
        }
        glDeleteTextures(1, &self->name);
      }
      
      // Create Flutter's texture from EGLImage
      glGenTextures(1, &self->name);
      glBindTexture(GL_TEXTURE_2D, self->name);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, self->egl_image);
      GLenum image_error = glGetError();
      if (image_error != GL_NO_ERROR) {
        gboolean failed = texture_gl_fail(
            self,
            error,
            5,
            "resize",
            "glEGLImageTargetTexture2DOES failed: 0x%x",
            image_error);
        glBindTexture(GL_TEXTURE_2D, previous_texture);
        return failed;
      }
      glBindTexture(GL_TEXTURE_2D, previous_texture);
      
      self->current_width = required_width;
      self->current_height = required_height;
      notify_texture_update = TRUE;
      
      // Flutter's context is already current, so we're ready to render
    }
    
    mpv_render_context* render_context = video_output_get_render_context(video_output);
    
    // Switch to mpv's isolated context for rendering
    if (!egl_scope.MakeCurrent(egl_display, egl_surface, egl_context)) {
      return texture_gl_fail(
          self,
          error,
          6,
          "render",
          "eglMakeCurrent failed while rendering TextureGL: 0x%x",
          eglGetError());
    }
    
    // Bind mpv's FBO
    glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
    
    // Render mpv frame to mpv's texture
    mpv_opengl_fbo fbo{(gint32)self->fbo, required_width, required_height, 0};
    int flip_y = 0;
    int block_for_target_time = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    mpv_render_context_render(render_context, params);
    
    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    const gchar* frame_sync_mode = "unknown";
    EGLSyncKHR frame_sync =
        texture_gl_create_frame_sync_or_finish(egl_display, &frame_sync_mode);
    
    // Restore Flutter's context
    if (!egl_scope.Restore()) {
      EGLint restore_error = eglGetError();
      if (frame_sync != EGL_NO_SYNC_KHR) {
        media_kit_egl_destroy_sync_khr(egl_display, frame_sync);
      }
      return texture_gl_fail(
          self,
          error,
          7,
          "render",
          "eglMakeCurrent failed while restoring Flutter TextureGL context: "
          "0x%x",
          restore_error);
    }
    texture_gl_log_frame_sync(
        self, g_strcmp0(read_sync_mode, "egl-fence") == 0 &&
                      g_strcmp0(frame_sync_mode, "egl-fence") == 0
                  ? "egl-fence-bidirectional" : "gl-finish");
    EGLint wait_result = EGL_FALSE;
    EGLint wait_error = EGL_SUCCESS;
    if (!texture_gl_wait_frame_sync(egl_display,
                                    frame_sync,
                                    &wait_result,
                                    &wait_error)) {
      return texture_gl_fail(self,
                             error,
                             8,
                             "render",
                             "EGL frame sync wait failed: result=0x%x "
                             "error=0x%x",
                             wait_result,
                             wait_error);
    }
  }

  if (notify_texture_update) {
    video_output_notify_texture_update(video_output);
  }
  
  *target = GL_TEXTURE_2D;
  *name = self->name;
  *width = self->current_width;
  *height = self->current_height;
  
  if (self->name == 0) {
    // First frame not yet available - create dummy texture in Flutter's context.
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGenTextures(1, &self->name);
    glBindTexture(GL_TEXTURE_2D, self->name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, previous_texture);
    *name = self->name;
    *width = 1;
    *height = 1;
  }
  
  return TRUE;
}