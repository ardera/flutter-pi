// SPDX-License-Identifier: MIT
/*
 * GBM Surface Backing Stores
 *
 * - implements EGL/GL render surfaces using a gbm surface
 * - ideal way to create render surfaces right now
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#include "egl_offscreen_render_surface.h"

#include <errno.h>
#include <stdlib.h>

#include "egl.h"
#include "gl_renderer.h"
#include "gles.h"
#include "modesetting.h"
#include "pixel_format.h"
#include "render_surface.h"
#include "render_surface_private.h"
#include "surface.h"
#include "tracer.h"
#include "util/collection.h"
#include "util/logging.h"
#include "util/refcounting.h"

struct egl_offscreen_render_surface;

struct egl_offscreen_render_surface {
    union {
        struct surface surface;
        struct render_surface render_surface;
    };

#ifdef DEBUG
    uuid_t uuid;
#endif

    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    struct gl_renderer *renderer;
};

COMPILE_ASSERT(offsetof(struct egl_offscreen_render_surface, surface) == 0);
COMPILE_ASSERT(offsetof(struct egl_offscreen_render_surface, render_surface.surface) == 0);

#ifdef DEBUG
static const uuid_t uuid = CONST_UUID(0xf9, 0xab, 0x5d, 0xad, 0x2e, 0x3b, 0x4e, 0x2c, 0x9d, 0x26, 0x64, 0x70, 0xfa, 0x9a, 0x25, 0xab);
#endif

#define CAST_THIS(ptr) CAST_EGL_OFFSCREEN_RENDER_SURFACE(ptr)
#define CAST_THIS_UNCHECKED(ptr) CAST_EGL_OFFSCREEN_RENDER_SURFACE_UNCHECKED(ptr)

#ifdef DEBUG
ATTR_PURE struct egl_offscreen_render_surface *__checked_cast_egl_offscreen_render_surface(void *ptr) {
    struct egl_offscreen_render_surface *s;

    s = CAST_EGL_OFFSCREEN_RENDER_SURFACE_UNCHECKED(ptr);
    assert(uuid_equals(s->uuid, uuid));
    return s;
}
#endif

void egl_offscreen_render_surface_deinit(struct surface *s);
static int egl_offscreen_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder);
static int
egl_offscreen_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder);
static int egl_offscreen_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store);
static int egl_offscreen_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store);

static int egl_offscreen_render_surface_init(
    struct egl_offscreen_render_surface *s,
    struct tracer *tracer,
    struct vec2i size,
    struct gl_renderer *renderer
) {
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLBoolean egl_ok;
    EGLConfig egl_config;
    int ok;

    ASSERT_NOT_NULL(renderer);
    egl_display = gl_renderer_get_egl_display(renderer);
    ASSERT_NOT_NULL(egl_display);

    /// TODO: Think about allowing different tilings / modifiers here
    // choose a config
    egl_config = gl_renderer_choose_pbuffer_config(renderer, 8, 8, 8, 8);
    if (egl_config == EGL_NO_CONFIG_KHR) {
        LOG_ERROR(
            "EGL doesn't supported the hardcoded software rendering pixel format ARGB8888.\n"
        );
        return EINVAL;
    }

// EGLAttribKHR is defined by EGL_KHR_cl_event2.
#ifndef EGL_KHR_cl_event2
    #error "EGL header definitions for extension EGL_KHR_cl_event2 are required."
#endif

    static const EGLAttribKHR surface_attribs[] = {
        /* EGL_GL_COLORSPACE, GL_LINEAR / GL_SRGB */
        /* EGL_RENDER_BUFFER, EGL_BACK_BUFFER / EGL_SINGLE_BUFFER */
        /* EGL_VG_ALPHA_FORMAT, EGL_VG_ALPHA_FORMAT_NONPRE / EGL_VG_ALPHA_FORMAT_PRE */
        /* EGL_VG_COLORSPACE, EGL_VG_COLORSPACE_sRGB / EGL_VG_COLORSPACE_LINEAR */
        EGL_NONE,
    };

    (void) surface_attribs;

    egl_ok = eglBindAPI(EGL_OPENGL_ES_API);
    if (egl_ok == EGL_FALSE) {
        LOG_EGL_ERROR(eglGetError(), "Couldn't bind OpenGL ES API to EGL. eglBindAPI");
        return EIO;
    }

    egl_surface = gl_renderer_create_pbuffer_surface(renderer, egl_config, NULL, NULL);
    if (egl_surface == EGL_NO_SURFACE) {
        return EIO;
    }

    /// TODO: Implement
    ok = render_surface_init(CAST_RENDER_SURFACE_UNCHECKED(s), tracer, size);
    if (ok != 0) {
        goto fail_destroy_egl_surface;
    }

    s->surface.present_kms = egl_offscreen_render_surface_present_kms;
    s->surface.present_fbdev = egl_offscreen_render_surface_present_fbdev;
    s->surface.deinit = egl_offscreen_render_surface_deinit;
    s->render_surface.fill = egl_offscreen_render_surface_fill;
    s->render_surface.queue_present = egl_offscreen_render_surface_queue_present;
