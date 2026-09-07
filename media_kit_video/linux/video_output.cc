// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.

#include "include/media_kit_video/video_output.h"
#include "include/media_kit_video/scoped_egl_context.h"
#include "include/media_kit_video/texture_gl.h"
#include "include/media_kit_video/texture_sw.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <epoxy/glx.h>
#include <gdk/gdkwayland.h>
#include <gdk/gdkx.h>

#include <string>

#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif

#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif

struct _VideoOutput {
  GObject parent_instance;
  TextureGL* texture_gl;
  gboolean texture_gl_registered;
  EGLDisplay egl_display; /* EGL display for mpv rendering. */
  EGLContext egl_context; /* Isolated EGL context (non-shared). */
  EGLSurface egl_surface; /* Pbuffer surface for activating EGL context. */
  guint8* pixel_buffer;
  TextureSW* texture_sw;
  GMutex mutex; /* Only used in S/W rendering. */
  mpv_handle* handle;
  mpv_render_context* render_context;
  gint64 width;
  gint64 height;
  VideoOutputConfiguration configuration;
  TextureUpdateCallback texture_update_callback;
  gpointer texture_update_callback_context;
  FlTextureRegistrar* texture_registrar;
  gboolean destroyed;
};

G_DEFINE_TYPE(VideoOutput, video_output, G_TYPE_OBJECT)

static GRecMutex media_kit_video_egl_mutex;

void video_output_lock_egl() {
  g_rec_mutex_lock(&media_kit_video_egl_mutex);
}

void video_output_unlock_egl() {
  g_rec_mutex_unlock(&media_kit_video_egl_mutex);
}

static void video_output_dispose(GObject* object) {
  VideoOutput* self = VIDEO_OUTPUT(object);
  self->destroyed = TRUE;
  
  // Make sure that no more callbacks are invoked from mpv.
  if (self->render_context) {
    mpv_render_context_set_update_callback(self->render_context, NULL, NULL);
  }

  // H/W
  if (self->texture_gl) {
    if (self->texture_gl_registered) {
      fl_texture_registrar_unregister_texture(self->texture_registrar,
                                              FL_TEXTURE(self->texture_gl));
      self->texture_gl_registered = FALSE;
    }

    ScopedEglContext egl_scope(
        GDK_IS_X11_DISPLAY(gdk_display_get_default()));

    gboolean egl_context_current = FALSE;
    if (self->egl_display != EGL_NO_DISPLAY &&
        self->egl_context != EGL_NO_CONTEXT &&
        self->egl_surface != EGL_NO_SURFACE) {
      egl_context_current =
          egl_scope.MakeCurrent(self->egl_display, self->egl_surface,
                                self->egl_context);
      if (!egl_context_current) {
        g_printerr(
            "media_kit: VideoOutput: failed to make EGL context current "
            "while disposing GPU renderer: 0x%x\n",
            eglGetError());
      }
    }

    if (egl_context_current && self->render_context != NULL) {
      mpv_render_context_free(self->render_context);
      self->render_context = NULL;
    }

    g_object_unref(self->texture_gl);
    self->texture_gl = NULL;

    egl_scope.Restore();

    if (self->egl_surface != EGL_NO_SURFACE) {
      eglDestroySurface(self->egl_display, self->egl_surface);
      self->egl_surface = EGL_NO_SURFACE;
    }

    if (self->egl_context != EGL_NO_CONTEXT) {
      eglDestroyContext(self->egl_display, self->egl_context);
      self->egl_context = EGL_NO_CONTEXT;
    }
  }
  // S/W
  if (self->texture_sw) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(self->texture_sw));
    g_free(self->pixel_buffer);
    g_object_unref(self->texture_sw);
    if (self->render_context != NULL) {
      mpv_render_context_free(self->render_context);
      self->render_context = NULL;
    }
  }
  
  g_mutex_clear(&self->mutex);
  G_OBJECT_CLASS(video_output_parent_class)->dispose(object);
}

static void video_output_class_init(VideoOutputClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = video_output_dispose;
}

