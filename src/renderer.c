#include <stdio.h>
#include <assert.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <flutter_embedder.h>

#include <collection.h>
#include <dylib_deps.h>
#include <renderer.h>

struct fbdev;

struct renderer {
    FlutterRendererType type;

    /**
     * @brief The drmdev for rendering (when using opengl renderer) and/or graphics output.
     */
    struct drmdev *drmdev;

    /**
     * @brief Optional fbdev for outputting the graphics there
     * instead of the drmdev.
     */
    struct fbdev *fbdev;

    union {
        struct {
            struct libegl *libegl;
            struct egl_client_info *client_info;

            /**
             * @brief The EGL display for this @ref drmdev.
             */
            EGLDisplay egl_display;

            struct egl_display_info *display_info;

            /**
             * @brief The GBM Surface backing the @ref egl_surface.
             * We need one regardless of whether we're outputting to @ref drmdev or @ref fbdev.
             * A gbm_surface has no concept of on-screen or off-screen, it's on-screen
             * when we decide to present its buffers via KMS and off-screen otherwise.
             */
            struct gbm_surface *gbm_surface;

            /**
             * @brief The root EGL surface. Can be single-buffered when we're outputting
             * to @ref fbdev since we're copying the buffer after present anyway.
             */
            EGLSurface egl_surface;

            /**
             * @brief Set of EGL contexts created by this renderer. All EGL contexts are created
             * the same so any context can be bound by any thread.
             */
            struct concurrent_pointer_set egl_contexts;

            /**
             * @brief Queue of EGL contexts available for use on any thread. Clients using this renderer
             * can also reserve an EGL context so they access it directly, without the renderer
             * having to lock/unlock this queue to find an unused context each time.
             */
            struct concurrent_queue unused_egl_contexts;

            EGLContext flutter_rendering_context, flutter_resource_context;
        } gl;
        struct {

        } sw;
    };
};

struct renderer *gl_renderer_new(
    struct drmdev *drmdev,
    struct libegl *libegl,
    struct egl_client_info *egl_client_info
) {
    struct renderer *renderer;

    renderer = malloc(sizeof *renderer);
    if (renderer == NULL) {
        return NULL;
    }

    renderer->type = kOpenGL;
    renderer->drmdev = drmdev;
    renderer->fbdev = NULL;

    /// TODO: Initialize

    if (renderer->gl.display_info->supports_14 == false) {
        LOG_RENDERER_ERROR("Flutter-pi requires EGL version 1.4 or newer, which is not supported by your system.\n");
        goto fail_free_renderer;
    }

    return renderer;


    fail_free_renderer:
    free(renderer);

    fail_return_null:
    return NULL;
}

static EGLContext create_egl_context(struct renderer *renderer) {
    /// TODO: Create EGL Context
    // cpset_put(&renderer->gl.egl_contexts, context);
}

static EGLContext get_unused_egl_context(struct renderer *renderer) {
    struct renderer_egl_context *context;
    int ok;
    
    ok = cqueue_try_dequeue(renderer, &context);
    if (ok == EAGAIN) {
        return create_egl_context(renderer);
    } else {
        return NULL;
    }

    return context;
}

static int put_unused_egl_context(struct renderer *renderer, EGLContext context) {
    return cqueue_enqueue(&renderer->gl.unused_egl_contexts, context);
}


int gl_renderer_make_current(struct renderer *renderer, bool surfaceless) {
    EGLContext context;
    EGLBoolean egl_ok;

    assert(renderer->type == kOpenGL);

    context = get_unused_egl_context(renderer);
    if (context == NULL) {
        return EINVAL;
    }

    egl_ok = eglMakeCurrent(
        renderer->gl.egl_display,
        surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
        surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
        context
    );
    if (egl_ok != EGL_TRUE) {
        LOG_RENDERER_ERROR("Could not make EGL context current.\n");
        return EINVAL;
    }

    return 0;
}