#ifdef DEBUG
    uuid_copy(&s->uuid, uuid);
#endif
    s->egl_display = egl_display;
    s->egl_surface = egl_surface;
    s->egl_config = egl_config;
    s->renderer = gl_renderer_ref(renderer);
    return 0;

fail_destroy_egl_surface:
    eglDestroySurface(egl_display, egl_surface);

    return ok;
}

/**
 * @brief Create a new gbm_surface based render surface, with an explicit EGL Config for the created EGLSurface.
 *
 * @param compositor The compositor that this surface will be registered to when calling surface_register.
 * @param size The size of the surface.
 * @param renderer The EGL/OpenGL used to create any GL surfaces.
 * @return struct egl_offscreen_render_surface*
 */
struct egl_offscreen_render_surface *egl_offscreen_render_surface_new(
    struct tracer *tracer,
    struct vec2i size,
    struct gl_renderer *renderer
) {
    struct egl_offscreen_render_surface *surface;
    int ok;

    surface = malloc(sizeof *surface);
    if (surface == NULL) {
        goto fail_return_null;
    }

    ok = egl_offscreen_render_surface_init(surface, tracer, size, renderer);
    if (ok != 0) {
        goto fail_free_surface;
    }

    return surface;

fail_free_surface:
    free(surface);

fail_return_null:
    return NULL;
}

void egl_offscreen_render_surface_deinit(struct surface *s) {
    struct egl_offscreen_render_surface *egl_surface;

    egl_surface = CAST_THIS(s);

    gl_renderer_unref(egl_surface->renderer);
    render_surface_deinit(s);
}

static int egl_offscreen_render_surface_present_kms(struct surface *s, const struct fl_layer_props *props, struct kms_req_builder *builder) {
    (void) s;
    (void) props;
    (void) builder;

    UNIMPLEMENTED();

    return 0;
}

static int
egl_offscreen_render_surface_present_fbdev(struct surface *s, const struct fl_layer_props *props, struct fbdev_commit_builder *builder) {
    struct egl_offscreen_render_surface *egl_surface;

    /// TODO: Implement by mmapping the current front bo, copy it into the fbdev
    /// TODO: Print a warning here if we're not using explicit linear tiling and use glReadPixels instead of gbm_bo_map in that case

    egl_surface = CAST_THIS(s);
    (void) egl_surface;
    (void) props;
    (void) builder;

    UNIMPLEMENTED();

    return 0;
}

static int egl_offscreen_render_surface_fill(struct render_surface *s, FlutterBackingStore *fl_store) {
    fl_store->type = kFlutterBackingStoreTypeOpenGL;
    fl_store->open_gl = (FlutterOpenGLBackingStore) {
        .type = kFlutterOpenGLTargetTypeFramebuffer,
        .framebuffer = {
            /* for some reason flutter wants this to be GL_BGRA8_EXT, contrary to what the docs say */
            .target = GL_BGRA8_EXT,

            /* 0 refers to the window surface, instead of to an FBO */
            .name = 0,

            /*
             * even though the compositor will call surface_ref too to fill the FlutterBackingStore.user_data,
             * we need to ref two times because flutter will call both this destruction callback and the
             * compositor collect callback
             */
            .user_data = surface_ref(CAST_SURFACE_UNCHECKED(s)),
            .destruction_callback = surface_unref_void,
        },
    };
    return 0;
}

static int egl_offscreen_render_surface_queue_present(struct render_surface *s, const FlutterBackingStore *fl_store) {
    (void) s;
    (void) fl_store;

    // nothing to do here

    return 0;
}

/**
 * @brief Get the EGL Surface for rendering into this render surface.
 *
 * Flutter doesn't really support backing stores to be EGL Surfaces, so we have to hack around this, kinda.
 *
 * @param s
 * @return EGLSurface The EGLSurface associated with this render surface. Only valid for the lifetime of this egl_offscreen_render_surface.
 */
ATTR_PURE EGLSurface egl_offscreen_render_surface_get_egl_surface(struct egl_offscreen_render_surface *s) {
    return s->egl_surface;
}

/**
 * @brief Get the EGLConfig that was used to create the EGLSurface for this render surface.
 *
 * If the display doesn't support EGL_KHR_no_config_context, we need to create the EGL rendering context with
 * the same EGLConfig as every EGLSurface we want to bind to it. So we can just let egl_offscreen_render_surface choose a config
 * and let flutter-pi query that config when creating the rendering contexts in that case.
 *
 * @param s
 * @return EGLConfig The chosen EGLConfig. Valid forever.
 */
ATTR_PURE EGLConfig egl_offscreen_render_surface_get_egl_config(struct egl_offscreen_render_surface *s) {
    return s->egl_config;
}