static void video_output_init(VideoOutput* self) {
  self->texture_gl = NULL;
  self->texture_gl_registered = FALSE;
  self->egl_display = EGL_NO_DISPLAY;
  self->egl_context = EGL_NO_CONTEXT;
  self->egl_surface = EGL_NO_SURFACE;
  self->texture_sw = NULL;
  self->pixel_buffer = NULL;
  self->handle = NULL;
  self->render_context = NULL;
  self->width = 0;
  self->height = 0;
  self->configuration = VideoOutputConfiguration{};
  self->texture_update_callback = NULL;
  self->texture_update_callback_context = NULL;
  self->texture_registrar = NULL;
  self->destroyed = FALSE;
  g_mutex_init(&self->mutex);
}


static const gchar* egl_error_to_string(EGLint error) {
  switch (error) {
    case EGL_SUCCESS:
      return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
      return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
      return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
      return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
      return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG:
      return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT:
      return "EGL_BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE:
      return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
      return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH:
      return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP:
      return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
      return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER:
      return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE:
      return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST:
      return "EGL_CONTEXT_LOST";
    default:
      return "unknown EGL error";
  }
}

static std::string egl_error_message_from_code(const gchar* action,
                                               EGLint error) {
  gchar error_hex[16];
  g_snprintf(error_hex, sizeof(error_hex), "0x%x", error);
  return std::string(action) + " failed: " + egl_error_to_string(error) +
         " (" + error_hex + ")";
}

static std::string egl_error_message(const gchar* action) {
  return egl_error_message_from_code(action, eglGetError());
}

static const gchar* gdk_display_backend_name(GdkDisplay* display) {
  if (display == NULL) {
    return "none";
  }
  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    return "wayland";
  }
  if (GDK_IS_X11_DISPLAY(display)) {
    return "x11";
  }
  return G_OBJECT_TYPE_NAME(display);
}

typedef EGLDisplay (*MediaKitEglGetPlatformDisplayProc)(
    EGLenum platform,
    void* native_display,
    const EGLint* attrib_list);

static EGLDisplay get_egl_platform_display(EGLenum platform,
                                           gpointer native_display,
                                           std::string* details) {
  static gboolean initialized = FALSE;
  static MediaKitEglGetPlatformDisplayProc get_platform_display = NULL;

  if (!initialized) {
    get_platform_display =
        reinterpret_cast<MediaKitEglGetPlatformDisplayProc>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display == NULL) {
      get_platform_display =
          reinterpret_cast<MediaKitEglGetPlatformDisplayProc>(
              eglGetProcAddress("eglGetPlatformDisplay"));
    }
    initialized = TRUE;
  }

  if (get_platform_display == NULL) {
    if (details != NULL) {
      *details += " eglGetPlatformDisplay=unavailable";
    }
    return EGL_NO_DISPLAY;
  }

  EGLDisplay display = get_platform_display(platform, native_display, NULL);
  if (display == EGL_NO_DISPLAY && details != NULL) {
    *details += " eglGetPlatformDisplay=" + egl_error_message("call");
  }
  return display;
}

static EGLDisplay get_egl_display_for_gdk_display(GdkDisplay* display,
                                                  std::string* details) {
  if (display == NULL) {
    if (details != NULL) {
      *details = "no GDK display";
    }
    return EGL_NO_DISPLAY;
  }

  const gchar* display_name = gdk_display_get_name(display);
  const gchar* backend = gdk_display_backend_name(display);
  if (details != NULL) {
    *details = std::string("backend=") + backend +
               " name=" + (display_name == NULL ? "unknown" : display_name);
  }

  EGLDisplay egl_display = EGL_NO_DISPLAY;
  if (GDK_IS_WAYLAND_DISPLAY(display)) {
    gpointer native_display = gdk_wayland_display_get_wl_display(display);
    egl_display = get_egl_platform_display(EGL_PLATFORM_WAYLAND_KHR,
                                           native_display, details);
    if (egl_display == EGL_NO_DISPLAY) {
      egl_display =
          eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(native_display));
      if (details != NULL) {
        *details += " eglGetDisplay=fallback";
      }
    }
  } else if (GDK_IS_X11_DISPLAY(display)) {
    gpointer native_display = gdk_x11_display_get_xdisplay(display);
    egl_display = get_egl_platform_display(EGL_PLATFORM_X11_KHR,
                                           native_display, details);
    if (egl_display == EGL_NO_DISPLAY) {
      egl_display =
          eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(native_display));
      if (details != NULL) {
        *details += " eglGetDisplay=fallback";
      }
    }
  }

  return egl_display;
}