int gl_renderer_clear_current(struct renderer *renderer) {
    EGLContext context;
    EGLBoolean egl_ok;
    int ok;
    
    assert(renderer->type == kOpenGL);

    context = renderer->gl.libegl->eglGetCurrentContext();
    if (context == EGL_NO_CONTEXT) {
        LOG_RENDERER_ERROR("in gl_renderer_clear_current: No EGL context is current, so none can be cleared.\n");
        return EINVAL;
    }

    egl_ok = eglMakeCurrent(renderer->gl.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok != EGL_TRUE) {
        LOG_RENDERER_ERROR("Could not clear the current EGL context. eglMakeCurrent");
        return EIO;
    }

    ok = put_unused_egl_context(renderer, context);
    if (ok != 0) {
        LOG_RENDERER_ERROR("Could not mark the cleared EGL context as unused. put_unused_egl_context: %s\n", strerror(ok));
        return ok;
    }

    return 0;
}


EGLContext gl_renderer_reserve_context(struct renderer *renderer) {
    return get_unused_egl_context(renderer);
}

int gl_renderer_release_context(struct renderer *renderer, EGLContext context) {
    return put_unused_egl_context(renderer, context);
}

int gl_renderer_reserved_make_current(struct renderer *renderer, EGLContext context, bool surfaceless) {
    EGLBoolean egl_ok;

    assert(renderer->type == kOpenGL);

    egl_ok = eglMakeCurrent(
        renderer->gl.egl_display,
        surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
        surfaceless ? EGL_NO_SURFACE : renderer->gl.egl_surface,
        context
    );
    if (egl_ok != EGL_TRUE) {
        LOG_RENDERER_ERROR("Could not make EGL context current.\n");
        return EINVAL;
    }

    return 0;
}

int gl_renderer_reserved_clear_current(struct renderer *renderer) {
    EGLContext context;
    EGLBoolean egl_ok;
    int ok;
    
    assert(renderer->type == kOpenGL);

    egl_ok = eglMakeCurrent(renderer->gl.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_ok != EGL_TRUE) {
        LOG_RENDERER_ERROR("Could not clear the current EGL context. eglMakeCurrent");
        return EIO;
    }

    return 0;
}


int gl_renderer_make_flutter_renderering_context_current(struct renderer *renderer) {
    EGLContext context;
    int ok;

    context = renderer->gl.flutter_rendering_context;

    if (context == EGL_NO_CONTEXT) {
        context = get_unused_egl_context(renderer);
        if (context == EGL_NO_CONTEXT) {
            return EIO;
        }

        renderer->gl.flutter_rendering_context = context;
    }

    ok = gl_renderer_reserved_make_current(renderer, context, false);
    if (ok != 0) {
        put_unused_egl_context(renderer, context);
        return ok;
    }

    return 0;
}

int gl_renderer_make_flutter_resource_context_current(struct renderer *renderer) {
    EGLContext context;
    int ok;

    context = renderer->gl.flutter_resource_context;

    if (context == EGL_NO_CONTEXT) {
        context = get_unused_egl_context(renderer);
        if (context == EGL_NO_CONTEXT) {
            return EIO;
        }

        renderer->gl.flutter_resource_context = context;
    }

    ok = gl_renderer_reserved_make_current(renderer, context, false);
    if (ok != 0) {
        put_unused_egl_context(renderer, context);
        return ok;
    }

    return 0;
}

int gl_renderer_clear_flutter_context(struct renderer *renderer) {
    return gl_renderer_reserved_clear_current(renderer);
}


struct renderer *sw_renderer_new(
    struct drmdev *drmdev
) {
    struct renderer *renderer;

    renderer = malloc(sizeof *renderer);
    if (renderer == NULL) {
        return NULL;
    }

    renderer->type = kSoftware;

    return renderer;


    fail_free_renderer:
    free(renderer);

    fail_return_null:
    return NULL;
}


int renderer_fill_flutter_renderer_config(
    struct renderer *renderer,
    FlutterSoftwareRendererConfig *sw_dispatcher,
    FlutterOpenGLRendererConfig *gl_dispatcher,
    FlutterRendererConfig *config
) {
    config->type = renderer->type;
    if (config->type == kOpenGL) {
        config->open_gl = *gl_dispatcher;
    } else if (config->type = kSoftware) {
        config->software = *sw_dispatcher;
    }
}


