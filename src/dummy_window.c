#include "dummy_window.h"

#include "window.h"
#include "window_private.h"

static const struct window_ops dummy_window_ops;

MUST_CHECK struct window *dummy_window_new(
    // clang-format off
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer,
    struct vec2i size,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    double refresh_rate
    // clang-format on
) {
    struct window *window;

    window = malloc(sizeof *window);
    if (window == NULL) {
        return NULL;
    }

    window_init(
        // clang-format off
        window,
        tracer,
        scheduler,
        false, PLANE_TRANSFORM_NONE,
        false, kLandscapeLeft,
        size.x, size.y,
        has_explicit_dimensions, width_mm, height_mm,
        refresh_rate,
        false, PIXFMT_RGB565,
        renderer_type,
        gl_renderer,
        vk_renderer
        // clang-format on
    );

    window->renderer_type = renderer_type;
    if (gl_renderer != NULL) {
#ifdef HAVE_EGL_GLES2
        window->gl_renderer = gl_renderer_ref(gl_renderer);
#else
        UNREACHABLE();
#endif
    }
    if (vk_renderer != NULL) {
#ifdef HAVE_VULKAN
        window->vk_renderer = vk_renderer_ref(vk_renderer);
#else
        UNREACHABLE();
#endif
    } else {
        window->vk_renderer = NULL;
    }
    window->ops = dummy_window_ops;
    return window;
}

static int dummy_window_push_composition(struct window *window, struct fl_layer_composition *composition) {
    (void) window;
    (void) composition;
    /// TODO: Maybe allow to export the layer composition as an image, for testing purposes.
    return 0;
}

static struct render_surface *dummy_window_get_render_surface_internal(struct window *window, bool has_size, UNUSED struct vec2i size) {
    struct render_surface *render_surface;

    ASSERT_NOT_NULL(window);

    if (!has_size) {
        size = vec2f_round_to_integer(window->view_size);
    }

    if (window->render_surface != NULL) {
        return window->render_surface;
    }

    if (window->renderer_type == kOpenGL_RendererType) {
        // opengl
#ifdef HAVE_EGL_GLES2
    // EGL_NO_CONFIG_KHR is defined by EGL_KHR_no_config_context.
    #ifndef EGL_KHR_no_config_context
        #error "EGL header definitions for extension EGL_KHR_no_config_context are required."
    #endif

        struct egl_gbm_render_surface *egl_surface = egl_gbm_render_surface_new_with_egl_config(
            window->tracer,
            size,
            gl_renderer_get_gbm_device(window->gl_renderer),
            window->gl_renderer,
            window->has_forced_pixel_format ? window->forced_pixel_format : PIXFMT_ARGB8888,
            EGL_NO_CONFIG_KHR,
            NULL,
            0
        );
        if (egl_surface == NULL) {
            LOG_ERROR("Couldn't create EGL GBM rendering surface.\n");
            render_surface = NULL;
        } else {
            render_surface = CAST_RENDER_SURFACE(egl_surface);
        }

#else
        UNREACHABLE();
#endif
    } else {
        ASSUME(window->renderer_type == kVulkan_RendererType);

        // vulkan
#ifdef HAVE_VULKAN
        UNIMPLEMENTED();
#else
        UNREACHABLE();
#endif
    }

    window->render_surface = render_surface;
    return render_surface;
}

static struct render_surface *dummy_window_get_render_surface(struct window *window, struct vec2i size) {
    ASSERT_NOT_NULL(window);
    return dummy_window_get_render_surface_internal(window, true, size);
}

#ifdef HAVE_EGL_GLES2
static bool dummy_window_has_egl_surface(struct window *window) {
    ASSERT_NOT_NULL(window);

    if (window->renderer_type == kOpenGL_RendererType) {
        return window->render_surface != NULL;
    } else {
        return false;
    }
}

static EGLSurface dummy_window_get_egl_surface(struct window *window) {
    ASSERT_NOT_NULL(window);

    if (window->renderer_type == kOpenGL_RendererType) {
        struct render_surface *render_surface = dummy_window_get_render_surface_internal(window, false, VEC2I(0, 0));
        return egl_gbm_render_surface_get_egl_surface(CAST_EGL_GBM_RENDER_SURFACE(render_surface));
    } else {
        return EGL_NO_SURFACE;
    }
}
#endif

static void dummy_window_deinit(struct window *window) {
    ASSERT_NOT_NULL(window);

    if (window->render_surface != NULL) {
        surface_unref(CAST_SURFACE(window->render_surface));
    }

    if (window->gl_renderer != NULL) {
#ifdef HAVE_EGL_GLES2
        gl_renderer_unref(window->gl_renderer);
#else
        UNREACHABLE();
#endif
    }

    if (window->vk_renderer != NULL) {
#ifdef HAVE_VULKAN
        vk_renderer_unref(window->vk_renderer);
#else
        UNREACHABLE();
#endif
    }

    window_deinit(window);
}

static int dummy_window_set_cursor_locked(
    // clang-format off
    struct window *window,
    bool has_enabled, bool enabled,
    bool has_kind, enum pointer_kind kind,
    bool has_pos, struct vec2i pos
    // clang-format on
) {
    ASSERT_NOT_NULL(window);

    (void) window;
    (void) has_enabled;
    (void) enabled;
    (void) has_kind;
    (void) kind;
    (void) has_pos;
    (void) pos;

    return 0;
}

static const struct window_ops dummy_window_ops = {
    .deinit = dummy_window_deinit,
    .push_composition = dummy_window_push_composition,
    .get_render_surface = dummy_window_get_render_surface,
#ifdef HAVE_EGL_GLES2
    .has_egl_surface = dummy_window_has_egl_surface,
    .get_egl_surface = dummy_window_get_egl_surface,
#endif
    .set_cursor_locked = dummy_window_set_cursor_locked,
};