static gboolean choose_gles2_egl_config(EGLDisplay display,
                                        EGLConfig* config,
                                        std::string* fallback_reason) {
  const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 0,
      EGL_STENCIL_SIZE, 0,
      EGL_NONE,
  };

  EGLint config_count = 0;
  if (!eglChooseConfig(display, config_attribs, config, 1, &config_count)) {
    if (fallback_reason != NULL) {
      *fallback_reason = egl_error_message("eglChooseConfig");
    }
    return FALSE;
  }
  if (config_count <= 0 || *config == NULL) {
    if (fallback_reason != NULL) {
      *fallback_reason = "eglChooseConfig found no GLES2 RGBA pbuffer config";
    }
    return FALSE;
  }
  return TRUE;
}

static std::string get_current_gl_renderer() {
  const GLubyte* renderer = glGetString(GL_RENDERER);
  return renderer == NULL ? "unknown" :
                            reinterpret_cast<const gchar*>(renderer);
}

VideoOutput* video_output_new(FlTextureRegistrar* texture_registrar,
                              FlView* view,
                              gint64 handle,
                              VideoOutputConfiguration configuration) {
  (void)view;
  VideoOutput* self = VIDEO_OUTPUT(g_object_new(video_output_get_type(), NULL));
  self->texture_registrar = texture_registrar;
  self->handle = (mpv_handle*)handle;
  self->width = configuration.width;
  self->height = configuration.height;
  self->configuration = configuration;
#ifndef MPV_RENDER_API_TYPE_SW
  // MPV_RENDER_API_TYPE_SW must be available for S/W rendering.
  if (!self->configuration.enable_hardware_acceleration) {
    g_printerr("media_kit: VideoOutput: S/W rendering is not supported.\n");
  }
  self->configuration.enable_hardware_acceleration = TRUE;
#endif
  mpv_set_option_string(self->handle, "video-sync", "audio");
  // Causes frame drops with `pulse` audio output. (SlotSun/dart_simple_live#42)
  // mpv_set_option_string(self->handle, "video-timing-offset", "0");
  gboolean hardware_acceleration_supported = FALSE;
  std::string fallback_reason;
  if (self->configuration.enable_hardware_acceleration) {
    GdkDisplay* display = gdk_display_get_default();
    ScopedEglContext egl_scope(GDK_IS_X11_DISPLAY(display));
    const gchar* backend = gdk_display_backend_name(display);
    std::string egl_display_details;
    self->egl_display =
        get_egl_display_for_gdk_display(display, &egl_display_details);

    if (self->egl_display == EGL_NO_DISPLAY) {
      fallback_reason =
          "could not obtain EGL display from X11/Wayland GDK display: " +
          egl_display_details;
    } else {
      EGLint egl_major = 0;
      EGLint egl_minor = 0;
      if (!eglInitialize(self->egl_display, &egl_major, &egl_minor)) {
        fallback_reason = egl_error_message("eglInitialize");
      } else if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fallback_reason = egl_error_message("eglBindAPI(EGL_OPENGL_ES_API)");
      } else {
        EGLConfig config = NULL;
        if (choose_gles2_egl_config(self->egl_display, &config,
                                    &fallback_reason)) {
          const EGLint context_attribs[] = {
              EGL_CONTEXT_CLIENT_VERSION, 2,
              EGL_NONE,
          };
          self->egl_context =
              eglCreateContext(self->egl_display, config, EGL_NO_CONTEXT,
                               context_attribs);
          if (self->egl_context == EGL_NO_CONTEXT) {
            fallback_reason = egl_error_message("eglCreateContext");
          } else {
            const EGLint pbuffer_attribs[] = {
                EGL_WIDTH, 1,
                EGL_HEIGHT, 1,
                EGL_NONE,
            };
            self->egl_surface =
                eglCreatePbufferSurface(self->egl_display, config,
                                        pbuffer_attribs);
            if (self->egl_surface == EGL_NO_SURFACE) {
              fallback_reason = egl_error_message("eglCreatePbufferSurface");
            } else {
              const gboolean egl_context_current =
                  egl_scope.MakeCurrent(self->egl_display, self->egl_surface,
                                        self->egl_context);
              if (!egl_context_current) {
                fallback_reason = egl_error_message("bind mpv EGL context");
              }
              if (egl_context_current) {
                if (!texture_gl_is_supported()) {
                  fallback_reason =
                      "required EGLImage/OpenGL ES interop extensions are "
                      "missing";
                } else {
                  const std::string gl_renderer = get_current_gl_renderer();
                  self->texture_gl = texture_gl_new(self);

                  if (fl_texture_registrar_register_texture(
                          texture_registrar, FL_TEXTURE(self->texture_gl))) {
                    self->texture_gl_registered = TRUE;

                    mpv_opengl_init_params gl_init_params{
                        [](auto, auto name) {
                          return (void*)eglGetProcAddress(name);
                        },
                        NULL,
                    };

                    mpv_render_param params[4] = {
                        {MPV_RENDER_PARAM_API_TYPE,
                         (void*)MPV_RENDER_API_TYPE_OPENGL},
                        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,
                         (void*)&gl_init_params},
                        {MPV_RENDER_PARAM_INVALID, (void*)0},
                        {MPV_RENDER_PARAM_INVALID, (void*)0},
                    };

                    // VAAPI needs the same native display that backs the EGL context.
                    const gchar* vaapi_display = "unavailable";
                    if (GDK_IS_WAYLAND_DISPLAY(display)) {
                      params[2].type = MPV_RENDER_PARAM_WL_DISPLAY;
                      params[2].data =
                          gdk_wayland_display_get_wl_display(display);
                      vaapi_display = "wayland";
                    } else if (GDK_IS_X11_DISPLAY(display)) {
                      params[2].type = MPV_RENDER_PARAM_X11_DISPLAY;
                      params[2].data = gdk_x11_display_get_xdisplay(display);
                      vaapi_display = "x11";
                    }

                    const int result = mpv_render_context_create(
                        &self->render_context, self->handle, params);
                    if (result == 0) {
                      mpv_render_context_set_update_callback(
                          self->render_context,
                          [](void* data) {
                            VideoOutput* self = (VideoOutput*)data;
                            if (self->destroyed) {
                              return;
                            }
                            fl_texture_registrar_mark_texture_frame_available(
                                self->texture_registrar,
                                FL_TEXTURE(self->texture_gl));
                          },
                          self);
                      hardware_acceleration_supported = TRUE;
                      g_print(
                          "media_kit: VideoOutput: HW render=gpu "
                          "player=%p api=libmpv/opengl texture-interop=egl-image "
                          "vaapi-display=%s backend=%s host-context=%s "
                          "egl=%d.%d gl=\"%s\" "
                          "cpu-copy=no\n",
                          (void*)self->handle,
                          vaapi_display,
                          backend,
                          egl_scope.HostContextType(),
                          egl_major,
                          egl_minor,
                          gl_renderer.c_str());
                    } else {
                      fallback_reason =
                          std::string("mpv_render_context_create failed: ") +
                          mpv_error_string(result);
                    }
                  } else {
                    fallback_reason =
                        "Flutter texture registrar rejected FlTextureGL";
                  }
                }
              }
              if (!egl_scope.Restore()) {
                g_printerr(
                    "media_kit: VideoOutput: failed to restore previous "
                    "EGL context after GPU renderer init player=%p "
                    "error=0x%x\n",
                    (void*)self->handle,
                    eglGetError());
              }
            }
          }
        }
      }
    }

    if (!hardware_acceleration_supported) {
      if (self->texture_gl_registered && self->texture_gl != NULL) {
        fl_texture_registrar_unregister_texture(texture_registrar,
                                                FL_TEXTURE(self->texture_gl));
        self->texture_gl_registered = FALSE;
      }
      if (self->texture_gl != NULL) {
        g_object_unref(self->texture_gl);
        self->texture_gl = NULL;
      }
      if (self->render_context != NULL) {
        mpv_render_context_free(self->render_context);
        self->render_context = NULL;
      }
      if (self->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(self->egl_display, self->egl_surface);
        self->egl_surface = EGL_NO_SURFACE;
      }
      if (self->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(self->egl_display, self->egl_context);
        self->egl_context = EGL_NO_CONTEXT;
      }
    }
  }
