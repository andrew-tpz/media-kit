/* Run from media_kit_video/linux:
 g++ -std=c++17 -Iinclude test/scoped_egl_context_test.cc \
   $(pkg-config --cflags --libs glib-2.0 epoxy) -o /tmp/scoped_egl_context_test
 /tmp/scoped_egl_context_test

*/

#include "media_kit_video/scoped_egl_context.h"

#include <cstdint>

static GRecMutex mutex;
void video_output_lock_egl() { g_rec_mutex_lock(&mutex); }
void video_output_unlock_egl() { g_rec_mutex_unlock(&mutex); }

template <typename T>
static T handle(uintptr_t value) {
  return reinterpret_cast<T>(value);
}

struct State {
  EGLenum api = EGL_OPENGL_API;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface draw = EGL_NO_SURFACE;
  EGLSurface read = EGL_NO_SURFACE;
  GLXContext glx = nullptr;
  Display* glx_display = nullptr;
  GLXDrawable glx_draw = None;
  GLXDrawable glx_read = None;
  bool fail_egl = false;
  bool fail_glx = false;
  bool fail_api = false;
  int glx_queries = 0;
};
static State state;

static EGLBoolean bind_api(EGLenum api) {
  if (state.fail_api) return EGL_FALSE;
  state.api = api;
  return EGL_TRUE;
}

static EGLBoolean make_current(EGLDisplay display, EGLSurface draw,
                               EGLSurface read, EGLContext context) {
  // This is libglvnd's GLX/EGL exclusion, independent of context ownership.
  if (state.glx != nullptr || state.fail_egl) return EGL_FALSE;
  state.context = context;
  state.display = context ? display : EGL_NO_DISPLAY;
  state.draw = draw;
  state.read = read;
  return EGL_TRUE;
}

static Bool glx_make_current(Display* display, GLXDrawable draw,
                             GLXDrawable read, GLXContext context) {
  if (state.context != EGL_NO_CONTEXT || state.fail_glx) return False;
  state.glx = context;
  state.glx_display = context ? display : nullptr;
  state.glx_draw = draw;
  state.glx_read = read;
  return True;
}

static void set_glx() {
  state = State{};
  state.glx = handle<GLXContext>(10);
  state.glx_display = handle<Display*>(11);
  state.glx_draw = 12;
  state.glx_read = 13;
}

static void assert_glx_restored() {
  g_assert_true(state.glx == handle<GLXContext>(10));
  g_assert_true(state.glx_display == handle<Display*>(11));
  g_assert_cmpuint(state.glx_draw, ==, 12);
  g_assert_cmpuint(state.glx_read, ==, 13);
  g_assert_true(state.context == EGL_NO_CONTEXT);
  g_assert_cmpuint(state.api, ==, EGL_OPENGL_API);
}

static bool bind_mpv(ScopedEglContext& scope, uintptr_t id = 1) {
  return scope.MakeCurrent(handle<EGLDisplay>(2), handle<EGLSurface>(3),
                            handle<EGLContext>(id));
}

static void test_glx_two_outputs() {
  set_glx();
  // The old path fails repeatedly while GTK's GLX context remains current.
  for (int i = 0; i < 80; ++i) {
    g_assert_false(eglMakeCurrent(handle<EGLDisplay>(2), handle<EGLSurface>(3),
                                  handle<EGLSurface>(3), handle<EGLContext>(1)));
  }
  for (uintptr_t id = 1; id <= 2; ++id) {
    {
      ScopedEglContext scope(true);
      g_assert_cmpstr(scope.HostContextType(), ==, "glx");
      g_assert_true(bind_mpv(scope, id));
      g_assert_null(state.glx);
      g_assert_true(state.context == handle<EGLContext>(id));
      g_assert_cmpuint(state.api, ==, EGL_OPENGL_ES_API);
    }
    assert_glx_restored();
  }
}