#ifdef MPV_RENDER_API_TYPE_SW
  if (!hardware_acceleration_supported) {
    if (fallback_reason.empty()) {
      fallback_reason =
          "hardware acceleration disabled by VideoControllerConfiguration";
    }
    g_printerr(
        "media_kit: VideoOutput: SW render=software api=libmpv/sw "
        "player=%p cpu-copy=yes reason=\"%s\"\n",
        (void*)self->handle,
        fallback_reason.c_str());
    // H/W rendering failed. Fallback to S/W rendering.
    self->pixel_buffer = g_new0(guint8, SW_RENDERING_PIXEL_BUFFER_SIZE);
    self->texture_gl = NULL;
    self->texture_sw = texture_sw_new(self);
    if (fl_texture_registrar_register_texture(texture_registrar,
                                              FL_TEXTURE(self->texture_sw))) {
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
          {MPV_RENDER_PARAM_INVALID, (void*)0},
      };
      if (mpv_render_context_create(&self->render_context, self->handle,
                                    params) == 0) {
        mpv_render_context_set_update_callback(
            self->render_context,
            [](void* data) {
              gdk_threads_add_idle(
                  [](gpointer data) -> gboolean {
                    VideoOutput* self = (VideoOutput*)data;
                    if (self->destroyed) {
                      return FALSE;
                    }
                    g_mutex_lock(&self->mutex);
                    gint64 width = video_output_get_width(self);
                    gint64 height = video_output_get_height(self);
                    if (width > 0 && height > 0) {
                      gint32 size[]{(gint32)width, (gint32)height};
                      gint32 pitch = 4 * (gint32)width;
                      mpv_render_param params[]{
                          {MPV_RENDER_PARAM_SW_SIZE, size},
                          {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgb0"},
                          {MPV_RENDER_PARAM_SW_STRIDE, &pitch},
                          {MPV_RENDER_PARAM_SW_POINTER, self->pixel_buffer},
                          {MPV_RENDER_PARAM_INVALID, (void*)0},
                      };
                      mpv_render_context_render(self->render_context, params);
                      fl_texture_registrar_mark_texture_frame_available(
                          self->texture_registrar,
                          FL_TEXTURE(self->texture_sw));
                    }
                    g_mutex_unlock(&self->mutex);
                    return FALSE;
                  },
                  data);
            },
            self);
      }
    }
  }
#endif
  return self;
}

void video_output_set_texture_update_callback(
    VideoOutput* self,
    TextureUpdateCallback texture_update_callback,
    gpointer texture_update_callback_context) {
  self->texture_update_callback = texture_update_callback;
  self->texture_update_callback_context = texture_update_callback_context;
  // Notify initial dimensions as (1, 1) if |width| & |height| are 0 i.e.
  // texture & video frame size is based on playing file's resolution. This
  // will make sure that `Texture` widget on Flutter's widget tree is actually
  // mounted & |fl_texture_registrar_mark_texture_frame_available| actually
  // invokes the |TextureGL| or |TextureSW| callbacks. Otherwise it will be a
  // never ending deadlock where no video frames are ever rendered.
  gint64 texture_id = video_output_get_texture_id(self);
  if (self->width == 0 || self->height == 0) {
    self->texture_update_callback(texture_id, 1, 1,
                                  self->texture_update_callback_context);
  } else {
    self->texture_update_callback(texture_id, self->width, self->height,
                                  self->texture_update_callback_context);
  }
}

void video_output_set_size(VideoOutput* self, gint64 width, gint64 height) {
  // Ideally, a mutex should be used here & |video_output_get_width| +
  // |video_output_get_height|. However, that is throwing everything into a
  // deadlock. Flutter itself seems to have some synchronization mechanism in
  // rendering & platform channels AFAIK.

  // H/W
  if (self->texture_gl) {
    self->width = width;
    self->height = height;
  }
  // S/W
  if (self->texture_sw) {
    self->width = CLAMP(width, 0, SW_RENDERING_MAX_WIDTH);
    self->height = CLAMP(height, 0, SW_RENDERING_MAX_HEIGHT);
  }
}