static void test_egl() {
  state = State{};
  state.display = handle<EGLDisplay>(20);
  state.context = handle<EGLContext>(21);
  state.draw = handle<EGLSurface>(22);
  state.read = handle<EGLSurface>(23);
  {
    ScopedEglContext scope;
    g_assert_cmpstr(scope.HostContextType(), ==, "egl");
    for (int i = 0; i < 2; ++i) {
      g_assert_true(bind_mpv(scope));
      g_assert_true(scope.Restore());
      g_assert_true(state.display == handle<EGLDisplay>(20));
      g_assert_true(state.context == handle<EGLContext>(21));
      g_assert_true(state.draw == handle<EGLSurface>(22));
      g_assert_true(state.read == handle<EGLSurface>(23));
      g_assert_cmpuint(state.api, ==, EGL_OPENGL_API);
    }
  }
  g_assert_cmpint(state.glx_queries, ==, 0);
}

static void test_no_context() {
  state = State{};
  {
    ScopedEglContext scope;
    g_assert_cmpstr(scope.HostContextType(), ==, "none");
    g_assert_true(bind_mpv(scope));
  }
  g_assert_true(state.context == EGL_NO_CONTEXT);
  g_assert_true(state.display == EGL_NO_DISPLAY);
  g_assert_cmpuint(state.api, ==, EGL_OPENGL_API);
  g_assert_cmpint(state.glx_queries, ==, 0);
}

static void test_failed_bind() {
  set_glx();
  {
    ScopedEglContext scope(true);
    state.fail_egl = true;
    g_assert_false(bind_mpv(scope));
  }
  assert_glx_restored();
}

static void test_failed_glx_release() {
  set_glx();
  {
    ScopedEglContext scope(true);
    state.fail_glx = true;
    g_assert_false(bind_mpv(scope));
  }
  assert_glx_restored();
}

static void test_failed_api_bind() {
  set_glx();
  {
    ScopedEglContext scope(true);
    state.fail_api = true;
    g_assert_false(bind_mpv(scope));
    state.fail_api = false;
  }
  assert_glx_restored();
}

static void test_early_init_failure() {
  set_glx();
  {
    ScopedEglContext scope(true);
    g_assert_true(eglBindAPI(EGL_OPENGL_ES_API));
    // For example, eglChooseConfig fails before MakeCurrent is attempted.
  }
  assert_glx_restored();
}

static void test_nested_cleanup() {
  set_glx();
  {
    ScopedEglContext outer(true);
    g_assert_true(bind_mpv(outer));
    {
      ScopedEglContext inner(true);
      g_assert_true(bind_mpv(inner));
    }
    g_assert_true(state.context == handle<EGLContext>(1));
    g_assert_true(outer.Restore());
    assert_glx_restored();
  }
  assert_glx_restored();
}

static void test_restore_failure_retry() {
  set_glx();
  {
    ScopedEglContext scope(true);
    g_assert_true(bind_mpv(scope));
    state.fail_glx = true;
    g_assert_false(scope.Restore());
    g_assert_true(state.context == EGL_NO_CONTEXT);
    state.fail_glx = false;
  }
  assert_glx_restored();
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  epoxy_eglQueryAPI = []() { return state.api; };
  epoxy_eglBindAPI = bind_api;
  epoxy_eglGetCurrentDisplay = []() { return state.display; };
  epoxy_eglGetCurrentContext = []() { return state.context; };
  epoxy_eglGetCurrentSurface = [](EGLint which) {
    return which == EGL_DRAW ? state.draw : state.read;
  };
  epoxy_eglMakeCurrent = make_current;
  epoxy_glXGetCurrentContext = []() {
    ++state.glx_queries;
    return state.glx;
  };
  epoxy_glXGetCurrentDisplay = []() { return state.glx_display; };
  epoxy_glXGetCurrentDrawable = []() { return state.glx_draw; };
  epoxy_glXGetCurrentReadDrawable = []() { return state.glx_read; };
  epoxy_glXMakeContextCurrent = glx_make_current;
  g_test_add_func("/egl/glx-two-outputs", test_glx_two_outputs);
  g_test_add_func("/egl/flutter-context", test_egl);
  g_test_add_func("/egl/no-context", test_no_context);
  g_test_add_func("/egl/failed-bind", test_failed_bind);
  g_test_add_func("/egl/failed-glx-release", test_failed_glx_release);
  g_test_add_func("/egl/failed-api-bind", test_failed_api_bind);
  g_test_add_func("/egl/early-init-failure", test_early_init_failure);
  g_test_add_func("/egl/nested-cleanup", test_nested_cleanup);
  g_test_add_func("/egl/restore-failure-retry", test_restore_failure_retry);
  return g_test_run();
}