mpv_render_context* video_output_get_render_context(VideoOutput* self) {
  return self->render_context;
}

mpv_handle* video_output_get_handle(VideoOutput* self) {
  return self->handle;
}

EGLDisplay video_output_get_egl_display(VideoOutput* self) {
  return self->egl_display;
}

EGLContext video_output_get_egl_context(VideoOutput* self) {
  return self->egl_context;
}

EGLSurface video_output_get_egl_surface(VideoOutput* self) {
  return self->egl_surface;
}

guint8* video_output_get_pixel_buffer(VideoOutput* self) {
  return self->pixel_buffer;
}

gint64 video_output_get_width(VideoOutput* self) {
  // Fixed width.
  if (self->width) {
    return self->width;
  }

  // Video resolution dependent width.
  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect ratio.
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return SW_RENDERING_MAX_WIDTH;
    }
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return width / height * SW_RENDERING_MAX_HEIGHT;
    }
  }

  return width;
}

gint64 video_output_get_height(VideoOutput* self) {
  // Fixed height.
  if (self->width) {
    return self->height;
  }

  // Video resolution dependent height.
  gint64 width = 0;
  gint64 height = 0;

  mpv_node params;
  mpv_get_property(self->handle, "video-out-params", MPV_FORMAT_NODE, &params);

  int64_t dw = 0, dh = 0, rotate = 0;
  if (params.format == MPV_FORMAT_NODE_MAP) {
    for (int32_t i = 0; i < params.u.list->num; i++) {
      char* key = params.u.list->keys[i];
      auto value = params.u.list->values[i];
      if (value.format == MPV_FORMAT_INT64) {
        if (strcmp(key, "dw") == 0) {
          dw = value.u.int64;
        }
        if (strcmp(key, "dh") == 0) {
          dh = value.u.int64;
        }
        if (strcmp(key, "rotate") == 0) {
          rotate = value.u.int64;
        }
      }
    }
    mpv_free_node_contents(&params);
  }

  width = rotate == 0 || rotate == 180 ? dw : dh;
  height = rotate == 0 || rotate == 180 ? dh : dw;

  if (self->texture_sw != NULL) {
    // Make sure |width| & |height| fit between |SW_RENDERING_MAX_WIDTH| &
    // |SW_RENDERING_MAX_HEIGHT| while maintaining aspect ratio.
    if (height >= SW_RENDERING_MAX_HEIGHT) {
      return SW_RENDERING_MAX_HEIGHT;
    }
    if (width >= SW_RENDERING_MAX_WIDTH) {
      return height / width * SW_RENDERING_MAX_WIDTH;
    }
  }

  return height;
}

gint64 video_output_get_texture_id(VideoOutput* self) {
  // H/W
  if (self->texture_gl) {
    return (gint64)self->texture_gl;
  }
  // S/W
  if (self->texture_sw) {
    return (gint64)self->texture_sw;
  }
  g_assert_not_reached();
  return -1;
}

void video_output_notify_texture_update(VideoOutput* self) {
  gint64 id = video_output_get_texture_id(self);
  gint64 width = video_output_get_width(self);
  gint64 height = video_output_get_height(self);
  gpointer context = self->texture_update_callback_context;
  if (self->texture_update_callback != NULL) {
    self->texture_update_callback(id, width, height, context);
  }
}
